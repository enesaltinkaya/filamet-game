#pragma once
#include "Defines.h"

/*
 * HeightmapTerrainRender
 * ----------------------
 * Backend-agnostic API of the heightmap terrain render pass (the GPU half
 * of ecs/system/heightmap/HeightmapTerrain). The game registers the
 * per-world look (biome/climate textures + bounds + thresholds) at world
 * load; the active render backend draws the streaming tiles each frame.
 *
 * Dispatch: every entry point forwards to the active backend's half
 * (filament now; diligent in phase 6 of plans/azgaar-terrain.md). The
 * per-frame call comes from the backend's draw (see FilamentRenderer);
 * the registration calls come from the game (main thread).
 */

namespace engine {

// Per-world look data for the heightmap terrain pass. Texture pixels are
// packed RGBA8 by the game (from its world data); the engine copies them
// and owns the data from this call on (the game may free its temporaries
// right after returning).
struct HeightmapTerrainLook {
    // Biome tint (RGB, A=255; authored display colours -> sRGB on the GPU).
    // Null or 0 dims = unavailable (climate blending stays off).
    const u8* biomeColorPixels;
    u32 biomeColorW, biomeColorH;
    // Climate field (byte-encoded scalars -> UNORM on the GPU):
    // R = temperature + 64, G = precipitation, B = coast + 11, A = biome id.
    // Null or 0 dims = unavailable (climate blending stays off).
    const u8* climatePixels;
    u32 climateW, climateH;
    // Map bounds in world metres (the map is centred at the world origin).
    float mapMinX, mapMinZ, mapMaxX, mapMaxZ;
    // Peak land elevation in metres (drives the altitude rock band).
    float maxLandHeightM;
    // Blend thresholds: snow isotherm band (deg C) + beach height (m).
    float snowLoC, snowHiC, beachHeightM;
    // Climate blending (biome tint / snow / beach) enabled.
    bool climateEnabled;
};

// Register the per-world look (main thread). Pass a null pointer to clear
// the look without releasing the pass (e.g. world load failed).
void heightmapTerrainRenderRegisterLook(const HeightmapTerrainLook* look);

// Drop the per-world look (world release; main thread): per-world textures
// are freed and climate blending turns off.
void heightmapTerrainRenderReleaseLook(void);

// Debug views (see the material's debugView parameter for the modes):
//   0 = off, 1 = periodic height ramp (one hue cycle per 256 m; matches
//     the CPU height grid exactly since the lattice lifts it),
//   2 = raw biome-colour texture (UV / texture registration check).
void heightmapTerrainRenderSetDebugView(u32 mode);

// One-frame pass update (tile sync + upload budget). Called by the active
// render backend once per frame, before its scene render. No-op when no
// active HeightmapTerrain exists.
void heightmapTerrainRenderUpdate(void);

// Steady-state stats of the render pass (phase-5 acceptance; valid once the
// pass has been running past its warmup):
//   renderAvgMs = per-frame cost of the render half, averaged over 1000
//                 frames after a 120-frame warmup
//   gpuTiles /  gpuBytes = resident per-tile VBOs + the shared lattice IBO
// Zeroed before the warmup completes or when no backend implements them.
struct HeightmapTerrainRenderStats {
    double renderAvgMs;
    u32    gpuTiles;
    size_t gpuBytes;
};
HeightmapTerrainRenderStats heightmapTerrainRenderStats(void);

// Destroy all terrain GPU state (backend teardown).
void heightmapTerrainRenderDestroy(void);
}  // namespace engine
