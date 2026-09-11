#pragma once

// Only diligent-path files include this.

#include "Defines.h"

#include "Common/interface/BasicMath.hpp"

namespace Diligent {
struct ISampler;
struct ITextureView;
}

namespace engine::renderer::diligent {

// DiligentFX ShadowMapManager-backed cascaded shadow pass (ShadowDiligent.cpp).
//
// The module owns the shadow map texture atlas (one 2D array, one slice per
// cascade), the cascade distribution (DistributeCascades against the
// camera-anchored view + unjittered projection) and the master LightAttribs
// block the world shadow passes copy into their frame cbuffers (matrices
// already TRANSPOSED — the runtime glslang convention, see GltfDiligent.cpp
// fillFrameAttribs).
//
// The cascade depth draws: the shadow module calls the caster passes once
// per cascade (the PBR glTF pass for the player character; the splat terrain
// depth re-render for the world — splatTerrainShadowDrawDiligent).
//
// Mode/quality come from rendererGraphicsSettings() (shadowMode 0=off..4,
// shadowQuality 0=low..2=high). Any change bumps the generation counter
// — the geometry passes poll it and rebuild their pipelines (the sampling
// macros are compile-time).

// Lazy init + per-frame CPU work (cascade distribution). Safe to call
// before the device exists (no-op).
void shadowDiligentUpdateFrame(void);

// GPU work: render all cascades (the PBR caster draws the player) +
// ConvertToFilterable for VSM/EVSM. Binds its own render
// targets; the caller must re-bind the world targets afterwards.
void shadowDiligentRenderCascades(void);

void shadowDiligentDestroy(void);

// False while shadows are off (mode == 0) or the pass failed to init.
bool shadowDiligentActive(void);
// Bumped whenever mode/quality changes (and at first activation): the
// geometry passes rebuild their pipelines when it moves.
u32  shadowDiligentGeneration(void);

// Current mode (1=PCF..4=EVSM4; 0 when off) + the active tier's PCF filter
// size (the lit shaders' PCF_FILTER_SIZE compile-time macro).
int  shadowDiligentMode(void);
int  shadowDiligentPcfFilterSize(void);

// The active tier's shadow distance in metres (0 while the pass is not
// ready). The cascade box (StabilizeExtents bounding sphere of the padded
// frustum) extends well past this, so the lit passes fade the shadow
// contribution to lit by this view-space depth (f4ShadowFade in their frame
// cbuffers) — it is the effective receiver cutoff.
float shadowDiligentTierDistance(void);
float shadowDiligentFarPadS(void);
u64 shadowDiligentTraceFrame(void);

// Master LightAttribs (ShadowMapAttribs included; matrices transposed for
// the runtime shaders). Copies into the passes' frame cbuffers.
const void* shadowDiligentLightAttribs(void);
// The SRV the lit passes bind (raw depth for PCF, filterable for VSM/EVSM —
// mode-aware).
Diligent::ITextureView* shadowDiligentShadowSRV(void);
// The sampler matching shadowDiligentShadowSRV (comparison for PCF, linear
// for the filterable modes) — bound into the passes' PRS as the
// g_tex2DShadowMap_sampler / g_tex2DFilterableShadowMap_sampler static.
Diligent::ISampler* shadowDiligentShadowSampler(void);

// Caster-side hardware rasterizer depth bias for the depth-pass PSOs
// (SlopeScaledDepthBias / DepthBias / DepthBiasClamp, in the graphics-API
// units: the D32 slope factor is dimensionless texels-of-slope, the
// constant is in r = 2^-23 units). Env-tunable, read once at PSO creation:
// ENGINE_SHADOW_SLOPE_BIAS, ENGINE_SHADOW_DEPTH_BIAS, ENGINE_SHADOW_BIAS_CLAMP.
void shadowDiligentCasterBias(float& slopeBias, float& constBias, float& biasClamp);

// PBR (glTF) receiver state: the one cascade that covers the player
// (feet + torso, render space). The untransposed world->light-projection in
// the PBR row-vector convention (the same matrix the caster draw renders
// with), the depth-atlas slice index, and the receiver-side NDC depth bias
// (casters draw without a rasterizer bias, so the receiver must subtract it
// or self-shadows). Null while the pass is not ready (shadows off).
const Diligent::float4x4* shadowDiligentPbrWorldToLightProj(void);
float shadowDiligentPbrSlice(void);
float shadowDiligentPbrDepthBias(void);

}
