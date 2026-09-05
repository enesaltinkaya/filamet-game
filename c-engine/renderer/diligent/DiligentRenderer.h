#pragma once

// Diligent Engine core objects + the internal hooks its frame loop calls.
// Only diligent-path files include this.

#include "Defines.h"

#include "Common/interface/BasicMath.hpp"

namespace Diligent {
struct IRenderDevice;
struct IDeviceContext;
struct ISwapChain;
}  // namespace Diligent

namespace engine::renderer::diligent {

extern Diligent::IRenderDevice* device;
extern Diligent::IDeviceContext* context;
extern Diligent::ISwapChain* swapChain;

// True when the world pass recorded draws this frame (drives the UI pass'
// load op: LOAD over the world, CLEAR otherwise — e.g. the main menu).
// Set by worldDraw.
bool diligentWorldDrew(void);
void setWorldDrew(bool drew);

// Hooks invoked by the frame loop between pass begin and present
// (implemented in GltfDiligent.cpp / GuiDiligent.cpp; safe to be no-ops).
void worldDraw(Diligent::IDeviceContext* ctx);
void guiDraw(Diligent::IDeviceContext* ctx);
void guiOnBackendDestroy(void);  // release gui resources before device dies

// The rmlui pass (crmlui wrapper, Diligent render half — see
// gui/rmlui/RmluiDiligent.h): the wrapper's callbacks queue geometry during
// rmlUpdate/rmlRenderVulkan (postUpdate); this hook uploads + draws it on
// top of the world/ImGui passes. No-op while no rmlui geometry is queued.
void rmluiDraw(Diligent::IDeviceContext* ctx);
void rmluiOnBackendDestroy(void);  // release rmlui GPU state before device dies

// Current frame camera/light state (updated by the frame loop before worldDraw)
const Diligent::float4x4& diligentFrameView(void);
const Diligent::float4x4& diligentFrameProj(void);

// The world anchor (f64 camera eye): the origin of the render space. The view
// matrix is rotation-only; passes subtract this from their f64 world state
// and round the small difference to f32.
void diligentWorldAnchor(f64 out[3]);
const f32* diligentSunDirection(void);
const f32* diligentSunColor(void);
f32 diligentSunIntensity(void);
const f32* diligentAmbientColor(void);
f32 diligentAmbientIntensity(void);

}  // namespace engine::renderer::diligent
