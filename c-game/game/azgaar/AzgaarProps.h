#pragma once

#include "azgaar/AzgaarWorld.h"

/*
 * AzgaarProps
 * -----------
 * CPU side of the Azgaar vegetation/props system (phase 7 of
 * plans/azgaar-terrain.md): a deterministic per-tile scatter that places
 * grass / trees / shrubs / flowers / rocks on the world terrain, ported
 * from the old engine's game-001-cpp AzgaarProps (scatter + overlap gate
 * + per-biome species mix; the old per-frame GPU cull/LOD cross-fade is
 * replaced by scatter-time frustum cull + camera-staleness re-scatter).
 *
 * Ownership: file-static per-world singleton, modelled on Game.cpp's
 * s_terrain. Lifecycle (wired in Game.cpp):
 *   azgaarPropsInit(world)   — at world load (after terrain init)
 *   azgaarPropsUpdate()      — per frame (GameSystem::update)
 *   azgaarPropsDestroy()     — before loadingAzgaarReleaseWorld
 *
 * Determinism contract: a tile's instances are a pure function of
 * (mapSeed, tileX, tileZ, build-time camera xz + frustum). Eviction +
 * regeneration is bit-identical. Instance Y comes ONLY from the 256^2
 * physics grid (heightmapGridBilinear over it), so props sit exactly on
 * the rendered/walkable surface.
 *
 * Rendering (phase 7, task: PropsRender pass) consumes the per-tile
 * grouped instance arrays through azgaarPropsGetTile; one draw per
 * (species, variant) range.
 */
