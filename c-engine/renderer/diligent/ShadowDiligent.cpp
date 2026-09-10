#include "renderer/diligent/ShadowDiligent.h"

#include "logger/Logger.h"
#include "renderer/RenderBackend.h"
#include "renderer/Renderer.h"
#include "renderer/diligent/DiligentRenderer.h"
#include "renderer/diligent/SplatTerrainDiligent.h"
#include "ecs/system/player/Player.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Sampler.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"
#include "Graphics/GraphicsTools/interface/GraphicsUtilities.h"
#include "Graphics/GraphicsTools/interface/MapHelper.hpp"
#include "Graphics/GraphicsTools/interface/ScopedDebugGroup.hpp"
#include "ShadowMapManager.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

// DiligentFX shadow cascade distribution + filtering. ShadowMapManager.hpp
// pulls BasicStructures.fxh into namespace Diligent (Diligent::LightAttribs,
// Diligent::ShadowMapAttribs) — the SAME header this TU hands to the passes
// as raw bytes (the passes see it as HLSL::LightAttribs from their own
// include; identical layout, exchanged as a memcpy).

/*
 * DiligentFX cascaded shadows for the terrain + props world passes
 * (DiligentFX/Components ShadowMapManager; the Shadows sample is the
 * integration reference).
 *
 * Conventions on this path (docs/lessons.md):
 *  - The camera is the render-space origin (rotation-only view, f64 anchor):
 *    DistributeCascades receives that rotation-only view, so cascade extents
 *    are computed around the anchor — exactly the space the geometry passes
 *    render in (positions minus the split anchor).
 *  - The master LightAttribs is consumed by RUNTIME-COMPILED HLSL (glslang)
 *    which reads cbuffer matrices transposed: DistributeCascades runs with
 *    PackMatrixRowMajor = false, so WriteShaderMatrix stores the transposed
 *    matrices directly. The ConvertToFilterable techniques bind their own
 *    internal attribs buffer and are unaffected by this choice.
 *
 * Settings: rendererGraphicsSettings().shadowMode (0=off, 1=PCF, 2=VSM,
 * 3=EVSM2, 4=EVSM4) and shadowQuality (0=low..2=high) map to
 * resolution/cascade-count/shadow-distance/filter-size (kQualityTiers).
 * Any change re-creates the shadow map and bumps the generation counter the
 * geometry passes poll to rebuild their sampling pipelines (the sampling
 * macros are compile-time).
 */

namespace engine::renderer::diligent {

    namespace {

        using Diligent::RefCntAutoPtr;
        using Diligent::ShadowMapManager;

        // Quality tier → (resolution, cascades, shadow distance m, PCF filter size).
        struct ShadowQualityTier {
            u32 resolution;
            u32 cascades;
            f32 distanceM;
            int pcfFilterSize;
        };

        constexpr ShadowQualityTier kQualityTiers[3] = {
            {1024, 2, 60.0f, 3},
            {2048, 2, 80.0f, 3},
            {2048, 3, 120.0f, 5},
        };

        bool passReady  = false;
        bool initFailed = false;
        u32 generation  = 0;
        int curMode     = -1;  // GraphicsSettings::shadowMode (0=off..4)
        int curQuality  = -1;  // GraphicsSettings::shadowQuality (0..2)

        ShadowMapManager mgr;
        RefCntAutoPtr<Diligent::ISampler> cmpSampler;
        RefCntAutoPtr<Diligent::ISampler> filterableSampler;
        Diligent::LightAttribs lightAttribs{};
        Diligent::float4x4 pbrW2L = Diligent::float4x4::Identity();
        int pbrSlice              = 0;
        float pbrBias             = 0.0f;
        bool pbrReady             = false;

        void createSamplers(void) {
            if (!cmpSampler) {
                Diligent::SamplerDesc desc;
                desc.Name           = "shadow map comparison sampler";
                desc.ComparisonFunc = Diligent::COMPARISON_FUNC_LESS;
                desc.MinFilter      = Diligent::FILTER_TYPE_COMPARISON_LINEAR;
                desc.MagFilter      = Diligent::FILTER_TYPE_COMPARISON_LINEAR;
                desc.MipFilter      = Diligent::FILTER_TYPE_COMPARISON_LINEAR;
                device->CreateSampler(desc, &cmpSampler);
            }
            if (!filterableSampler) {
                Diligent::SamplerDesc desc;
                desc.Name      = "shadow map filterable sampler";
                desc.MinFilter = Diligent::FILTER_TYPE_LINEAR;
                desc.MagFilter = Diligent::FILTER_TYPE_LINEAR;
                desc.MipFilter = Diligent::FILTER_TYPE_LINEAR;
                device->CreateSampler(desc, &filterableSampler);
            }
            if (!cmpSampler || !filterableSampler) {
                utils::warn("shadow: sampler creation failed");
                initFailed = true;
            }
        }

