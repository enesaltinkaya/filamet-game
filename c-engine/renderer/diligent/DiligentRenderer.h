#pragma once

// Only diligent-path files include this.

#include "Defines.h"

#include "Common/interface/BasicMath.hpp"
#include "image/Image.h"

namespace Diligent {
struct IRenderDevice;
struct IDeviceContext;
struct ISwapChain;
struct ITexture;
}

namespace engine::renderer::diligent {

extern Diligent::IRenderDevice* device;
extern Diligent::IDeviceContext* context;
extern Diligent::ISwapChain* swapChain;

// True when the world pass recorded draws this frame (drives the UI pass'
// load op: LOAD over the world, CLEAR otherwise — e.g. the main menu).
// Set by worldDraw.
bool diligentWorldDrew(void);
void setWorldDrew(bool drew);

// GPU time of the last COMPLETED frame in ns: a QUERY_TYPE_DURATION query
// (two bottom-of-pipe timestamps, see DiligentRenderer.cpp) spans draw()
// begin->Present, so it is the whole-frame GPU time, like the old engine's
// rendererElapsedGpu. Results arrive ~1 frame late; 0 until the first
// sample, and forever 0 when the driver lacks timestamp support.
double diligentGpuTimeNs(void);
bool diligentGpuTimeSupported(void);

// Hooks invoked by the frame loop between pass begin and present
// (implemented in GltfDiligent.cpp / GuiDiligent.cpp; safe to be no-ops).
void worldDraw(Diligent::IDeviceContext* ctx);
// CSM caster: draws the character into one cascade's depth atlas (light
// view-proj in row-major PBR packing, i.e. UNtransposed; no-ops while the
// model/TAA chain is absent).
void gltfDiligentShadowDraw(Diligent::IDeviceContext* ctx,
                           const Diligent::float4x4& lightViewProjRowMajor,
                           Diligent::ITextureView* cascadeDSV);
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
const Diligent::float4x4& diligentBaseProj(void);

// The world anchor (f64 camera eye): the origin of the render space. The view
// matrix is rotation-only; passes subtract this from their f64 world state
// and round the small difference to f32.
void diligentWorldAnchor(f64 out[3]);
const f32* diligentSunDirection(void);
const f32* diligentSunColor(void);
f32 diligentSunIntensity(void);
const f32* diligentAmbientColor(void);
f32 diligentAmbientIntensity(void);

// Create an immutable sampling texture from a decoded ktx2 (utils::imageLoad:
// basis sources transcode to BC4/BC5/BC7 by channel count, and libktx honours
// the DFD's sRGB flag by picking the SRGB block variants — albedo lands in
// BC7_SRGB, normals in BC7_UNORM) and transition it to shader-resource state.
// The baked mip chain is uploaded verbatim — no GPU mipgen, and block
// formats cost 1/4 the VRAM/bandwidth of the RGBA8 decode. The vkFormat →
// Diligent format mapping lives in the .cpp. srgbForUncompressed only
// affects plain RGBA8 payloads (libktx reports those UNORM even for sRGB
// DFDs — the rmlui pass passes true, the terrain table's flag flows through).
// Returns nullptr on failure; caller owns the returned pointer (AddRef'd).
Diligent::ITexture* diligentCreateImageTexture(const utils::Image& image, const char* name,
        bool srgbForUncompressed = false);

}
