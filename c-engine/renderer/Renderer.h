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

// Camera: eye/center/up in world space (lookAt semantics, world up +Y, camera
// forward = normalize(center - eye)). Projections use a 60 degree vertical fov
// with 0.1..20000 depth (matches the old filament setup).
void rendererCameraLookAt(const f32 eye[3], const f32 center[3], const f32 up[3]);
void rendererCameraGet(f32 pos[3], f32 forward[3]);

// Scene lighting (sun = directional). Both are backend-agnostic: filament maps
// them to LightManager/IndirectLight, diligent feeds its PBR/splat shaders.
void rendererSetSun(const f32 direction[3], const f32 color[3], f32 intensity);
void rendererSetAmbient(const f32 color[3], f32 intensity);

// Atmospheric distance fog on the world view (see RenderBackend::setFog).
void rendererSetFog(const f32 color[3], f32 density);

}  // namespace engine::renderer
