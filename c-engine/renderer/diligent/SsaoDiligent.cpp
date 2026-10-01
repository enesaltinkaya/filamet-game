#include "renderer/diligent/SsaoDiligent.h"

#include "renderer/diligent/DiligentRenderer.h"
#include "renderer/diligent/TaaDiligent.h"
#include "Utils.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "PostProcess/Common/interface/PostFXContext.hpp"
#include "PostProcess/Common/interface/PostFXRenderTechnique.hpp"
#include "PostProcess/ScreenSpaceAmbientOcclusion/interface/ScreenSpaceAmbientOcclusion.hpp"

#include <cstdlib>
#include <string>
#include <vector>

namespace Diligent {
    namespace HLSL {
#include "Shaders/Common/public/ShaderDefinitions.fxh"
#include "Shaders/PostProcess/ScreenSpaceAmbientOcclusion/public/ScreenSpaceAmbientOcclusionStructures.fxh"
    }  // namespace HLSL
}  // namespace Diligent

namespace engine::renderer::diligent {

    using namespace Diligent;

    static float envFloat(const char* name, float fallback, float lo, float hi) {
        const char* e = getenv(name);
        if (e == nullptr) {
            return fallback;
        }
        const float v = (float)atof(e);
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static std::unique_ptr<ScreenSpaceAmbientOcclusion> ssao;
    static HLSL::ScreenSpaceAmbientOcclusionAttribs ssaoAttribs{};
    static bool ssaoEnabled         = false;
    static bool ssaoHalfRes         = true;
    static float ssaoIntensityValue = 1.0f;
    static bool ssaoSettingsDirty   = true;

    static void ssaoApplyEnvAttribs(void);

    // The AO accumulates its own history (SSAO_MAX_HISTORY_LENGTH frames) and
    // denoises with it; the frame-index-rotated reconstruction kernel and the
    // half-res bilaterally-upsampled sample are only stable once the history is
    // alive. DiligentFX's gates are compile-time defines tuned for object
    // motion, and every one of them kills the history under a plain camera
    // dolly (docs/ao-temporal.md): the motion factor drops the history at
    // ~0.008 px/frame of flow, the 1% relative-depth disocclusion test drops it
    // at 1% of view-z change per frame (0.12 m at 12 m), and the variance gamma
    // then collapses to its floor. Seed the defines through the FX define
    // channel (DiligentFX/PostProcess/Common/src/PostFXRenderTechnique.cpp)
    // before the AO PSOs compile, on the same law the TAA uses: the history
    // dies at ENGINE_AO_VELOCITY_KNEE px/frame of flow (ENGINE_AO_DISOCCLUSION
    // for the relative-depth test, ENGINE_AO_MAX_HISTORY for the length).
    static void ssaoSeedDefines(void) {
        const float knee     = envFloat("ENGINE_AO_VELOCITY_KNEE", 102.0f, 1.0f, 512.0f);
        const float disocc   = envFloat("ENGINE_AO_DISOCCLUSION", 0.1f, 0.001f, 0.999f);
        const float maxHist  = envFloat("ENGINE_AO_MAX_HISTORY", 16.0f, 1.0f, 64.0f);
        char add[192];
        snprintf(add, sizeof(add),
                 "SSAO_TEMPORAL_MOTION_VECTOR_DIFF_FACTOR=%.5f;"
                 "SSAO_DISOCCLUSION_DEPTH_THRESHOLD=%.5f;"
                 "SSAO_MAX_HISTORY_LENGTH=%.1f",
                 (double)(1.025f / knee), (double)disocc, (double)maxHist);
        const char* cur = getenv("ENGINE_FX_SHADER_DEFINES");
        const std::string merged = (cur != nullptr && *cur != 0)
                                         ? std::string(cur) + ";" + add
                                         : std::string(add);
        setenv("ENGINE_FX_SHADER_DEFINES", merged.c_str(), 1);
    }

    void ssaoInit(void) {
        if (!device) {
            return;
        }
        ssaoSeedDefines();
        ScreenSpaceAmbientOcclusion::CreateInfo ci;
        ci.EnableAsyncCreation = false;
        ssao                   = std::make_unique<ScreenSpaceAmbientOcclusion>(device, ci);
        ssaoAttribs            = HLSL::ScreenSpaceAmbientOcclusionAttribs{};
    }

    void ssaoDestroy(void) {
        ssao.reset();
        ssaoEnabled = false;
    }

    void ssaoSettingsApply(bool enabled, float radius, int algorithm, float intensity) {
        ssaoSettingsDirty = ssaoSettingsDirty || enabled != ssaoEnabled ||
                            radius != ssaoAttribs.EffectRadius ||
                            algorithm != (int)ssaoAttribs.Algorithm ||
                            intensity != ssaoIntensityValue;
        ssaoEnabled                   = enabled;
        ssaoIntensityValue            = intensity;
        ssaoAttribs.EffectRadius      = radius;
        ssaoAttribs.Algorithm         = (uint)algorithm;
        ssaoAttribs.ResetAccumulation = false;
        ssaoApplyEnvAttribs();
    }

    static void ssaoApplyEnvAttribs(void) {
        static const auto table = [] {
            struct Pair { std::string k, v; };
            std::vector<Pair> out;
            const char* env = getenv("ENGINE_SSAO_ATTRIBS");
            for (const char* item = env; item != nullptr && *item != 0;) {
                const char* end = item;
                while (*end != 0 && *end != ';') end++;
                const char* eq  = item;
                while (eq < end && *eq != '=') eq++;
                if (eq < end)
                    out.push_back({std::string(item, eq), std::string(eq + 1, end)});
                item = (*end == ';') ? end + 1 : end;
            }
            return out;
        }();
        for (const auto& kv : table) {
            const float v = (float)atof(kv.v.c_str());
            const std::string& n = kv.k;
            if      (n == "EffectFalloffRange")        ssaoAttribs.EffectFalloffRange = v;
            else if (n == "RadiusMultiplier")          ssaoAttribs.RadiusMultiplier = v;
            else if (n == "DepthMIPSamplingOffset")    ssaoAttribs.DepthMIPSamplingOffset = v;
            else if (n == "TemporalStabilityFactor")   ssaoAttribs.TemporalStabilityFactor = v;
            else if (n == "SpatialReconstructionRadius") ssaoAttribs.SpatialReconstructionRadius = v;
            else if (n == "AlphaInterpolation")        ssaoAttribs.AlphaInterpolation = v;
            else if (n == "BitmaskThickness")          ssaoAttribs.BitmaskThickness = v;
            else if (n == "HalfResolution")            ssaoHalfRes = (v >= 0.5f);
            else
                utils::warn("ssao: unknown ENGINE_SSAO_ATTRIBS key %s", n.c_str());
        }
    }

    bool ssaoOn(void) {
        return ssaoEnabled;
    }

    bool ssaoReady(void) {
        return ssao != nullptr;
    }

    Diligent::ITextureView* ssaoAOSRV(void) {
        return ssao ? ssao->GetAmbientOcclusionSRV() : nullptr;
    }

    float ssaoIntensity(void) {
        return ssaoIntensityValue;
    }

    void ssaoFrameBegin(Diligent::IDeviceContext* ctx) {
        if (!ssao || !ctx || !taaPostFXContext()) {
            return;
        }
        ssao->PrepareResources(device,
                               ctx,
                               taaPostFXContext(),
                               ssaoHalfRes ? ScreenSpaceAmbientOcclusion::FEATURE_FLAG_HALF_RESOLUTION
                                           : ScreenSpaceAmbientOcclusion::FEATURE_FLAG_NONE);

        // DiligentFX's AO history reprojects with the motion vectors the world
        // passes write (they carry the camera flow: taaPrevAnchorDelta is
        // folded into every consumer's prev view-proj), so camera motion is
        // reprojected, not ghosted. The reset control law this call used to run
        // — reset on every moving frame — threw the accumulation away for the
        // whole dolly and left the single-frame AO (half-res sample,
        // frame-index-rotated reconstruction kernel) as the image: AO that
        // boils and crawls while the camera moves. The history now survives
        // camera motion through the seeded defines, and the accumulation only
        // resets when the effect's own inputs change: the settings, or the
        // offscreen chain size (PrepareResources just recreated the history
        // textures, which come back undefined).
        static const bool noHistory = getenv("ENGINE_SSAO_NO_HISTORY") != nullptr;
        static const bool motionLaw = getenv("ENGINE_AO_MOTION_RESET") != nullptr;
        static const bool trace     = getenv("ENGINE_SSAO_RESET_TRACE") != nullptr;
        u32 width = 0, height = 0;
        taaTargetSize(&width, &height);
        static u32 lastWidth = 0, lastHeight = 0;
        const bool resized = width != lastWidth || height != lastHeight;
        lastWidth = width;
        lastHeight = height;
        const bool reset = noHistory ||
                           (motionLaw ? taaCameraMoved() : (ssaoSettingsDirty | resized));
        ssaoSettingsDirty = false;
        if (trace) {
            static int last = -1;
            if (last != (int)reset) {
                last = (int)reset;
                utils::info("ssao: accumulation reset %s (dRot %.5f rad, dEye %.4f m, %ux%u)",
                            reset ? "ACTIVE" : "off",
                            (double)taaCameraRotationRad(), (double)taaPrevEyeDeltaMag(),
                            width, height);
            }
        }
        ssaoAttribs.ResetAccumulation = reset ? TRUE : FALSE;
    }

    bool ssaoExecute(Diligent::IDeviceContext* ctx, Diligent::ITextureView* depthSRV) {
        if (!ssao || !ctx || !depthSRV || !taaPostFXContext() || !taaNormalSRV()) {
            return false;
        }

        ScreenSpaceAmbientOcclusion::RenderAttributes ra;
        ra.pDevice          = device;
        ra.pDeviceContext   = ctx;
        ra.pPostFXContext   = taaPostFXContext();
        ra.pDepthBufferSRV  = depthSRV;
        ra.pNormalBufferSRV = taaNormalSRV();
        ra.pSSAOAttribs     = &ssaoAttribs;

        const POST_FX_EXECUTION_STATUS status = ssao->Execute(ra);
        return status == POST_FX_EXECUTION_STATUS_READY;
    }

}  // namespace engine::renderer::diligent
