#pragma once

namespace filament::gltfio {
class AssetLoader;
class FilamentAsset;
}

namespace engine::gltf {
extern filament::gltfio::FilamentAsset* asset;

bool gltfInit(void);
bool gltfLoad(const char* pakPath);
void gltfUpdate(double elapsedSeconds);
void gltfFrameCamera(void);
void gltfDestroy(void);

// Fills `out` (up to cap) with entities whose name component starts with
// `prefix`. Returns the number of entities found (may exceed cap).
size_t gltfEntitiesNamed(const char* prefix, utils::Entity* out, size_t cap);
}  // namespace engine::gltf