namespace game {

// Vegetation species (the old engine's 0..12 ids, kept stable so the
// scatter's per-species tables match). The *_FAR ids exist for table
// parity but are never emitted by the scatter (no near/far LOD
// double-instances in this port).
enum AzgaarPropSpecies {
    AZGAAR_PROP_GRASS_TUFT = 0,
    AZGAAR_PROP_CONIFER,
    AZGAAR_PROP_CONIFER_FAR,
    AZGAAR_PROP_DECIDUOUS,
    AZGAAR_PROP_DECIDUOUS_FAR,
    AZGAAR_PROP_ACACIA,
    AZGAAR_PROP_PALM,
    AZGAAR_PROP_CACTUS,
    AZGAAR_PROP_DEAD_TREE,
    AZGAAR_PROP_REED,
    AZGAAR_PROP_SHRUB,
    AZGAAR_PROP_ROCK,
    AZGAAR_PROP_FLOWER,
    AZGAAR_PROP_COUNT,
};

// One scattered prop. `species`/`variant` are the (task 4) merged-mesh
// range identity — NOT per-instance GPU attributes; the render pass draws
// one InstancedDraw per contiguous (species, variant) range.
//   pos    world position, Y on the physics-grid surface
//   yaw    rotation around Y (rad)
//   scale  uniform target height (metres; meshes are unit-height)
//   color  per-instance RGB tint (biome tint x patch noise x jitter)
//   phase  wind phase (rad, 0 for static species)
struct AzgaarPropInstance {
    float pos[3];
    float yaw;
    float scale;
    float color[3];
    float phase;
    u32 species;
    u32 variant;
};

// A contiguous run of one (species, variant) inside a tile's instance
// array, plus its world AABB (instance bounding spheres) for per-frame
// range-level frustum culling by the render pass.
struct AzgaarPropRange {
    u32 species;
    u32 variant;
    u32 start;
    u32 count;
    float aabbMin[3];
    float aabbMax[3];
};

// One scattered tile (immutable while valid; replaced wholesale on
// re-scatter). Instances are grouped by (species, variant) in
// species-then-variant order.
struct AzgaarPropsTile {
    i32 tileX = 0, tileZ = 0;
    u64 readyStamp = 0;  // terrain tile stamp this scatter was built for
    // Monotonic publish counter (increments on every scatter publish, incl.
    // camera-stale re-scatters that keep the same readyStamp). The render
    // bridge keys its pushed state on (readyStamp, buildSeq) so a
    // re-scatter re-uploads exactly once.
    u32 buildSeq = 0;
    bool valid = false;
    std::vector<AzgaarPropInstance> instances;
    std::vector<AzgaarPropRange> ranges;  // frustum-culled at scatter time
    u32 perSpecies[AZGAAR_PROP_COUNT] = {};
};

// Grass texture variants (one crossed card per PNG; the scatter picks a
// variant deterministically per instance). Exposed for the mesh builder
// (task 4): variant index == position in this list.
struct AzgaarGrassVariantInfo {
    const char* path;
    float aspect;  // texture width/height (card half-width, unit height)
    float bottomV;  // V of the tuft base (1.0 = no trim of the empty band)
    bool loaded;
};

// Current wind for the props pass (vertex-stage sway). Static default
// when the map has no authored wind (the old engine's weather-driven
// gust is out of scope).
struct AzgaarPropsWind {
    float dirX, dirZ;  // unit wind direction
    float speed;       // sway angular speed (rad/s)
    float strength;    // 0..1 sway amount
};

// Main-thread access for the render pass (safe: tile entries are only
// mutated under the props lock, which the render pass never takes — it
// reads after update on the same thread).
const AzgaarPropsTile* azgaarPropsGetTile(i32 tileX, i32 tileZ, u64 readyStamp);
const AzgaarPropsWind* azgaarPropsWind(void);

// Phase-7 acceptance instrumentation: streaming counters + the resident CPU
// footprint of the tracked tile window. claims / rescatters / stampRebuilds /
// evictions are lifetime totals since azgaarPropsInit (a dolly run shows
// them as the follow/evict band); workerAvgMs is the per-tile scatter build
// time on the background worker (queueDepth > 0 for long means the camera
// outruns the worker).
struct AzgaarPropsStats {
    u32    resident;      // tiles tracked (terrain window)
    u32    built;         // resident tiles with a valid publish
    u32    instances;     // instances across resident tiles
    size_t cpuBytes;      // instance + range vectors of resident tiles
    u32    queueDepth;    // scatter jobs waiting for the worker
    u32    claims;        // first claim of a tile entering the window
    u32    rescatters;    // camera-stale rebuilds (AZGAAR_PROPS_RESCATTER_DIST)
    u32    stampRebuilds; // terrain readyStamp changes
    u32    evictions;     // tiles dropped (left the window)
    double workerAvgMs;   // per-tile scatter cost on the worker
    u32    workerBuilds;
};
AzgaarPropsStats azgaarPropsStats(void);

u32 azgaarPropsGrassVariantCount(void);
const AzgaarGrassVariantInfo* azgaarPropsGrassVariant(u32 i);

// Expected pre-clump vegetation density (instances/m^2) for a biome, from
// the same tables the scatter uses (icon weights x kSpeciesDensity x
// iconsDensity/120). treesOnly counts only tree species (conifer, deciduous,
// acacia, palm, dead tree). Used by the game to frame the props validation
// camera over the densest prop-bearing land; returns 0 for water/unknown
// biomes. Requires azgaarPropsInit(world) to have run for `world`.
float azgaarPropsBiomeDensity(const AzgaarWorld* world, u32 biomeId, bool treesOnly);

// Per-species wind sway (0..1) for the render pass' vertex-stage sway, from
// the same species table the scatter uses. Static for the pass (rocks are
// 0, so they never sway).
float azgaarPropsSpeciesSway(u32 species);

// Per-species look flags for the render pass' material (bitmask, see
// AZGAAR_PROPS_FLAG_*): which ranges alpha-test their base texture, which
// take the radial flower-disc test, and which are thin double-sided
// vegetation. Every other range is plain opaque.
#define AZGAAR_PROPS_FLAG_ALPHA_TEST   1u  // cutout: alpha-test the base texture
#define AZGAAR_PROPS_FLAG_DOUBLE_SIDED 2u  // thin blades: light both faces with the unflipped normal (no backface normal flip)
#define AZGAAR_PROPS_FLAG_FLOWER       4u  // unit-UV quad: radial disc alpha test
u32 azgaarPropsSpeciesRenderFlags(u32 species);

void azgaarPropsInit(const AzgaarWorld* world);
void azgaarPropsUpdate(void);
void azgaarPropsDestroy(void);
}  // namespace game
