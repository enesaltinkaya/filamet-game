#include "renderer/diligent/BloomDiligent.h"

#include "renderer/diligent/DiligentRenderer.h"
#include "renderer/diligent/TaaDiligent.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "PostProcess/Common/interface/PostFXContext.hpp"
#include "PostProcess/Common/interface/PostFXRenderTechnique.hpp"
#include "PostProcess/Bloom/interface/Bloom.hpp"

namespace Diligent {
namespace HLSL {
#include "Shaders/Common/public/ShaderDefinitions.fxh"
#include "Shaders/PostProcess/Bloom/public/BloomStructures.fxh"
}
}

namespace engine::renderer::diligent {

using namespace Diligent;

static std::unique_ptr<Bloom> bloom;
static HLSL::BloomAttribs bloomAttribs{};
static bool bloomOnFlag = false;

void bloomInit(void) {
    if (!device) {
        return;
    }
    Bloom::CreateInfo ci;
    ci.EnableAsyncCreation = false;
    bloom = std::make_unique<Bloom>(device, ci);
    bloomAttribs = HLSL::BloomAttribs{};
    bloomAttribs.Intensity = 0.15f;
    bloomAttribs.Threshold = 0.5f;
    bloomAttribs.SoftTreshold = 0.125f;
    bloomAttribs.Radius = 0.75f;
}

void bloomDestroy(void) {
    bloom.reset();
    bloomOnFlag = false;
}

void bloomSettingsApply(bool enabled) {
    bloomOnFlag = enabled;
}

bool bloomOn(void) {
    return bloomOnFlag;
}

bool bloomReady(void) {
    return bloom != nullptr;
}

Diligent::ITextureView* bloomSRV(void) {
    return bloom ? bloom->GetBloomTextureSRV() : nullptr;
}

void bloomFrameBegin(Diligent::IDeviceContext* ctx) {
    if (!bloom || !ctx || !taaPostFXContext()) {
        return;
    }
    bloom->PrepareResources(device, ctx, taaPostFXContext(),
            Bloom::FEATURE_FLAG_NONE);
}

bool bloomExecute(Diligent::IDeviceContext* ctx, Diligent::ITextureView* colorSRV) {
    if (!bloom || !ctx || !colorSRV || !taaPostFXContext()) {
        return false;
    }

    Bloom::RenderAttributes ra;
    ra.pDevice = device;
    ra.pDeviceContext = ctx;
    ra.pPostFXContext = taaPostFXContext();
    ra.pColorBufferSRV = colorSRV;
    ra.pBloomAttribs = &bloomAttribs;

    const POST_FX_EXECUTION_STATUS status = bloom->Execute(ra);
    return status == POST_FX_EXECUTION_STATUS_READY;
}

}
