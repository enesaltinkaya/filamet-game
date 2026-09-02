#pragma once

// Internal split of the terrain module: Terrain.cpp parses the manifest and
// decodes the KTX2/BC7 arrays (backend-agnostic); each backend uploads them
// and owns its material/shader state.

#include "Defines.h"

#include <cstddef>
#include <vector>

namespace Diligent {
struct IDeviceContext;
}

namespace Diligent::GLTF {
struct Model;
struct ModelTransforms;
}

namespace engine::terrain {

enum class TerrainArrayKind : u8 { SplatTiles, StyleAlbedo, StyleNormal, DefaultAlbedo, DefaultNormal };

// one decoded KTX2 file set → a BC7 2D array; layers[layer][mip]
struct TerrainLevelBlocks {
    u8* blocks;  // malloc'd BC7 blocks
    size_t byteCount;
    u32 width;
    u32 height;
};
struct TerrainDecodedArray {
    std::vector<std::vector<TerrainLevelBlocks>> layers;
    bool srgb = false;
};

struct TerrainParams {
    static constexpr int kMaxGroups = 3;
    static constexpr int kMaxTiles = 100;

    int tileLayer[kMaxGroups][kMaxTiles];
    int styleRemap[12];
    float sandHeight = 20.0f;
    float sandFade = 15.0f;
    float snowHeight = 800.0f;
    float snowFade = 120.0f;
    float cliffSlope = 0.45f;
    float cliffFade = 0.12f;
    float styleTiling = 1.0f;
};

// backend lifecycle (called by Terrain.cpp in this order)
bool terrainStartFilament(const char* materialPath);  // material from pak (.filamat)
bool terrainStartDiligent(void);                      // embedded HLSL splat shader
bool terrainArrayFilament(TerrainArrayKind kind, TerrainDecodedArray& array);  // consumes blocks
bool terrainArrayDiligent(TerrainArrayKind kind, TerrainDecodedArray& array);  // consumes blocks
bool terrainFinishFilament(const TerrainParams& params);
bool terrainFinishDiligent(const TerrainParams& params);
void terrainApplyFilament(void);
void terrainApplyDiligent(void);
void terrainDestroyFilament(void);
void terrainDestroyDiligent(void);

// diligent draw path, invoked by the gltf world hook when the terrain material
// covers the loaded model
bool terrainDiligentOwnsDrawing(void);
void terrainDiligentDrawWorld(Diligent::IDeviceContext* ctx, const Diligent::GLTF::Model& model,
        const Diligent::GLTF::ModelTransforms& transforms);

void freeDecodedArray(TerrainDecodedArray& array);

}  // namespace engine::terrain
