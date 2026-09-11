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
#include <cmath>
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
                float v = 1.0f;
                if (const char* padEnv = getenv("ENGINE_SHADOW_CASTER_PAD")) {
                    const float parsed = (float)atof(padEnv);
                    if (parsed >= 1.0f && parsed <= 4.0f) v = parsed;
                }
                return v;
            }();
            Diligent::float4x4 padProj = proj;
            padProj._11 /= casterPad;
            padProj._22 /= casterPad;

            // Third-person focus anchoring: the orbit camera sits ~11 m from the
            // player, outside a pure camera-distance cascade 0 (~4.7 m), so the
            // player's caster and the ground receiving its shadow both land in the
            // coarse far cascade. Widen cascade 0 to cover the focus band (player
            // cam-Z + margin) and log-uniformly re-partition the remaining
            // cascades from the band to the tier distance (the next cascade's
            // near plane follows this cascade's far by construction).
            double ppos[3] = {0.0, 0.0, 0.0};
            double an[3]  = {0.0, 0.0, 0.0};
            const bool hasPlayer = engine::playerGetFootPos(ppos);
            if (hasPlayer) diligentWorldAnchor(an);
            float focusBand = 0.0f;
            if (hasPlayer) {
                const float rx = (f32)(ppos[0] - an[0]);
                const float ry = (f32)(ppos[1] - an[1]);
                const float rz = (f32)(ppos[2] - an[2]);
                const float focusCamZ = view._31 * rx + view._32 * ry + view._33 * rz;
                static const float focusMargin = [] {
                    float v = 3.0f;
                    if (const char* env = getenv("ENGINE_SHADOW_FOCUS_MARGIN")) {
                        const float parsed = (float)atof(env);
                        if (parsed >= 1.0f && parsed <= 20.0f) v = parsed;
                    }
                    return v;
                }();
                if (focusCamZ <= tier.distanceM) {
                    focusBand = focusCamZ + focusMargin;
                    if (focusBand > tier.distanceM * 0.8f) focusBand = tier.distanceM * 0.8f;
                    if (focusBand < 0.0f) focusBand = 0.0f;
                }
            }
            int bandActive = 0;

            ShadowMapManager::DistributeCascadeInfo dist;
            dist.pCameraView                      = &view;
            dist.pCameraProj                      = &padProj;
            dist.pLightDir                         = &dir;
            dist.SnapCascades                      = true;
            dist.StabilizeExtents                  = true;
            dist.EqualizeExtents                   = true;
            dist.fPartitioningFactor               = 0.95f;
            dist.PackMatrixRowMajor                = false;
            dist.UseRightHandedLightViewTransform  = true;
            dist.AdjustCascadeRange                = [&](int i, float& minZ, float& maxZ) {
                if (minZ < engine::renderer::kCameraNear) minZ = engine::renderer::kCameraNear;
                if (maxZ > tier.distanceM) maxZ = tier.distanceM;
                if (i <= 0) {
                    if (i == 0 && focusBand > maxZ && focusBand > minZ + 2.0f) {
                        maxZ = focusBand;
                        bandActive = 1;
                    }
                    return;
                }
                if (!bandActive) return;
                const int rem     = (int)tier.cascades - 1;
                const float power = (float)i / (float)rem;
                float logZ       = focusBand * powf(tier.distanceM / focusBand, power);
                float uniformZ   = focusBand + (tier.distanceM - focusBand) * power;
                maxZ = dist.fPartitioningFactor * (logZ - uniformZ) + uniformZ;
            };
            mgr.DistributeCascades(dist, lightAttribs.ShadowAttribs);

            // Depth bias per resolution (the Shadows sample's policy), normalized
            // to the cascade z range: FractionalSamplingError adds it verbatim in
            // NDC depth, so a fixed 0.005 is ~30 texels of cascade-0 depth here and
            // washes every comparison to lit. Scale by the light-space z scale.
            sa.fFixedDepthBias =
                (tier.resolution >= 2048 ? 0.0025f : 0.005f) * sa.Cascades[0].f4LightSpaceScale.z;

            // ENGINE_SHADOW_ORACLE=frameN: one-shot CPU check that the caster
            // matrix (the raw GetCascadeTransform the depth pass renders with)
            // and the receiver reconstruction (mWorldToLightView + cascade
            // scale/scaled-bias, byte-for-byte the splat PS's per-pixel formula)
            // land on the same NDC point for known render-space points per
            // cascade. frameN = Nth frame the pass is ready. On mismatch the
            // matrix bytes (hex) and full-precision floats are logged.
            {
                static const u64 oracleAt = [] {
                    const char* env = getenv("ENGINE_SHADOW_ORACLE");
                    return env ? strtoull(env, nullptr, 10) : 0;
                }();
                static u64 oracleCounter = 0;
                static bool oracleDone = false;
                oracleCounter++;
                if (!oracleDone && oracleAt && oracleCounter >= oracleAt && hasPlayer) {
                    struct OPt {
                        const char* name;
                        float x, y, z;
                    };
                    OPt pts[4];
                    int np = 0;
                    const float prx = (f32)(ppos[0] - an[0]);
                    const float pry = (f32)(ppos[1] - an[1]);
                    const float prz = (f32)(ppos[2] - an[2]);
                    pts[np++] = {"feet", prx, pry, prz};
                    pts[np++] = {"head", prx, pry + 1.8f, prz};
                    if (const SplatTerrain* st = splatTerrainDiligent()) {
                        const SplatChunk* best = nullptr;
                        float bestD            = 1e30f;
                        for (const auto& ch : st->chunks) {
                            const float cxp = (ch.aabbMin[0] + ch.aabbMax[0]) * 0.5f;
                            const float cyp = (ch.aabbMin[1] + ch.aabbMax[1]) * 0.5f;
                            const float czp = (ch.aabbMin[2] + ch.aabbMax[2]) * 0.5f;
                            const float dx  = (f32)ppos[0] - cxp;
                            const float dy  = (f32)ppos[1] - cyp;
                            const float dz  = (f32)ppos[2] - czp;
                            const float d   = dx * dx + dy * dy + dz * dz;
                            if (d < bestD) {
                                bestD = d;
                                best  = &ch;
                            }
                        }
                        if (best) {
                            const float cxp = (best->aabbMin[0] + best->aabbMax[0]) * 0.5f;
                            const float cyp = (best->aabbMin[1] + best->aabbMax[1]) * 0.5f;
                            const float czp = (best->aabbMin[2] + best->aabbMax[2]) * 0.5f;
                            const f32 cpx  = ppos[0] < cxp ? best->aabbMin[0] : best->aabbMax[0];
                            const f32 cpy  = ppos[1] < cyp ? best->aabbMin[1] : best->aabbMax[1];
                            const f32 cpz  = ppos[2] < czp ? best->aabbMin[2] : best->aabbMax[2];
                            pts[np++] = {"chunk corner", (f32)(cpx - an[0]), (f32)(cpy - an[1]), (f32)(cpz - an[2])};
                        }
                    }
                    {
                        float z = tier.distanceM * 0.6f;
                        if (z < 40.0f) z = 40.0f;
                        if (z > 60.0f) z = 60.0f;
                        pts[np++] = {"view axis", 0.0f, 0.0f, z};
                    }

                    utils::info(
                        "shadow oracle: frame %llu mode %d res %u, %u cascades, tier %.0f m",
                        (unsigned long long)oracleCounter,
                        curMode,
                        tier.resolution,
                        (unsigned)tier.cascades,
                        (double)tier.distanceM);
                    const Diligent::float4x4& w2l = sa.mWorldToLightView;
                    const float res               = (float)tier.resolution;
                    int failMask                   = 0;
                    for (u32 c = 0; c < (u32)sa.iNumCascades; c++) {
                        const auto& ca   = sa.Cascades[c];
                        const auto& w2lp = mgr.GetCascadeTransform(c).WorldToLightProjSpace;
                        for (int pi = 0; pi < np; pi++) {
                            const OPt& p = pts[pi];
                            // Caster: standard column-vector transform by the raw
                            // (untransposed) matrix.
                            const float ccx = w2lp._11 * p.x + w2lp._21 * p.y + w2lp._31 * p.z + w2lp._41;
                            const float ccy = w2lp._12 * p.x + w2lp._22 * p.y + w2lp._32 * p.z + w2lp._42;
                            const float ccz = w2lp._13 * p.x + w2lp._23 * p.y + w2lp._33 * p.z + w2lp._43;
                            const float ccw = w2lp._14 * p.x + w2lp._24 * p.y + w2lp._34 * p.z + w2lp._44;
                            // Receiver: row-vector math on the transposed-stored
                            // mWorldToLightView + the cascade's scale/scaled-bias —
                            // the splat_terrain_ps formula, w == 1 (ortho).
                            const float lvx = p.x * w2l._11 + p.y * w2l._12 + p.z * w2l._13 + w2l._14;
                            const float lvy = p.x * w2l._21 + p.y * w2l._22 + p.z * w2l._23 + w2l._24;
                            const float lvz = p.x * w2l._31 + p.y * w2l._32 + p.z * w2l._33 + w2l._34;
                            const float rxn = lvx * ca.f4LightSpaceScale.x + ca.f4LightSpaceScaledBias.x;
                            const float ryn = lvy * ca.f4LightSpaceScale.y + ca.f4LightSpaceScaledBias.y;
                            const float rzn = lvz * ca.f4LightSpaceScale.z + ca.f4LightSpaceScaledBias.z;
                            bool bad = false;
                            if (ccw == 0.0f) {
                                bad = true;
                            } else {
                                const float iw  = 1.0f / ccw;
                                const float czn = ccz * iw;
                                const float cuv[2] = {0.5f + 0.5f * ccx * iw, 0.5f - 0.5f * ccy * iw};
                                const float ruv[2] = {0.5f + 0.5f * rxn, 0.5f - 0.5f * ryn};
                                const float duvx = fabsf(cuv[0] - ruv[0]) * res;
                                const float duvy = fabsf(cuv[1] - ruv[1]) * res;
                                const float dz   = fabsf(czn - rzn);
                                // The receiver only queries in-box pixels (the
                                // splat PS clamps + skips out-of-box), so the
                                // criteria only bind there: far points live in
                                // f32 precision where the two rounding orders
                                // legitimately diverge.
                                const bool inBox = cuv[0] >= -0.01f && cuv[0] <= 1.01f &&
                                                   cuv[1] >= -0.01f && cuv[1] <= 1.01f &&
                                                   czn >= -0.01f && czn <= 1.01f;
                                bad = inBox && (duvx >= 2.0f || duvy >= 2.0f || dz >= 1e-4f);
                                utils::info(
                                    "shadow oracle: c%u %s caster uv(%.5f %.5f) z %.6f recv uv(%.5f %.5f) z "
                                    "%.6f d %.2f/%.2f tex dz %.2e %s",
                                    c,
                                    p.name,
                                    cuv[0],
                                    cuv[1],
                                    czn,
                                    ruv[0],
                                    ruv[1],
                                    rzn,
                                    duvx,
                                    duvy,
                                    dz,
                                    bad ? "FAIL" : (inBox ? "ok" : "out-of-box"));
                            }
                            if (bad) {
                                failMask |= 1 << c;
                                utils::info("shadow oracle: c%u %s FAIL (caster w %f)", c, p.name, ccw);
                            }
                        }
                    }
                    if (failMask == 0) {
                        utils::info("shadow oracle: PASS — caster and receiver agree on all points/cascades");
                    } else {
                        utils::info("shadow oracle: MISMATCH (cascade bits %x) — matrix dump:", failMask);
                        auto dumpMat = [](const char* label, const void* p, int nf) {
                            const unsigned char* b = reinterpret_cast<const unsigned char*>(p);
                            char hex[257] = {0};
                            for (int i = 0; i < nf * 4; i++) snprintf(hex + 2 * i, 3, "%02x", b[i]);
                            const float* f = reinterpret_cast<const float*>(p);
                            utils::info("shadow oracle:   %s bytes: %s", label, hex);
                            for (int i = 0; i < nf; i++)
                                utils::info("shadow oracle:   %s[%d] %.9g", label, i, (double)f[i]);
                        };
                        for (u32 c = 0; c < (u32)sa.iNumCascades; c++) {
                            if (!(failMask & (1 << c))) continue;
                            dumpMat("casterWorldToLightProj", &mgr.GetCascadeTransform(c).WorldToLightProjSpace, 16);
                            dumpMat("receiverWorldToLightView", &sa.mWorldToLightView, 16);
                            dumpMat("cascadeScale", &sa.Cascades[c].f4LightSpaceScale, 4);
                            dumpMat("cascadeScaledBias", &sa.Cascades[c].f4LightSpaceScaledBias, 4);
                        }
                    }
                    oracleDone = true;
                }
            }

            // PBR (glTF) receiver: the player is one small receiver, so the
            // per-pixel cascade pick the runtime receivers get collapses to a
            // per-frame CPU pick: the cascade whose camera-space z range covers the
            // feet (render space, +1 m for the torso). Same untransposed matrix the
            // caster draws with (GetCascadeTransform), so receiver and caster agree.
            if (hasPlayer) ppos[1] += 1.0;  // torso
            const Diligent::float3 pPos{(f32)(ppos[0] - an[0]), (f32)(ppos[1] - an[1]), (f32)(ppos[2] - an[2])};
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
            if (!dumped && hasPlayer) {
                dumped                      = true;
                utils::info("shadow dbg: focus band %.2f m active %d", (double)focusBand, bandActive);
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
                                // Where does the player caster land in this cascade's
                                // light NDC? (same matrix the caster draws with)
                                double pp[3] = {0.0, 0.0, 0.0};
                                if (engine::playerGetFootPos(pp)) {
                                    double an[3] = {0.0, 0.0, 0.0};
                                    diligentWorldAnchor(an);
                                    utils::info(
                                        "shadow readback cascade%d player pos (%.2f, %.2f, %.2f) "
                                        "anchor (%.2f, %.2f, %.2f) rel (%.2f, %.2f, %.2f)",
                                        ci,
                                        (double)pp[0],
                                        (double)pp[1],
                                        (double)pp[2],
                                        (double)an[0],
                                        (double)an[1],
                                        (double)an[2],
                                        (double)pp[0] - (double)an[0],
                                        (double)pp[1] - (double)an[1],
                                        (double)pp[2] - (double)an[2]);
                                    const float ft[3] = {(f32)(pp[0] - an[0]),
                                                         (f32)(pp[1] - an[1]),
                                                         (f32)(pp[2] - an[2])};
                                    const float hd[3] = {ft[0], ft[1] + 1.8f, ft[2]};
                                    const Diligent::float4x4& w2lp =
                                        mgr.GetCascadeTransform((u32)ci).WorldToLightProjSpace;
                                    auto ndc = [&](const float* p) {
                                        return Diligent::float3{
                                            p[0] * w2lp._11 + p[1] * w2lp._21 + p[2] * w2lp._31 +
                                                w2lp._41,
                                            p[0] * w2lp._12 + p[1] * w2lp._22 + p[2] * w2lp._32 +
                                                w2lp._42,
                                            p[0] * w2lp._13 + p[1] * w2lp._23 + p[2] * w2lp._33 +
                                                w2lp._43};
                                    };
                                    const Diligent::float3 nF = ndc(ft);
                                    const Diligent::float3 nH = ndc(hd);
                                    utils::info(
                                        "shadow readback cascade%d player NDC feet (%.3f, %.3f, "
                                        "%.3f) head (%.3f, %.3f, %.3f) inside feet %d head %d",
                                        ci,
                                        nF.x,
                                        nF.y,
                                        nF.z,
                                        nH.x,
                                        nH.y,
                                        nH.z,
                                        std::abs(nF.x) <= 1.f && std::abs(nF.y) <= 1.f &&
                                                std::abs(nF.z) <= 1.f
                                            ? 1
                                            : 0,
                                        std::abs(nH.x) <= 1.f && std::abs(nH.y) <= 1.f &&
                                                std::abs(nH.z) <= 1.f
                                            ? 1
                                            : 0);
                                }
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

            // ENGINE_SHADOW_EVSM=frameN: one-shot CPU deep-dive on the filterable
            // (VSM/EVSM) atlas — per-cascade moment stats plus, per known point,
            // the receiver warpDepthEVSM value vs the atlas moments (Chebyshev as
            // the PS computes it) and the moments re-derived from the RAW depth
            // atlas on the CPU, so a zero-moment (converter) fault is separated
            // from a receiver-convention fault. ENGINE_EVSM_DUMP=path writes a
            // log10(R) PGM per cascade slice.
            {
                static const u64 evsmAt = [] {
                    const char* env = getenv("ENGINE_SHADOW_EVSM");
                    return env ? strtoull(env, nullptr, 10) : 0;
                }();
                static u64 evsmCounter = 0;
                static bool evsmDone = false;
                evsmCounter++;
                if (!evsmDone && evsmAt && evsmCounter >= evsmAt &&
                    (curMode == 2 || curMode == 3 || curMode == 4)) {
                    evsmDone = true;
                    const Diligent::ShadowMapAttribs& sa = lightAttribs.ShadowAttribs;
                    const ShadowQualityTier& tier =
                        kQualityTiers[curQuality < 0 ? 0 : (curQuality > 2 ? 2 : curQuality)];
                    double ppos[3] = {0.0, 0.0, 0.0};
                    double an[3]  = {0.0, 0.0, 0.0};
                    const bool hasPlayer = engine::playerGetFootPos(ppos);
                    Diligent::ITextureView* fltSRV = mgr.GetFilterableSRV();
                    Diligent::ITextureView* rawSRV = mgr.GetSRV();
                    if (!fltSRV || !rawSRV) {
                        utils::warn("evsm probe: no SRVs");
                        return;
                    }
                    Diligent::ITexture* fltTex = fltSRV->GetTexture();
                    Diligent::ITexture* rawTex = rawSRV->GetTexture();
                    const Diligent::TextureDesc& fd = fltTex->GetDesc();
                    const Diligent::TextureDesc& rd = rawTex->GetDesc();
                    const int fpt = (fd.Format == Diligent::TEX_FORMAT_RGBA32_FLOAT) ? 4 : 2;
                    utils::info(
                        "evsm probe: frame %llu mode %d raw fmt %d flt fmt %d (%d floats/texel) "
                        "exp (pos %.1f neg %.1f) bIs32 %d fixedFilter %d vsmBias %.3g",
                        (unsigned long long)evsmCounter,
                        curMode,
                        (int)rd.Format,
                        (int)fd.Format,
                        fpt,
                        (double)sa.fEVSMPositiveExponent,
                        (double)sa.fEVSMNegativeExponent,
                        (int)sa.bIs32BitEVSM,
                        sa.iFixedFilterSize,
                        (double)sa.fVSMBias);
                    struct SliceData {
                        bool ok = false;
                        Diligent::RefCntAutoPtr<Diligent::ITexture> staging;
                        Diligent::MappedTextureSubresource mapped;
                        u32 w = 0, h = 0;
                        size_t stride = 0;
                        const float* data = nullptr;
                    };
                    SliceData rawSlice[8], fltSlice[8];
                    auto copySlice = [&](Diligent::ITexture* src,
                                         const Diligent::TextureDesc& sd,
                                         Diligent::RESOURCE_STATE fromState,
                                         int ci,
                                         SliceData& out) -> bool {
                        if (ci < 0 || ci >= (int)sd.ArraySize) return false;
                        Diligent::TextureDesc stg = sd;
                        stg.Name         = "evsm probe staging";
                        stg.Usage        = Diligent::USAGE_STAGING;
                        stg.BindFlags    = Diligent::BIND_NONE;
                        stg.CPUAccessFlags = Diligent::CPU_ACCESS_READ;
                        stg.MipLevels    = 1;
                        stg.ArraySize    = 1;
                        device->CreateTexture(stg, nullptr, &out.staging);
                        if (out.staging == nullptr) {
                            return false;
                        }
                        out.w = sd.Width;
                        out.h = sd.Height;
                        Diligent::StateTransitionDesc tc{
                            src, fromState, Diligent::RESOURCE_STATE_COPY_SOURCE,
                            Diligent::STATE_TRANSITION_FLAG_UPDATE_STATE};
                        ctx->TransitionResourceStates(1, &tc);
                        Diligent::CopyTextureAttribs cp(src, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE,
                                                        out.staging, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
                        cp.SrcSlice = (Diligent::Uint32)ci;
                        ctx->CopyTexture(cp);
                        Diligent::StateTransitionDesc back{src, Diligent::RESOURCE_STATE_COPY_SOURCE,
                                                          fromState, Diligent::STATE_TRANSITION_FLAG_UPDATE_STATE};
                        ctx->TransitionResourceStates(1, &back);
                        ctx->WaitForIdle();
                        ctx->MapTextureSubresource(out.staging, 0, 0, Diligent::MAP_READ,
                                                  Diligent::MAP_FLAG_NONE, nullptr, out.mapped);
                        if (!out.mapped.pData) {
                            return false;
                        }
                        out.stride = out.mapped.Stride;
                        out.data   = (const float*)out.mapped.pData;
                        out.ok     = true;
                        return true;
                    };
                    const int nCas = sa.iNumCascades;
                    for (int ci = 0; ci < nCas; ci++) {
                        if (!copySlice(rawTex, rd, Diligent::RESOURCE_STATE_DEPTH_WRITE, ci, rawSlice[ci]))
                            utils::warn("evsm probe: raw slice %d copy failed", ci);
                        if (!copySlice(fltTex, fd, Diligent::RESOURCE_STATE_RENDER_TARGET, ci, fltSlice[ci]))
                            utils::warn("evsm probe: filterable slice %d copy failed", ci);
                    }
                    if (hasPlayer) diligentWorldAnchor(an);
                    struct EPt {                        const char* name;
                        float x, y, z;
                    };
                    EPt pts[4];
                    int np = 0;
                    if (hasPlayer) {
                        const float px = (f32)(ppos[0] - an[0]);
                        const float py = (f32)(ppos[1] - an[1]);
                        const float pz = (f32)(ppos[2] - an[2]);
                        pts[np++] = {"feet", px, py, pz};
                        pts[np++] = {"head", px, py + 1.8f, pz};
                    }
                    if (const SplatTerrain* st = splatTerrainDiligent()) {
                        const SplatChunk* best = nullptr;
                        float bestD = 1e30f;
                        for (const auto& ch : st->chunks) {
                            const float cxp = (ch.aabbMin[0] + ch.aabbMax[0]) * 0.5f;
                            const float cyp = (ch.aabbMin[1] + ch.aabbMax[1]) * 0.5f;
                            const float czp = (ch.aabbMin[2] + ch.aabbMax[2]) * 0.5f;
                            const float dx = (f32)ppos[0] - cxp;
                            const float dy = (f32)ppos[1] - cyp;
                            const float dz = (f32)ppos[2] - czp;
                            const float d = dx * dx + dy * dy + dz * dz;
                            if (d < bestD) {
                                bestD = d;
                                best = &ch;
                            }
                        }
                        if (best) {
                            const float cxp = (best->aabbMin[0] + best->aabbMax[0]) * 0.5f;
                            const float cyp = (best->aabbMin[1] + best->aabbMax[1]) * 0.5f;
                            const float czp = (best->aabbMin[2] + best->aabbMax[2]) * 0.5f;
                            const f32 cpx = ppos[0] < cxp ? best->aabbMin[0] : best->aabbMax[0];
                            const f32 cpy = ppos[1] < cyp ? best->aabbMin[1] : best->aabbMax[1];
                            const f32 cpz = ppos[2] < czp ? best->aabbMin[2] : best->aabbMax[2];
                            pts[np++] = {"chunk corner", (f32)(cpx - an[0]), (f32)(cpy - an[1]), (f32)(cpz - an[2])};
                        }
                    }
                    {
                        float z = tier.distanceM * 0.6f;
                        if (z < 40.0f) z = 40.0f;
                        if (z > 60.0f) z = 60.0f;
                        pts[np++] = {"view axis", 0.0f, 0.0f, z};
                    }
                    // Same warp the PS's warpDepthEVSM runs (exponents clamped
                    // to 42 for the 32-bit atlas, bIs32BitEVSM is 1 here).
                    const float exP = std::min(sa.fEVSMPositiveExponent, 42.0f);
                    const float exN = std::min(sa.fEVSMNegativeExponent, 42.0f);
                    auto warp1 = [&](float depth) { return std::exp(exP * (2.0f * depth - 1.0f)); };
                    auto warp2 = [&](float depth) { return -std::exp(-exN * (2.0f * depth - 1.0f)); };
                    const float bleed = sa.fVSMLightBleedingReduction;
                    auto cheb = [&](float m1, float m2, float mean, float minVar) {
                        float var = std::max(m2 - m1 * m1, minVar);
                        float d   = mean - m1;
                        float p    = var / (var + d * d);
                        if (bleed > 0.0f)
                            p = std::clamp((p - bleed) / (1.0f - bleed), 0.0f, 1.0f);
                        return mean <= m1 ? 1.0f : std::min(p, 1.0f);
                    };
                    auto sampleWeight = [](int x, float R) {
                        float lo = std::max((float)x, std::min(0.5f - R, 0.0f));
                        float hi = std::min((float)x + 1.0f, std::max(0.5f + R, 1.0f));
                        return hi - lo;
                    };
                    for (int ci = 0; ci < nCas; ci++) {
                        if (rawSlice[ci].ok) {
                            float mn = 2.0f, mx = -1.0f, sum = 0.0f;
                            u32 zeros = 0;
                            const u32 n = rawSlice[ci].w * rawSlice[ci].h;
                            for (u32 y = 0; y < rawSlice[ci].h; y++) {
                                const float* row = (const float*)((const u8*)rawSlice[ci].data + (size_t)y * rawSlice[ci].stride);
                                for (u32 x = 0; x < rawSlice[ci].w; x++) {
                                    float d = row[x];
                                    mn = std::min(mn, d);
                                    mx = std::max(mx, d);
                                    sum += d;
                                    if (d == 0.0f) zeros++;
                                }
                            }
                            utils::info("evsm probe: c%d raw depth min %.6f max %.6f mean %.6f zeros %.1f%%",
                                        ci, (double)mn, (double)mx, (double)(sum / (double)n),
                                        100.0 * zeros / (double)n);
                        }
                        if (fltSlice[ci].ok) {
                            float rMin = 1e30f, rMax = -1e30f, rSum = 0.0f;
                            float gMin = 1e30f, gMax = -1e30f, gSum = 0.0f;
                            float bMin = 1e30f, bMax = -1e30f, aMin = 1e30f, aMax = -1e30f;
                            u32 zeros = 0;
                            const u32 n = fltSlice[ci].w * fltSlice[ci].h;
                            for (u32 y = 0; y < fltSlice[ci].h; y++) {
                                const float* row = (const float*)((const u8*)fltSlice[ci].data + (size_t)y * fltSlice[ci].stride);
                                for (u32 x = 0; x < fltSlice[ci].w; x++) {
                                    const float* t = row + (size_t)x * fpt;
                                    if (t[0] == 0.0f && t[1] == 0.0f) zeros++;
                                    rMin = std::min(rMin, t[0]); rMax = std::max(rMax, t[0]); rSum += t[0];
                                    gMin = std::min(gMin, t[1]); gMax = std::max(gMax, t[1]); gSum += t[1];
                                    if (fpt == 4) {
                                        bMin = std::min(bMin, t[2]); bMax = std::max(bMax, t[2]);
                                        aMin = std::min(aMin, t[3]); aMax = std::max(aMax, t[3]);
                                    }
                                }
                            }
                            utils::info(
                                "evsm probe: c%d filterable zeros %.1f%% R(mean %.6g min %.6g max %.6g) "
                                "G(mean %.6g min %.6g max %.6g)",
                                ci, 100.0 * zeros / (double)n, (double)(rSum / (double)n),
                                (double)rMin, (double)rMax, (double)(gSum / (double)n), (double)gMin,
                                (double)gMax);
                            if (fpt == 4)
                                utils::info("evsm probe: c%d filterable B(min %.6g max %.6g) A(min %.6g max %.6g)",
                                            ci, (double)bMin, (double)bMax, (double)aMin, (double)aMax);
                        }
                    }
                    if (const char* dumpPath = getenv("ENGINE_EVSM_DUMP")) {
                        for (int ci = 0; ci < nCas; ci++) {
                            if (!fltSlice[ci].ok) continue;
                            char pathBuf[512];
                            snprintf(pathBuf, sizeof(pathBuf), "%s.%d", dumpPath, ci);
                            FILE* f = fopen(pathBuf, "wb");
                            if (!f) continue;
                            fprintf(f, "P5\n%u %u\n255\n", fltSlice[ci].w, fltSlice[ci].h);
                            for (u32 y = 0; y < fltSlice[ci].h; y++) {
                                const float* row = (const float*)((const u8*)fltSlice[ci].data + (size_t)y * fltSlice[ci].stride);
                                for (u32 x = 0; x < fltSlice[ci].w; x++) {
                                    float r = row[(size_t)x * fpt];
                                    u8 v = 0;
                                    if (r > 0.0f) {
                                        float lg = std::log10(r);
                                        lg = std::clamp(lg, -18.0f, 18.0f);
                                        v = (u8)((lg + 18.0f) * (255.0f / 36.0f));
                                    }
                                    fputc(v, f);
                                }
                            }
                            fclose(f);
                            utils::info("evsm probe: dump written to %s", pathBuf);
                        }
                    }
                    const Diligent::float4x4& w2l = sa.mWorldToLightView;
                    for (int ci = 0; ci < nCas; ci++) {
                        const auto& ca = sa.Cascades[ci];
                        const int radius = sa.iFixedFilterSize > 0 ? (sa.iFixedFilterSize - 1) / 2 : 0;
                        for (int pi = 0; pi < np; pi++) {
                            const EPt& p = pts[pi];
                            // The splat PS per-pixel formula (row-vector on the
                            // transposed-stored W2LView + cascade scale/bias).
                            const float lvx = p.x * w2l._11 + p.y * w2l._12 + p.z * w2l._13 + w2l._14;
                            const float lvy = p.x * w2l._21 + p.y * w2l._22 + p.z * w2l._23 + w2l._24;
                            const float lvz = p.x * w2l._31 + p.y * w2l._32 + p.z * w2l._33 + w2l._34;
                            const float u  = 0.5f + 0.5f * (lvx * ca.f4LightSpaceScale.x + ca.f4LightSpaceScaledBias.x);
                            const float v  = 0.5f - 0.5f * (lvy * ca.f4LightSpaceScale.y + ca.f4LightSpaceScaledBias.y);
                            const float z  = std::max(lvz * ca.f4LightSpaceScale.z + ca.f4LightSpaceScaledBias.z, 0.0f);
                            const bool inBox = u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
                            if (!fltSlice[ci].ok) continue;
                            const u32 iw = fltSlice[ci].w;
                            const u32 ih = fltSlice[ci].h;
                            const u32 ix = (u32)std::clamp((u32)(u * (float)iw), 0u, iw - 1u);
                            // Sampler v (0 = bottom) vs memory row 0 (top): try
                            // both and report whichever convention matches the
                            // atlas against the CPU-re-derived warp.
                            const u32 iyA = (u32)std::clamp((u32)(v * (float)ih), 0u, ih - 1u);
                            const u32 iyB = (u32)std::clamp((u32)((1.0f - v) * (float)ih), 0u, ih - 1u);
                            const float* tA = (const float*)((const u8*)fltSlice[ci].data + (size_t)iyA * fltSlice[ci].stride) + (size_t)ix * fpt;
                            const float* tB = (const float*)((const u8*)fltSlice[ci].data + (size_t)iyB * fltSlice[ci].stride) + (size_t)ix * fpt;
                            float cpuW1[2] = {0.0f, 0.0f};
                            float cpuMom[2] = {0.0f, 0.0f};
                            bool haveCpu   = false;
                            if (rawSlice[ci].ok) {
                                auto rawAt = [&](u32 x, u32 y) {
                                    return ((const float*)((const u8*)rawSlice[ci].data + (size_t)y * rawSlice[ci].stride))[x];
                                };
                                cpuW1[0] = warp1(rawAt(ix, iyA));
                                cpuW1[1] = warp1(rawAt(ix, iyB));
                                for (int ciConv = 0; ciConv < 2; ciConv++) {
                                    const u32 base = ciConv == 0 ? iyA : iyB;
                                    float sumW = 0.0f, tot = 0.0f;
                                    for (int dy = -radius; dy <= radius; dy++) {
                                        for (int dx = -radius; dx <= radius; dx++) {
                                            const u32 rx = (u32)std::clamp((int)ix + dx, 0, (int)iw - 1);
                                            const u32 ry = (u32)std::clamp((int)base + dy, 0, (int)ih - 1);
                                            const float wgt = sampleWeight(dx, (float)radius) * sampleWeight(dy, (float)radius);
                                            sumW += warp1(rawAt(rx, ry)) * wgt;
                                            tot += wgt;
                                        }
                                    }
                                    if (tot > 0.0f) {
                                        cpuMom[ciConv] = sumW / tot;
                                        haveCpu = true;
                                    }
                                }
                            }
                            utils::info("evsm probe: c%d %s recv uv (%.5f %.5f) z %.6f inBox %d",
                                        ci, p.name, u, v, (double)z, inBox ? 1 : 0);
                            const float w1 = warp1(z);
                            const float w2 = warp2(z);
                            const float minVarX = sa.fVSMBias * exP * w1;
                            const float minVarY = sa.fVSMBias * exN * w2;
                            for (int conv = 0; conv < 2; conv++) {
                                const float* t    = conv == 0 ? tA : tB;
                                float pPos        = cheb(t[0], t[1], w1, minVarX * minVarX);
                                float pFinal      = pPos;
                                float pNeg        = -1.0f;
                                if (curMode == 4 && fpt == 4) {
                                    pNeg   = cheb(t[2], t[3], w2, minVarY * minVarY);
                                    pFinal = std::min(pPos, pNeg);
                                }
                                const char* convTag =
                                    haveCpu ? (fabsf(t[0] - cpuMom[conv]) <=
                                                   std::max(1e-30f, 0.1f * fabsf(cpuMom[conv]))
                                                 ? "MATCH" : "")
                                            : "";
                                utils::info(
                                    "evsm probe: c%d %s conv%d texel(%u %u) atlas(R G%s B A) = (%.6g %.6g%s %.6g %.6g) "
                                    "cpuRawTexelW1 %.6g cpuBoxMom %.6g %s warp(w1 w2) (%.6g %.6g) "
                                    "minVar (%.3g %.3g) pPos %.5f pNeg %.5f p %.5f",
                                    ci, p.name, conv, ix, conv == 0 ? iyA : iyB, fpt == 4 ? " " : "",
                                    (double)t[0], (double)t[1], fpt == 4 ? " " : "",
                                    fpt == 4 ? (double)t[2] : 0.0, fpt == 4 ? (double)t[3] : 0.0,
                                    (double)cpuW1[conv], (double)cpuMom[conv], convTag,
                                    (double)w1, (double)w2, (double)minVarX, (double)minVarY,
                                    (double)pPos, (double)pNeg, (double)pFinal);
                            }
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
            float v = 33554432.0f;
            if (const char* clampEnv = getenv("ENGINE_SHADOW_BIAS_CLAMP")) {
                const float parsed = (float)atof(clampEnv);
                if (parsed >= 0.0f && parsed <= 1e10f) v = parsed;
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
