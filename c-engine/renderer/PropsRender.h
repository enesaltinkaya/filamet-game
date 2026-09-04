#pragma once
#include <cstddef>
#include "Defines.h"

/*
 * PropsRender
 * -----------
 * Backend-agnostic API of the Azgaar props (vegetation) render pass — the
 * GPU half of the game's AzgaarProps scatter (phase 7 of
 * plans/azgaar-terrain.md).
 *
 * Domain-agnostic contract (like the old engine's VulkanAzgaarPropsPass):
 * the pass only knows a merged species-mesh buffer, a per-tile instance
 * buffer (instances grouped by (species, variant)) and a per-frame wind
 * vector. All scatter / species / variant logic lives in the game
 * (c-game/game/azgaar/AzgaarProps) and is pushed through the Set* API
 * below.
 *
 * Data flow (main thread only):
 *   propsRenderSetMesh / SetVariants — once per world load (merged mesh +
 *       per-(species, variant) sub-ranges; replaces previous state)
 *   propsRenderSetTile               — once per (tile, readyStamp,
 *       buildSeq) when the game's scatter publishes a tile
 *   propsRenderUpdate                — once per frame from the backend's
 *       draw(): applies queued tiles (budgeted, nearest first), evicts GPU
 *       state for tiles that left the active HeightmapTerrain window,
 *       refreshes the wind time
 *   propsRenderClearAll / Destroy    — world release / backend teardown
 *
 * Dispatch: every entry point forwards to the active backend's half
 * (filament now; the diligent mirror is deferred with phase 6 of
 * plans/azgaar-terrain.md and is a no-op).
 */

namespace engine {

// One scatter instance (GPU-packed; the game's AzgaarPropInstance starts
// with exactly these 10 floats, in this order — the bridge converts).
//   pos    world position, Y on the physics-grid surface
//   yaw    rotation around Y (rad)
//   scale  uniform target height (metres; meshes are unit-height)
//   color  per-instance RGB tint (biome tint x patch noise x jitter)
//   phase  wind phase (rad, 0 for static species)
struct PropsRenderInstance {
    float pos[3];
    float yaw;
    float scale;
    float color[3];
    float phase;
};

// A contiguous run of one (species, variant) inside a tile's instance
// array: one instanced draw (chunked by the pass when the count exceeds
// the driver's instance limit), plus its world AABB (instance bounding
// spheres) used as the renderable's culling box.
struct PropsRenderRange {
    u32 species;
    u32 variant;
    u32 start;  // index into the tile's instance array
    u32 count;
    float aabbMin[3];
    float aabbMax[3];
};

// Per-draw look flags (PropsRenderMeshVariant.flags). A shared contract with
// the game (AzgaarProps.h AZGAAR_PROPS_FLAG_*) and the material (props.mat).
namespace props_render_flags {
constexpr u32 ALPHA_TEST   = 1u;  // bit 0: alpha-test the base texture
constexpr u32 DOUBLE_SIDED = 2u;  // bit 1: thin veg — no backface normal flip
constexpr u32 FLOWER       = 4u;  // bit 2: radial flower-disc alpha test
}  // namespace props_render_flags

// Merged-mesh sub-range for one (species, variant): all indices in
// [indexOffset, indexOffset + indexCount) point into the merged vertex
// array. boundsMin/Max are the unit-space AABB (metres at scale 1) used
// to normalise the wind-sway height weight; swayFactor is how much this
// species sways (0 = static, e.g. rocks). flags are per-draw look flags:
//   bit 0 = alpha-test the base texture (cutout grass cards)
//   bit 1 = thin double-sided vegetation: render both faces but light them
//           with the unflipped normal (no backface normal flip) — the built-in
//           lit model's doubleSided flip would darken the back of every blade
//   bit 2 = radial flower-disc alpha test (unit-UV quads)
// texturePath (pak-relative) is the base-colour texture for textured
// species (grass cards); null for procedural species (tinted vertex
// colour only).
struct PropsRenderMeshVariant {
    u32 species;
    u32 variant;
    u32 indexOffset;
    u32 indexCount;
    float boundsMin[3];
    float boundsMax[3];
    float swayFactor;
    u32 flags;
    const char* texturePath;
};

// Merged species mesh (uploaded once per world; replaces previous state).
// verts are interleaved 13-float vertices (52 B): float3 position @ 0,
// float4 normal (w = 0) @ 12, float2 uv @ 28, float4 part colour (w = 1)
// @ 36 — the Filament half repacks the normal slot into a tangent-frame
// quaternion and maps the array to POSITION/TANGENTS/UV0/CUSTOM0.
void propsRenderSetMesh(const float* verts, u32 vertCount, const u32* idx, u32 idxCount);

// Per-(species, variant) merged-mesh table (replaces previous state). The
// variantCount rows must cover every (species, variant) the scatter emits.
void propsRenderSetVariants(const PropsRenderMeshVariant* variants, u32 variantCount);

// Publish one scattered tile (copied; caller may free afterwards). Ranges
// must be contiguous runs of `instances` in (species, variant) order.
void propsRenderSetTile(i32 tileX, i32 tileZ, u64 readyStamp,
                        const PropsRenderInstance* instances, u32 instanceCount,
                        const PropsRenderRange* ranges, u32 rangeCount);

// Drop all per-tile GPU state (world release; the mesh/variant table and
// the enabled flag stay put and are re-set on the next world load).
void propsRenderClearAll(void);

// Global wind (vertex-stage sway): unit direction, angular speed (rad/s),
// 0..1 strength. Set at world load; the pass advances the phase with the
// frame clock each update.
void propsRenderSetWind(float dirX, float dirZ, float speed, float strength);

// Kill switch (draws nothing; also the default).
void propsRenderSetEnabled(bool enabled);

// Phase-7 acceptance instrumentation (mirrors the terrain pass'
// HeightmapTerrainRenderStats): rolling per-frame cost of the pass update
// and the analytic GPU footprint of the resident props window. Zeroed
// before the pass initializes or when no backend implements it.
//   renderAvgMs / applyAvgMs  updateImpl cost averaged over the last ~2 s;
//       applyAvgMs is the portion spent applying new tile uploads
//   gpuTiles / gpuDraws / gpuInstances  live GPU state (draws include
//       per-chunk splits over the 32767-instance renderable clamp)
//   gpuBytes          instance-data textures (live + deferred-destroy) +
//                     shared merged mesh + grass card textures
//   cpuStagingBytes   CPU staging kept alive for the no-copy uploads
struct PropsRenderStats {
    double renderAvgMs;
    double applyAvgMs;
    u32    frame;
    u32    gpuTiles;
    u32    gpuDraws;
    u32    gpuInstances;
    u32    pending;
    size_t gpuBytes;
    size_t cpuStagingBytes;
};
PropsRenderStats propsRenderStats(void);

// One-frame pass update. Called by the active render backend once per
// frame, before its scene render. No-op when disabled or no terrain.
void propsRenderUpdate(void);

// Destroy all props GPU state (backend teardown).
void propsRenderDestroy(void);
}  // namespace engine
