#pragma once

#include "Defines.h"

namespace engine::renderer {

enum class Backend : u8 { Filament = 0, Diligent = 1 };

// Active backend (valid after rendererInit).
Backend rendererBackend(void);
const char* rendererBackendName(void);

bool rendererInit(const char* title, u32 width, u32 height);
void rendererDraw(void);
void rendererDestroy(void);

// Camera: eye/center/up in ABSOLUTE world space (double precision — the
// camera position is the world anchor; the renderer re-expresses the whole
// frame relative to it for float precision, see FilamentRenderer). lookAt
// semantics, world up +Y, camera forward = normalize(center - eye).
// Projections use a 60 degree vertical fov with 0.1..20000 depth.
void rendererCameraLookAt(const double eye[3], const double center[3], const double up[3]);
// f32 overload: convenience for one-shot (static) vantages — far-from-origin
// per-frame cameras must use the double overload (the eye is the world anchor).
void rendererCameraLookAt(const f32 eye[3], const f32 center[3], const f32 up[3]);
void rendererCameraGet(f32 pos[3], f32 forward[3]);

// The world anchor (xz, metres) all renderables are currently placed relative
// to — the last cameraLookAt's eye. World-space code (model placement, tile
// transforms) subtracts it before handing floats to the backend.
double rendererWorldAnchorX(void);
double rendererWorldAnchorZ(void);

// Scene lighting (sun = directional). Both are backend-agnostic: filament maps
// them to LightManager/IndirectLight, diligent feeds its PBR/splat shaders.
void rendererSetSun(const f32 direction[3], const f32 color[3], f32 intensity);
void rendererSetAmbient(const f32 color[3], f32 intensity);

// Atmospheric distance fog on the world view (see RenderBackend::setFog).
void rendererSetFog(const f32 color[3], f32 density);

}  // namespace engine::renderer