        // (Re)create the shadow map + conversion techniques for the current
        // mode/quality (the Shadows sample's CreateShadowMap).
        bool createShadowMap(int mode, const ShadowQualityTier& tier) {
            ShadowMapManager::InitInfo info;
            info.Format                      = Diligent::TEX_FORMAT_D32_FLOAT;
            info.Resolution                  = tier.resolution;
            info.NumCascades                 = tier.cascades;
            info.ShadowMode                  = mode;
            info.Is32BitFilterableFmt        = true;
            info.pComparisonSampler          = cmpSampler;
            info.pFilterableShadowMapSampler = filterableSampler;
            mgr.Initialize(device, nullptr, info);
            return true;
        }

        // Per-frame CPU work: settings → distribution → master LightAttribs.
        // The geometry passes memcpy the LightAttribs into their frame cbuffers
        // later in the frame (their fillFrameAttribs), so this must run before the
        // draws — it is called from draw() right before the world passes.
        void updateFrameImpl(void) {
            const auto& settings = engine::renderer::rendererGraphicsSettings();
            const int mode       = settings.shadowMode;
            const int quality    = settings.shadowQuality;

            const bool settingsChanged = mode != curMode || quality != curQuality;
            if (settingsChanged || (!passReady && mode != 0 && !initFailed)) {
                // Settings changed: rebuild (or tear down when mode == off).
                curMode    = mode;
                curQuality = quality;
                passReady  = false;
                if (mode == 0) {
                    generation++;
                    pbrReady = false;
                    utils::info("shadow: disabled");
                    return;
                }
                if (!device || initFailed) return;
                createSamplers();
                if (initFailed) return;
                const ShadowQualityTier& tier =
                    kQualityTiers[quality < 0 ? 0 : (quality > 2 ? 2 : quality)];
                if (!createShadowMap(mode, tier)) return;
                passReady = true;
                generation++;
                utils::info("shadow: mode %d quality %d (res %u, %u cascades, %.0f m, PCF %dx%d)",
                            mode,
                            quality,
                            tier.resolution,
                            tier.cascades,
                            (double)tier.distanceM,
                            tier.pcfFilterSize,
                            tier.pcfFilterSize);
            }
            if (!passReady) return;

            const int cq = curQuality < 0 ? 0 : (curQuality > 2 ? 2 : curQuality);
            const ShadowQualityTier& tier = kQualityTiers[cq];

            memset(&lightAttribs, 0, sizeof(lightAttribs));
            const f32* sunDir = diligentSunDirection();
            Diligent::float3 dir =
                Diligent::normalize(Diligent::float3{sunDir[0], sunDir[1], sunDir[2]});
            lightAttribs.f4Direction = Diligent::float4{dir.x, dir.y, dir.z, 0.0f};

            Diligent::ShadowMapAttribs& sa  = lightAttribs.ShadowAttribs;
            sa.iNumCascades                 = (int)tier.cascades;
            sa.fNumCascades                 = (float)tier.cascades;
            sa.iFixedFilterSize             = tier.pcfFilterSize;
            sa.fFilterWorldSize             = 0.1f;
            sa.fCascadeTransitionRegion     = 0.1f;
            sa.fReceiverPlaneDepthBiasClamp = 10.0f;
            sa.iMaxAnisotropy               = 4;
            sa.fVSMBias                     = 1e-4f;
            sa.fEVSMPositiveExponent        = 40.0f;
            sa.fEVSMNegativeExponent        = 5.0f;
            sa.bIs32BitEVSM                 = 1;

            // Distribute around the camera anchor: the view is rotation-only, so
            // the derived camera world position is the render-space origin. The
            // projection is the UNJITTERED one (stable cascades under TAA). The
            // whole range is clamped to the tier's shadow distance — cascades
            // spread logarithmically inside it. PackMatrixRowMajor = false stores
            // the TRANSPOSED matrices the runtime glslang shaders consume.
            const Diligent::float4x4& view = diligentFrameView();
            const Diligent::float4x4& proj = diligentBaseProj();

            static const float casterPad = [] {
                float v = 2.8f;
                if (const char* padEnv = getenv("ENGINE_SHADOW_CASTER_PAD")) {
                    const float parsed = (float)atof(padEnv);
                    if (parsed >= 1.0f && parsed <= 4.0f) v = parsed;
                }
                return v;
            }();
            Diligent::float4x4 padProj = proj;
            padProj._11 /= casterPad;
            padProj._22 /= casterPad;

            ShadowMapManager::DistributeCascadeInfo dist;
            dist.pCameraView                      = &view;
            dist.pCameraProj                      = &padProj;
            dist.pLightDir                        = &dir;
            dist.SnapCascades                     = true;
            dist.StabilizeExtents                 = true;
            dist.EqualizeExtents                  = true;
            dist.fPartitioningFactor              = 0.95f;
            dist.PackMatrixRowMajor               = false;
            dist.UseRightHandedLightViewTransform = true;
            dist.AdjustCascadeRange               = [&](int, float& minZ, float& maxZ) {
                if (minZ < engine::renderer::kCameraNear) minZ = engine::renderer::kCameraNear;
                if (maxZ > tier.distanceM) maxZ = tier.distanceM;
            };
            mgr.DistributeCascades(dist, lightAttribs.ShadowAttribs);

            // Depth bias per resolution (the Shadows sample's policy), normalized
            // to the cascade z range: FractionalSamplingError adds it verbatim in
            // NDC depth, so a fixed 0.005 is ~30 texels of cascade-0 depth here and
            // washes every comparison to lit. Scale by the light-space z scale.
            sa.fFixedDepthBias =
                (tier.resolution >= 2048 ? 0.0025f : 0.005f) * sa.Cascades[0].f4LightSpaceScale.z;

            // PBR (glTF) receiver: the player is one small receiver, so the
            // per-pixel cascade pick the runtime receivers get collapses to a
            // per-frame CPU pick: the cascade whose camera-space z range covers the
            // feet (render space, +1 m for the torso). Same untransposed matrix the
            // caster draws with (GetCascadeTransform), so receiver and caster agree.
            double ppos[3] = {0.0, 0.0, 0.0};
            if (engine::playerGetFootPos(ppos)) ppos[1] += 1.0;  // torso
            const Diligent::float3 pPos{(f32)ppos[0], (f32)ppos[1], (f32)ppos[2]};
            const float camZ = view._31 * pPos.x + view._32 * pPos.y + view._33 * pPos.z;
            int cascade      = 0;
            for (int c = 0; c < sa.iNumCascades; c++) {
                if (sa.fCascadeCamSpaceZEnd[c] < camZ) cascade = c + 1;
            }
            if (cascade >= sa.iNumCascades) cascade = sa.iNumCascades - 1;
            pbrW2L   = mgr.GetCascadeTransform((u32)cascade).WorldToLightProjSpace;
            pbrSlice = cascade;
            pbrBias  = sa.fFixedDepthBias;
            pbrReady = true;

            // One-shot CPU dump of the distributed cascade math (debug).
            static bool dumped = false;
            if (!dumped) {
                dumped                      = true;
                const Diligent::float4x4& m = sa.mWorldToLightView;
                utils::info(
                    "shadow dbg: W2L rows [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f "
                    "%.3f %.3f | %.3f %.3f %.3f %.3f]",
                    m._11,
                    m._12,
                    m._13,
                    m._14,
                    m._21,
                    m._22,
                    m._23,
                    m._24,
                    m._31,
                    m._32,
                    m._33,
                    m._34,
                    m._41,
                    m._42,
                    m._43,
                    m._44);
                for (u32 c = 0; c < (u32)sa.iNumCascades; c++) {
                    const auto& ca = sa.Cascades[c];
                    utils::info(
                        "shadow dbg: cascade %u scale (%.4f, %.4f, %.4f) bias (%.4f, %.4f, %.4f, "
                        "%.4f) zEnd %.2f",
                        c,
                        ca.f4LightSpaceScale.x,
                        ca.f4LightSpaceScale.y,
                        ca.f4LightSpaceScale.z,
                        ca.f4LightSpaceScaledBias.x,
                        ca.f4LightSpaceScaledBias.y,
                        ca.f4LightSpaceScaledBias.z,
                        ca.f4LightSpaceScaledBias.w,
                        sa.fCascadeCamSpaceZEnd[c]);
                }
                // UV of the render-space origin (the camera) in cascade 0.
                const Diligent::float3 org = Diligent::float3(0, 0, 0);
                const auto& ca0            = sa.Cascades[0];
                float nx = org.x * ca0.f4LightSpaceScale.x + ca0.f4LightSpaceScaledBias.x;
                float ny = org.y * ca0.f4LightSpaceScale.y + ca0.f4LightSpaceScaledBias.y;
                float nz = org.z * ca0.f4LightSpaceScale.z + ca0.f4LightSpaceScaledBias.z;
                utils::info(
                    "shadow dbg: origin NDC in cascade0 (%.4f, %.4f, %.4f) -> uv (%.4f, %.4f) "
                    "depth %.4f",
                    nx,
                    ny,
                    nz,
                    nx * 0.5f + 0.5f,
                    0.5f - ny * 0.5f,
                    nz * 0.5f + 0.5f);
                // Cross-check: the actual cascade proj (what the depth pass renders
                // with) vs the scale/bias the sampling path uses.
                const Diligent::float4x4& w2lp = mgr.GetCascadeTransform(0).WorldToLightProjSpace;
                utils::info(
                    "shadow dbg: W2LP0 [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f "
                    "%.4f | %.4f %.4f %.4f %.4f]",
                    w2lp._11,
                    w2lp._12,
                    w2lp._13,
                    w2lp._14,
                    w2lp._21,
                    w2lp._22,
                    w2lp._23,
                    w2lp._24,
                    w2lp._31,
                    w2lp._32,
                    w2lp._33,
                    w2lp._34,
                    w2lp._41,
                    w2lp._42,
                    w2lp._43,
                    w2lp._44);
            }
        }

