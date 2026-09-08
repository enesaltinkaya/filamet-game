#pragma once

#include "Defines.h"

namespace Diligent {
struct IDeviceContext;
struct ITextureView;
}

namespace engine::renderer::diligent {

void ssaoInit(void);
void ssaoDestroy(void);

void ssaoSettingsApply(bool enabled, float radius, int algorithm, float intensity);

bool ssaoOn(void);
bool ssaoReady(void);

void ssaoFrameBegin(Diligent::IDeviceContext* ctx);

bool ssaoExecute(Diligent::IDeviceContext* ctx, Diligent::ITextureView* depthSRV);

Diligent::ITextureView* ssaoAOSRV(void);

float ssaoIntensity(void);

}
