#pragma once

namespace filament {
class Material;
class MaterialInstance;
class Texture;
}  // namespace filament

namespace engine::terrain {

// Loads the splat material + splat tile / style detail texture arrays from a
// manifest (see scripts/build-terrain.py). Must be called after rendererInit
// and before gltfLoad of the terrain model.
bool terrainInit(const char* manifestPath);

// Swaps the material of every terrain_chunk_* renderable in the currently
// loaded glTF asset to the terrain material instance. Call after gltfLoad.
void terrainApplyToAsset(void);

void terrainDestroy(void);

}  // namespace engine::terrain
