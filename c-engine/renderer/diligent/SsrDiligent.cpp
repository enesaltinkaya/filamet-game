#include "renderer/diligent/SsrDiligent.h"

#include "renderer/diligent/DiligentRenderer.h"
#include "renderer/diligent/TaaDiligent.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "PostProcess/Common/interface/PostFXContext.hpp"
#include "PostProcess/Common/interface/PostFXRenderTechnique.hpp"
#include "PostProcess/ScreenSpaceReflection/interface/ScreenSpaceReflection.hpp"

namespace Diligent {
    namespace HLSL {
#include "Shaders/Common/public/ShaderDefinitions.fxh"
#include "Shaders/PostProcess/ScreenSpaceReflection/public/ScreenSpaceReflectionStructures.fxh"
    }  // namespace HLSL
}  // namespace Diligent

namespace engine::renderer::diligent {

    using namespace Diligent;

    static std::unique_ptr<ScreenSpaceReflection> ssr;
    static HLSL::ScreenSpaceReflectionAttribs ssrAttribs{};
    static bool ssrEnabled      = false;
    static float ssrStrengthVal = 1.0f;

    void ssrInit(void) {
        if (!device) {
            return;
        }
        ScreenSpaceReflection::CreateInfo ci;
        ci.EnableAsyncCreation               = false;
        ssr                                  = std::make_unique<ScreenSpaceReflection>(device, ci);
        ssrAttribs                           = HLSL::ScreenSpaceReflectionAttribs{};
        ssrAttribs.RoughnessThreshold        = 0.2f;
        ssrAttribs.MostDetailedMip           = 0;
        ssrAttribs.IsRoughnessPerceptual     = TRUE;
        ssrAttribs.RoughnessChannel          = 3;
        ssrAttribs.MaxTraversalIntersections = 128;
    }

    void ssrDestroy(void) {
        ssr.reset();
        ssrEnabled = false;
    }

    void ssrSettingsApply(bool enabled, float strength) {
        ssrEnabled     = enabled;
        ssrStrengthVal = strength;
    }

    bool ssrOn(void) {
        return ssrEnabled;
    }

    bool ssrReady(void) {
        return ssr != nullptr;
    }

    Diligent::ITextureView* ssrRadianceSRV(void) {
        return ssr ? ssr->GetSSRRadianceSRV() : nullptr;
    }

    float ssrStrength(void) {
        return ssrStrengthVal;
    }

    void ssrFrameBegin(Diligent::IDeviceContext* ctx) {
        if (!ssr || !ctx || !taaPostFXContext()) {
            return;
        }
        ssr->PrepareResources(device,
                              ctx,
                              taaPostFXContext(),
                              ScreenSpaceReflection::FEATURE_FLAG_NONE);
    }

    bool ssrExecute(Diligent::IDeviceContext* ctx,
                    Diligent::ITextureView* colorSRV,
                    Diligent::ITextureView* depthSRV,
                    Diligent::ITextureView* motionSRV) {
        if (!ssr || !ctx || !colorSRV || !depthSRV || !motionSRV || !taaPostFXContext() ||
            !taaNormalSRV()) {
            return false;
        }

        ScreenSpaceReflection::RenderAttributes ra;
        ra.pDevice            = device;
        ra.pDeviceContext     = ctx;
        ra.pPostFXContext     = taaPostFXContext();
        ra.pColorBufferSRV    = colorSRV;
        ra.pDepthBufferSRV    = depthSRV;
        ra.pNormalBufferSRV   = taaNormalSRV();
        ra.pMaterialBufferSRV = taaNormalSRV();
        ra.pMotionVectorsSRV  = motionSRV;
        ra.pSSRAttribs        = &ssrAttribs;

        const POST_FX_EXECUTION_STATUS status = ssr->Execute(ra);
        return status == POST_FX_EXECUTION_STATUS_READY;
    }

}  // namespace engine::renderer::diligent
