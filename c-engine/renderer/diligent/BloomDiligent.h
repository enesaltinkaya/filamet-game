#pragma once

#include "Defines.h"

namespace Diligent {
struct IDeviceContext;
struct ITextureView;
}

namespace engine::renderer::diligent {

void bloomInit(void);
void bloomDestroy(void);

void bloomSettingsApply(bool enabled);

bool bloomOn(void);
bool bloomReady(void);

void bloomFrameBegin(Diligent::IDeviceContext* ctx);

bool bloomExecute(Diligent::IDeviceContext* ctx, Diligent::ITextureView* colorSRV);

Diligent::ITextureView* bloomSRV(void);

}