        // GPU work: all cascades (depth draws through the geometry passes) +
        // ConvertToFilterable for VSM/EVSM. Binds its own render targets; the
        // caller re-binds the world targets afterwards.
        void renderCascadesImpl(void) {
            if (!passReady) return;
            Diligent::IDeviceContext* ctx = engine::renderer::diligent::context;
            static const bool noPlayer = getenv("ENGINE_SHADOW_NO_PLAYER") != nullptr;

            const int numCascades = lightAttribs.ShadowAttribs.iNumCascades;
            const Diligent::TextureDesc& smDesc = mgr.GetCascadeDSV(0)->GetTexture()->GetDesc();
            for (int i = 0; i < numCascades; i++) {
                Diligent::ITextureView* dsv = mgr.GetCascadeDSV((u32)i);
                ctx->SetRenderTargets(0,
                                      nullptr,
                                      dsv,
                                      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                ctx->ClearDepthStencil(dsv,
                                       Diligent::CLEAR_DEPTH_FLAG,
                                       1.0f,
                                       0,
                                       Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

                Diligent::Viewport vp(0.0f, 0.0f, (f32)smDesc.Width, (f32)smDesc.Height, 0.0f, 1.0f);
                ctx->SetViewports(1, &vp, 0, 0);
                Diligent::Rect scissor(0, 0, (i32)smDesc.Width, (i32)smDesc.Height);
                ctx->SetScissorRects(1, &scissor, 0, 0);

                if (!noPlayer)
                    gltfDiligentShadowDraw(ctx,
                                           mgr.GetCascadeTransform((u32)i).WorldToLightProjSpace,
                                           dsv);
                if (splatTerrainShadowDrawsDiligent()) {
                    Diligent::ScopedDebugGroup terrainGroup(ctx, "terrain");
                    splatTerrainShadowDrawDiligent(ctx,
                                                  mgr.GetCascadeTransform((u32)i).WorldToLightProjSpace,
                                                  dsv,
                                                  i);
                }
            }

            if (curMode != 1 /* PCF */) {
                mgr.ConvertToFilterable(ctx, lightAttribs.ShadowAttribs);
            }

            // ENGINE_SHADOW_READBACK=frameN: one-shot readback of cascade 0's depth
            // (min/max/zero-fraction) — verifies the cascade pass actually writes
            // depth. Debug only.
            static bool readbackDone = false;
            if (!readbackDone) {
                static const u64 atFrame = [] {
                    const char* rbEnv = getenv("ENGINE_SHADOW_READBACK");
                    return rbEnv ? strtoull(rbEnv, nullptr, 10) : 0;
                }();
                static u64 frameCounter = 0;
                frameCounter++;
                if (atFrame && frameCounter >= atFrame) {
                    readbackDone         = true;
                    const int rbCascades = lightAttribs.ShadowAttribs.iNumCascades;
                    for (int ci = 0; ci < rbCascades; ci++) {
                        Diligent::ITextureView* dsv0    = mgr.GetCascadeDSV((u32)ci);
                        Diligent::ITexture* src         = dsv0->GetTexture();
                        const Diligent::TextureDesc& sd = src->GetDesc();
                        Diligent::TextureDesc stg       = sd;
                        stg.Name                        = "shadow readback staging";
                        stg.Usage                       = Diligent::USAGE_STAGING;
                        stg.BindFlags                   = Diligent::BIND_NONE;
                        stg.CPUAccessFlags              = Diligent::CPU_ACCESS_READ;
                        stg.MipLevels                   = 1;
                        stg.ArraySize                   = 1;
                        stg.Height                      = sd.Height;
                        RefCntAutoPtr<Diligent::ITexture> staging;
                        device->CreateTexture(stg, nullptr, &staging);
                        if (staging) {
                            Diligent::StateTransitionDesc toCopy{
                                src,
                                Diligent::RESOURCE_STATE_DEPTH_WRITE,
                                Diligent::RESOURCE_STATE_COPY_SOURCE,
                                Diligent::STATE_TRANSITION_FLAG_UPDATE_STATE};
                            ctx->TransitionResourceStates(1, &toCopy);
                            Diligent::CopyTextureAttribs cp(
                                src,
                                Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE,
                                staging,
                                Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
                            cp.SrcSlice = (Diligent::Uint32)ci;
                            ctx->CopyTexture(cp);
                            Diligent::StateTransitionDesc back{
                                src,
                                Diligent::RESOURCE_STATE_COPY_SOURCE,
                                Diligent::RESOURCE_STATE_DEPTH_WRITE,
                                Diligent::STATE_TRANSITION_FLAG_UPDATE_STATE};
                            ctx->TransitionResourceStates(1, &back);
                            ctx->WaitForIdle();
                            Diligent::MappedTextureSubresource mapped;
                            ctx->MapTextureSubresource(staging,
                                                       0,
                                                       0,
                                                       Diligent::MAP_READ,
                                                       Diligent::MAP_FLAG_NONE,
                                                       nullptr,
                                                       mapped);
                            if (mapped.pData) {
                                if (const char* dumpPath = getenv("ENGINE_SHADOW_DUMP")) {
                                    char pathBuf[512];
                                    snprintf(pathBuf, sizeof(pathBuf), "%s.%d", dumpPath, ci);
                                    FILE* f = fopen(pathBuf, "wb");
                                    if (f) {
                                        fprintf(f, "P5\n%u %u\n255\n", sd.Width, sd.Height);
                                        for (u32 y = 0; y < sd.Height; y++) {
                                            const float* row =
                                                (const float*)((const u8*)mapped.pData +
                                                               (size_t)y * mapped.Stride);
                                            for (u32 x = 0; x < sd.Width; x++) {
                                                float d = std::clamp(row[x], 0.0f, 1.0f);
                                                fputc((u8)(d * 255.0f), f);
                                            }
                                        }
                                        fclose(f);
                                        utils::info("shadow readback: dump written to %s", pathBuf);
                                    }
                                }
                                u32 zeros = 0, ones = 0;
                                float mn = 2.0f, mx = -1.0f;
                                u32 minx = sd.Width, maxx = 0, miny = sd.Height, maxy = 0;
                                for (u32 y = 0; y < sd.Height; y++) {
                                    const float* row = (const float*)((const u8*)mapped.pData +
                                                                      (size_t)y * mapped.Stride);
                                    for (u32 x = 0; x < sd.Width; x++) {
                                        float d = row[x];
                                        if (d == 0.0f) zeros++;
                                        if (d == 1.0f) ones++;
                                        mn = std::min(mn, d);
                                        mx = std::max(mx, d);
                                        if (d < 0.999f) {
                                            minx = std::min(minx, x);
                                            maxx = std::max(maxx, x);
                                            miny = std::min(miny, y);
                                            maxy = std::max(maxy, y);
                                        }
                                    }
                                }
                                utils::info(
                                    "shadow readback cascade%d: min %.6f max %.6f zeros %.1f%% "
                                    "ones %.1f%%",
                                    ci,
                                    mn,
                                    mx,
                                    100.0 * zeros / (sd.Width * sd.Height),
                                    100.0 * ones / (sd.Width * sd.Height));
                                if (maxx > minx) {
                                    utils::info(
                                        "shadow readback: geometry bbox uv (%.3f, %.3f)..(%.3f, "
                                        "%.3f)",
                                        (float)minx / sd.Width,
                                        (float)miny / sd.Height,
                                        (float)maxx / sd.Width,
                                        (float)maxy / sd.Height);
                                } else {
                                    utils::warn("shadow readback: NO geometry depth in cascade %d",
                                                ci);
                                }
                            } else {
                                utils::warn("shadow readback: map failed");
                            }
                            ctx->UnmapTextureSubresource(staging, 0, 0);
                        }
                    }
                }
            }
        }

    }  // namespace

    void shadowDiligentUpdateFrame(void) {
        updateFrameImpl();
    }

    void shadowDiligentRenderCascades(void) {
        renderCascadesImpl();
    }

    void shadowDiligentDestroy(void) {
        mgr.~ShadowMapManager();
        new (&mgr) ShadowMapManager();
        cmpSampler.Release();
        filterableSampler.Release();
        passReady  = false;
        pbrReady   = false;
        initFailed = false;
        curMode    = -1;
        curQuality = -1;
    }

    bool shadowDiligentActive(void) {
        return passReady;
    }

    u32 shadowDiligentGeneration(void) {
        return generation;
    }

    int shadowDiligentMode(void) {
        return curMode;
    }

    int shadowDiligentPcfFilterSize(void) {
        if (curQuality < 0 || curQuality > 2) return 3;
        return kQualityTiers[curQuality].pcfFilterSize;
    }

    float shadowDiligentTierDistance(void) {
        if (!passReady) return 0.0f;
        static const bool fadeDisabled = [] {
            const char* fadeEnv = getenv("ENGINE_SHADOW_FADE");
            return fadeEnv != nullptr && atof(fadeEnv) <= 0.0;
        }();
        if (fadeDisabled)
            return 0.0f;  // A/B: disable the receiver fade
        if (curQuality < 0 || curQuality > 2) return 0.0f;
        return kQualityTiers[curQuality].distanceM;
    }

    const void* shadowDiligentLightAttribs(void) {
        return &lightAttribs;
    }

    Diligent::ITextureView* shadowDiligentShadowSRV(void) {
        if (!passReady) return nullptr;
        return curMode == 1 ? mgr.GetSRV() : mgr.GetFilterableSRV();
    }

    Diligent::ISampler* shadowDiligentShadowSampler(void) {
        return curMode == 1 ? cmpSampler : filterableSampler;
    }

    void shadowDiligentCasterBias(float& slopeBias, float& constBias, float& biasClamp) {
        static const float slope = [] {
            float v = 2.0f;
            if (const char* slopeEnv = getenv("ENGINE_SHADOW_SLOPE_BIAS")) {
                const float parsed = (float)atof(slopeEnv);
                if (parsed >= 0.0f && parsed <= 16.0f) v = parsed;
            }
            return v;
        }();
        static const float constant = [] {
            float v = 0.0f;
            if (const char* constEnv = getenv("ENGINE_SHADOW_DEPTH_BIAS")) {
                const float parsed = (float)atof(constEnv);
                if (parsed >= 0.0f && parsed <= 1000000.0f) v = parsed;
            }
            return v;
        }();
        static const float clampV = [] {
            float v = 0.002f;
            if (const char* clampEnv = getenv("ENGINE_SHADOW_BIAS_CLAMP")) {
                const float parsed = (float)atof(clampEnv);
                if (parsed >= 0.0f && parsed <= 0.05f) v = parsed;
            }
            return v;
        }();
        slopeBias = slope;
        constBias = constant;
        biasClamp = clampV;
    }

    const Diligent::float4x4* shadowDiligentPbrWorldToLightProj(void) {
        return pbrReady ? &pbrW2L : nullptr;
    }

    float shadowDiligentPbrSlice(void) {
        return (float)pbrSlice;
    }

    float shadowDiligentPbrDepthBias(void) {
        return pbrBias;
    }

}  // namespace engine::renderer::diligent
