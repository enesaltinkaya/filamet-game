#pragma once

#include "Defines.h"

namespace engine::gltf {

bool gltfInit(void);
bool gltfLoad(const char* pakPath);
void gltfUpdate(double elapsedSeconds);
void gltfFrameCamera(void);
void gltfDestroy(void);

// Move the loaded instance so the min corner of its local-space AABB (feet for
// character assets) lands at world (x, y, z). false if nothing is loaded.
bool gltfPlaceAt(f32 x, f32 y, f32 z);

// World-space bounding box of the loaded asset; false if nothing is loaded.
bool gltfBoundingBox(f32 min[3], f32 max[3]);


}  // namespace engine::gltf
