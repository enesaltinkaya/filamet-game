#pragma once

#include "Defines.h"

namespace Diligent {
struct IDeviceContext;
struct ITextureView;
}

namespace engine::renderer::diligent {

void ssrInit(void);
void ssrDestroy(void);

void ssrSettingsApply(bool enabled, float strength);

bool ssrOn(void);
bool ssrReady(void);

void ssrFrameBegin(Diligent::IDeviceContext* ctx);

bool ssrExecute(Diligent::IDeviceContext* ctx, Diligent::ITextureView* colorSRV,
        Diligent::ITextureView* depthSRV, Diligent::ITextureView* motionSRV);

Diligent::ITextureView* ssrRadianceSRV(void);

float ssrStrength(void);

}
