#pragma once

// Temporal anti-aliasing on the Diligent backend (DiligentFX's
// PostProcess/TemporalAntiAliasing + PostFXContext — the Tutorial27
// integration shape).
// The world pass ALWAYS renders into an offscreen chain (linear RGBA16F
// color + double-buffered D32 depth + RG16F motion vectors), because the
// TAA pass and the UI passes need those intermediates whether or not
// accumulation runs:
//   world → offscreen color+motion+depth
//         → [TAA enabled: PostFXContext.Execute + TAA.Execute]
//         → blit (linear → sRGB swapchain backbuffer)
//         → imgui/rmlui draw directly on the backbuffer (unchanged)
// Motion vectors follow the DiligentFX PBR convention
// (RenderPBR.psh GetMotionVector): NDC-space
// (currNDC − currJitter) − (prevNDC − prevJitter). The camera jitter comes
// from TAA::GetJitterOffset (Halton 2/3) and is baked into the projection
// every world pass consumes via diligentFrameProj().
// Settings live here: taaSettingsApply is driven by
// RenderBackend::applyGraphicsSettings (taaEnabled / taaWeight in
// settings.json — the graphics settings page).

#include "Defines.h"

#include "Common/interface/BasicMath.hpp"

namespace Diligent {
struct IDeviceContext;
struct ITextureView;
class PostFXContext;
}

namespace engine::renderer::diligent {

// Create the PostFXContext + TAA + blit state (device must exist). Targets
// are created lazily on the first frame / after resize.
void taaInit(void);
void taaDestroy(void);

// applyGraphicsSettings mapping: taaEnabled/taaWeight/renderScale from the
// graphics settings. Safe to call any time (also mid-run from the settings
// page). renderScale sizes the offscreen world chain (the post-world blit
// upsamples it to the backbuffer); UI passes stay at full resolution.
void taaSettingsApply(bool enabled, float stabilityFactor, float casStrength, float renderScale);

bool taaEnabled(void);

// Window resize: drop the offscreen targets (recreated next frame) and let
// the TAA's frame-index continuity check reset the history automatically.
void taaOnResized(void);

// Creates/resizes targets, advances the frame index, picks this frame's
// jitter and REWRITES proj with the jittered projection (the one every
// world pass must consume). Also fills the camera-attribs constant buffer
// (curr + prev) the PostFX/TAA shaders read.
void taaFrameBegin(Diligent::IDeviceContext* ctx, const Diligent::float4x4& view,
        Diligent::float4x4& proj);

// The current frame's jitter (NDC units, matches CameraAttribs.f2Jitter);
// zero while TAA is disabled or not ready yet.
float taaCurrentJitterX(void);
float taaCurrentJitterY(void);

// The PREVIOUS frame's camera attribs (raw row-major storage — consumers
// transpose as their cbuffer convention requires).
const void* taaPrevCameraAttribs(void);

// The per-frame camera translation (eyeCurr - eyePrev) of the last frame,
// metres. The world passes are camera-anchored (the VS subtracts the CURRENT
// anchor and reuses that rel for PrevClipPos), so the prev-clip misses this
// term and every motion vector loses the translation flow — TAA then smears
// the near ground while the camera moves. Each consumer folds it in its own
// matrix convention:
//   - transposed-upload consumers (terrain/props, runtime glslang HLSL):
//       uploaded = Translate_row(delta) * (prev.mViewProj.Transpose())
//     i.e. pre-multiply the transposed matrix with an identity whose bottom
//     row is delta (_41/_42/_43).
//   - raw-upload consumers (DiligentFX PBR, PackMatrixRowMajor):
//       uploaded = Translate_row(delta) * prev.mViewProj
// See TaaDiligent.cpp taaFrameBegin for the derivation.
void taaPrevAnchorDelta(f32 out[3]);

// Offscreen chain size (renderScale * swapchain size, rounded; 0 before the
// first target creation).
void taaTargetSize(u32* width, u32* height);

Diligent::ITextureView* taaColorRTV(void);
Diligent::ITextureView* taaMotionRTV(void);
Diligent::ITextureView* taaNormalRTV(void);
Diligent::ITextureView* taaNormalSRV(void);
Diligent::ITextureView* taaDepthDSV(void);
Diligent::ITextureView* taaDepthSRV(int idx);
Diligent::PostFXContext* taaPostFXContext(void);

// Post-world resolve: PostFXContext::Execute + TAA::Execute (when enabled),
// then the RCAS sharpen pass (when casStrength > 0), then blit the result
// (or the raw scene color when both are off) into the swapchain backbuffer.
void taaWorldResolve(Diligent::IDeviceContext* ctx, Diligent::ITextureView* backRTV);

}
