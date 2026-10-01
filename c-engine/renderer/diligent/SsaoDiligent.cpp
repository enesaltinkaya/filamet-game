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

    static std::unique_ptr<ScreenSpaceAmbientOcclusion> ssao;
    static HLSL::ScreenSpaceAmbientOcclusionAttribs ssaoAttribs{};
    static bool ssaoEnabled         = false;
    static bool ssaoHalfRes         = true;
    static float ssaoIntensityValue = 1.0f;

    static void ssaoApplyEnvAttribs(void);

    void ssaoInit(void) {
        if (!device) {
            return;
        }
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

        // DiligentFX's AO history reprojects with the OBJECT motion vectors
        // only (PostFXContext closest-MV: zero for static terrain/props), so
        // camera motion never reprojects it — every frame keeps
        // 1 - 1/history (~94%) of last frame's screen-space AO and the
        // occlusion trails the geometry (AO ghosting while the camera moves,
        // docs/lessons.md 2026-09-28). Reset on moving frames; static frames
        // keep accumulating so the half-res AO stays denoised.
        static const bool noHistory = getenv("ENGINE_SSAO_NO_HISTORY") != nullptr;
        static const bool trace     = getenv("ENGINE_SSAO_RESET_TRACE") != nullptr;
        const bool reset = noHistory ? true : taaCameraMoved();
        if (trace) {
            static int last = -1;
            if (last != (int)reset) {
                last = (int)reset;
                utils::info("ssao: accumulation reset %s (dRot %.5f rad, dEye %.4f m)",
                            reset ? "ACTIVE" : "off",
                            (double)taaCameraRotationRad(), (double)taaPrevEyeDeltaMag());
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
