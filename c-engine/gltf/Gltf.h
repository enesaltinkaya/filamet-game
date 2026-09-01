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
}  // namespace engine::gltf
