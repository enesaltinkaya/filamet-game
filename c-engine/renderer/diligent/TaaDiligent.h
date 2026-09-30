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
// (RenderPBR.psh GetMotionVector): NDC-space total apparent motion
// currNDC − prevNDC, including the per-frame camera-jitter delta. The
// camera jitter comes from TAA::GetJitterOffset (Halton 2/3) and is baked
// into the projection every world pass consumes via diligentFrameProj().
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

// Allocate the offscreen chain + post-FX targets at renderer init (menu
// time) so the first world frame skips the device allocations. No render
// happens — the TAA history stays pristine (frame-index continuity resets
// it on the first real frame).
void taaWarmup(void);

// applyGraphicsSettings mapping: taaEnabled/taaWeight/renderScale from the
// graphics settings. Safe to call any time (also mid-run from the settings
// page). renderScale sizes the offscreen world chain (the post-world blit
// upsamples it to the backbuffer); UI passes stay at full resolution.
void taaSettingsApply(bool enabled, float stabilityFactor, float casStrength, float renderScale);

bool taaEnabled(void);

// The running frame index (the same counter the jitter phase and the stage
// dump use) — other passes gate their per-frame dumps on it so dumps from
// different passes share frame numbers.
u32 taaFrameIndex(void);

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

// Per-frame camera motion (measured in taaFrameBegin, NDC-pixel thresholds:
// eye translation at the near-plane depth proxy + view-forward rotation,
// ENGINE_AO_MOTION_RESET px/frame, default 0.1, 0 = never "moved"). True on
// the first measured frame. Drives the SSAO accumulation reset: the AO
// history only receives the object motion vectors, so camera motion ghosts
// it (DiligentFX SSAO_MAX_HISTORY_LENGTH: "reduce noise ... increased
// ghosting") unless the accumulation resets on moving frames.
bool taaCameraMoved(void);

// This frame's view-forward rotation (radians, 0 on the first frame).
f32 taaCameraRotationRad(void);

// Last measured per-frame eye translation magnitude (metres).
f32 taaPrevEyeDeltaMag(void);

// Per-frame image displacement in pixels of a world point at the jitter
// gate's reference depth: (eye translation / ENGINE_TAA_MOTION_Z + view
// rotation) * focal. The gate fades the sub-pixel Halton offset out over
// [0.25 * ENGINE_TAA_MOTION_PX, ENGINE_TAA_MOTION_PX]: 0 px/frame = full
// jitter, >= ENGINE_TAA_MOTION_PX = unjittered frame.
f32 taaCameraMotionPx(void);

// This frame's camera world axes (unit vectors, the view matrix's columns):
// forward = col 3, right = col 1, up = fwd x right. For the stage dump's
// per-pixel camera-flow computation.
void taaCameraBasis(f32 fwd[3], f32 right[3], f32 up[3]);

// The PREVIOUS frame's camera attribs (raw row-major storage — consumers
// transpose as their cbuffer convention requires).
const void* taaPrevCameraAttribs(void);

// This frame's camera attribs (raw storage, as uploaded to the cbuffer).
const void* taaCurrCameraAttribs(void);

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
