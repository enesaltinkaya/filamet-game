#include "renderer/diligent/SsaoDiligent.h"

#include "renderer/diligent/DiligentRenderer.h"
#include "renderer/diligent/TaaDiligent.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "PostProcess/Common/interface/PostFXContext.hpp"
#include "PostProcess/Common/interface/PostFXRenderTechnique.hpp"
#include "PostProcess/ScreenSpaceAmbientOcclusion/interface/ScreenSpaceAmbientOcclusion.hpp"

namespace Diligent {
namespace HLSL {
#include "Shaders/Common/public/ShaderDefinitions.fxh"
#include "Shaders/PostProcess/ScreenSpaceAmbientOcclusion/public/ScreenSpaceAmbientOcclusionStructures.fxh"
}
}

namespace engine::renderer::diligent {

using namespace Diligent;

static std::unique_ptr<ScreenSpaceAmbientOcclusion> ssao;
static HLSL::ScreenSpaceAmbientOcclusionAttribs ssaoAttribs{};
static bool ssaoEnabled = false;
static float ssaoIntensityValue = 1.0f;

void ssaoInit(void) {
    if (!device) {
        return;
    }
    ScreenSpaceAmbientOcclusion::CreateInfo ci;
    ci.EnableAsyncCreation = false;
    ssao = std::make_unique<ScreenSpaceAmbientOcclusion>(device, ci);
    ssaoAttribs = HLSL::ScreenSpaceAmbientOcclusionAttribs{};
}

void ssaoDestroy(void) {
    ssao.reset();
    ssaoEnabled = false;
}

void ssaoSettingsApply(bool enabled, float radius, int algorithm, float intensity) {
    ssaoEnabled = enabled;
    ssaoIntensityValue = intensity;
    ssaoAttribs.EffectRadius = radius;
    ssaoAttribs.Algorithm = (uint)algorithm;
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
    ssao->PrepareResources(device, ctx, taaPostFXContext(),
            ScreenSpaceAmbientOcclusion::FEATURE_FLAG_HALF_RESOLUTION);
}

bool ssaoExecute(Diligent::IDeviceContext* ctx, Diligent::ITextureView* depthSRV) {
    if (!ssao || !ctx || !depthSRV || !taaPostFXContext() || !taaNormalSRV()) {
        return false;
    }

    ScreenSpaceAmbientOcclusion::RenderAttributes ra;
    ra.pDevice = device;
    ra.pDeviceContext = ctx;
    ra.pPostFXContext = taaPostFXContext();
    ra.pDepthBufferSRV = depthSRV;
    ra.pNormalBufferSRV = taaNormalSRV();
    ra.pSSAOAttribs = &ssaoAttribs;

    const POST_FX_EXECUTION_STATUS status = ssao->Execute(ra);
    return status == POST_FX_EXECUTION_STATUS_READY;
}

}
