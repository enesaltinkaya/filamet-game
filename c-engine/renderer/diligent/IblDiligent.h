#pragma once

#include "Defines.h"

namespace Diligent {
struct ITexture;
}

namespace engine::renderer::diligent {

void iblDiligentInit(void);
void iblDiligentDestroy(void);

bool iblDiligentReady(void);
Diligent::ITexture* iblDiligentIrradianceCube(void);
Diligent::ITexture* iblDiligentPrefilteredCube(void);
f32 iblDiligentPrefilteredLastMip(void);
const char* iblDiligentEnvName(void);

void iblDiligentCycleNext(void);
void iblDiligentCyclePrev(void);

// Global IBL intensity (scales the diffuse + specular IBL together; consumed
// as each render path's IBLScale). Env override: ENGINE_IBL_INTENSITY.
f32 iblDiligentGetIntensity(void);
void iblDiligentSetIntensity(f32 intensity);

// Specular-only IBL attenuation: scales the prefiltered env the spec lobe
// samples, the diffuse irradiance keeps full strength. Changing it re-runs
// the prefilter pass. Env override: ENGINE_IBL_SPEC_INTENSITY (read at
// init). The specular env copy also luminance-clamps the baked sun disk
// (ENGINE_IBL_SPEC_CLAMP, <= 0 disables) — the analytic sun supplies that
// energy; the disk prefiltered into the lobe read as over-shiny sheen.
f32 iblDiligentGetSpecularIntensity(void);
void iblDiligentSetSpecularIntensity(f32 intensity);

}
