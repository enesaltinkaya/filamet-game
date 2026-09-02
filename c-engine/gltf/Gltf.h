#pragma once

#include "Defines.h"

namespace engine::gltf {

bool gltfInit(void);
bool gltfLoad(const char* pakPath);
void gltfUpdate(double elapsedSeconds);
void gltfFrameCamera(void);
void gltfDestroy(void);

// World-space bounding box of the loaded asset; false if nothing is loaded.
bool gltfBoundingBox(f32 min[3], f32 max[3]);

// Fills `out` (up to cap) with handles of meshes whose node name starts with
// `prefix`. Handles are backend-defined (filament renderable entity id /
// diligent scene node index) and only meaningful to the matching terrain path.
// Returns the number of meshes found (may exceed cap).
size_t gltfEntitiesNamed(const char* prefix, u64* out, size_t cap);

}  // namespace engine::gltf
