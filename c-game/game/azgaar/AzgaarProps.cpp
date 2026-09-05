#include "azgaar/AzgaarPropMesh.h"
#include "azgaar/AzgaarProps.h"
#include "azgaar/AzgaarWorld.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "renderer/Renderer.h"
#include "renderer/RenderBackend.h"
#include "renderer/Window.h"
#include "Utils.h"

#include <cfloat>
#include <math.h>

// CPU scatter port of the old engine's AzgaarProps (game-001-cpp,
// c-game/game/azgaar/AzgaarProps.cpp): species tables + per-biome weighted
// mix (icons x repetition x iconsDensity/120), propsRand / propsClumpNoise /
// propsVegetationColor, per-texel loop over the 512^2 CPU grid with instance
// Y sampled from the 256^2 PHYSICS grid, PlacedHash overlap gate, 4-card grass
// undergrowth, rocks, one background worker, readyStamp-keyed tile state with
// camera-staleness re-scatter.
//
// Dropped relative to the old system (phase-7 scope, see
// .pi/ledger/plan.md): the road-clearing gate (no roads yet, phase 8), the
// landmark forest-boost discs (no landmarks module), the near/far LOD
// double-instances (no far-LOD cross-fade), the player-reaction push (no
// player, phase 9) and the per-frame per-instance GPU cull (replaced by
// scatter-time frustum cull + re-scatter on camera staleness).

namespace game {

// ── Constants ─────────────────────────────────────────────────────────────

#define AZGAAR_PROPS_MAX_BIOMES 32
#define AZGAAR_PROPS_TILE_CAP   2000000u  // hard per-tile instance cap (old engine, plan B)
// Re-scatter when the camera has moved more than this AND the tile is within
// this of the current or the build-time camera (the old engine's PROPS_LOD_DIST
// threshold; it now refreshes the scatter-time cull set, not the LOD).
#define AZGAAR_PROPS_RESCATTER_DIST 100.0f
// Conservative margin on the scatter-time frustum + per-species distance caps.
#define AZGAAR_PROPS_CULL_MARGIN 40.0f
// Angular sweep (rad) the scatter-time frustum test inflates by, to cover
// camera motion between re-scatters (old engine's PROPS_CULL_ROT_SWEEP idea).
#define AZGAAR_PROPS_SWEEP_RAD (3.0f * static_cast<float>(M_PI) / 180.0f)

// Grass card tint: the per-instance colour multiplies the grass texture, so
// keep it near unit magnitude (old engine's kGrassTintScale).
static const float kGrassTintScale = 1.0f;

// The grass card textures (one crossed-card variant each). Shipped in the
// pak; variant index == position among the files that actually load.
static const char* kGrassTexPaths[] = {
    "images/grass-textures/green-grass-1.png",
    "images/grass-textures/green-grass-2.png",
    "images/grass-textures/green-grass-3.png",
    "images/grass-textures/green-grass-4.png",
    "images/grass-textures/green-grass-with-plant.png",
    "images/grass-textures/dry-grass-1.png",
    "images/grass-textures/dry-grass-2.png",
};
static const u32 kGrassTexCount = sizeof(kGrassTexPaths) / sizeof(kGrassTexPaths[0]);

// ── Species tables (ported verbatim from the old engine) ──────────────────
// Placeholders are authored at UNIT height (base y=0, top y=1), so the
// scatter's uniform `scale` == target height in metres. `slopeMax` rejects a
// species on steeper ground; `sway` drives the wind animation (0 = static).
struct PropSpeciesDef {
    const char* key;
    float baseMin;   // target height metres (min)
    float baseMax;   // target height metres (max)
    float slopeMax;  // reject above this slope (dy/dx)
    float sway;      // 0..1 wind sway
};

static const PropSpeciesDef kSpecies[AZGAAR_PROP_COUNT] = {
    [AZGAAR_PROP_GRASS_TUFT]    = {"grass_tuft", 0.3f, 1.0f, 0.40f, 0.45f},
    [AZGAAR_PROP_CONIFER]       = {"conifer", 4.0f, 9.0f, 0.55f, 0.5f},
    [AZGAAR_PROP_CONIFER_FAR]   = {"conifer_far", 4.0f, 9.0f, 0.55f, 0.5f},
    [AZGAAR_PROP_DECIDUOUS]     = {"deciduous", 5.0f, 12.0f, 0.45f, 0.4f},
    [AZGAAR_PROP_DECIDUOUS_FAR] = {"deciduous_far", 5.0f, 12.0f, 0.45f, 0.4f},
    [AZGAAR_PROP_ACACIA]        = {"acacia", 6.0f, 10.0f, 0.35f, 0.35f},
    [AZGAAR_PROP_PALM]          = {"palm", 5.0f, 8.0f, 0.20f, 0.4f},
    [AZGAAR_PROP_CACTUS]        = {"cactus", 1.0f, 2.5f, 0.35f, 0.15f},
    [AZGAAR_PROP_DEAD_TREE]     = {"dead_tree", 3.0f, 7.0f, 0.5f, 0.5f},
    [AZGAAR_PROP_REED]          = {"reed", 0.8f, 1.5f, 0.15f, 1.0f},
    [AZGAAR_PROP_SHRUB]         = {"shrub", 0.4f, 1.0f, 0.6f, 0.2f},
    [AZGAAR_PROP_ROCK]          = {"rock", 0.5f, 3.0f, 1.0f, 0.0f},
    [AZGAAR_PROP_FLOWER]        = {"flower", 0.2f, 0.5f, 0.4f, 0.8f},
};

// Per-species within-patch base density (instances/m^2 before the biome's
// iconsDensity/120 scale). Grass is dense (crossed alpha-tested cards are
// cheap), forests moderate, deserts sparse (old engine's kSpeciesDensity).
static const float kSpeciesDensity[AZGAAR_PROP_COUNT] = {
    [AZGAAR_PROP_GRASS_TUFT]    = 0.40f,
    [AZGAAR_PROP_CONIFER]       = 0.008f,
    [AZGAAR_PROP_CONIFER_FAR]   = 0.008f,
    [AZGAAR_PROP_DECIDUOUS]     = 0.008f,
    [AZGAAR_PROP_DECIDUOUS_FAR] = 0.008f,
    [AZGAAR_PROP_ACACIA]        = 0.008f,
    [AZGAAR_PROP_PALM]          = 0.008f,
    [AZGAAR_PROP_CACTUS]        = 0.001f,
    [AZGAAR_PROP_DEAD_TREE]     = 0.0002f,
    [AZGAAR_PROP_REED]          = 0.015f,
    [AZGAAR_PROP_SHRUB]         = 0.004f,
    [AZGAAR_PROP_ROCK]          = 0.0f,
    [AZGAAR_PROP_FLOWER]        = 0.004f,
};

// Per-species minimum separation (m) for the overlap gate (old kMinDist).
static const float kMinDist[AZGAAR_PROP_COUNT] = {
    [AZGAAR_PROP_GRASS_TUFT]     = 0.5f,
    [AZGAAR_PROP_CONIFER]        = 5.0f,
    [AZGAAR_PROP_CONIFER_FAR]    = 5.0f,
    [AZGAAR_PROP_DECIDUOUS]      = 5.0f,
    [AZGAAR_PROP_DECIDUOUS_FAR]  = 5.0f,
    [AZGAAR_PROP_ACACIA]         = 5.0f,
    [AZGAAR_PROP_PALM]           = 5.0f,
    [AZGAAR_PROP_CACTUS]         = 2.0f,
    [AZGAAR_PROP_DEAD_TREE]      = 4.0f,
    [AZGAAR_PROP_REED]           = 1.5f,
    [AZGAAR_PROP_SHRUB]          = 1.5f,
    [AZGAAR_PROP_ROCK]           = 3.0f,
    [AZGAAR_PROP_FLOWER]         = 1.0f,
};

// Canopy radius as a fraction of the instance's height (scale), read from
// the placeholder mesh builders (old kCanopyFactor). The overlap gate uses
// it to reject two trees whose canopy discs would intersect.
static const float kCanopyFactor[AZGAAR_PROP_COUNT] = {
    [AZGAAR_PROP_GRASS_TUFT]     = 0.0f,
    [AZGAAR_PROP_CONIFER]        = 0.55f,
    [AZGAAR_PROP_CONIFER_FAR]    = 0.50f,
    [AZGAAR_PROP_DECIDUOUS]      = 0.80f,
    [AZGAAR_PROP_DECIDUOUS_FAR]  = 0.70f,
    [AZGAAR_PROP_ACACIA]         = 0.70f,
    [AZGAAR_PROP_PALM]           = 0.60f,
    [AZGAAR_PROP_CACTUS]         = 0.14f,
    [AZGAAR_PROP_DEAD_TREE]      = 0.12f,
    [AZGAAR_PROP_REED]           = 0.0f,
    [AZGAAR_PROP_SHRUB]          = 0.0f,
    [AZGAAR_PROP_ROCK]           = 0.0f,
    [AZGAAR_PROP_FLOWER]         = 0.0f,
};

// Per-species distance caps (metres) for the scatter-time cull (old
// kCullDist; trees 800, small ground species 400, rocks 500).
static const float kCullDist[AZGAAR_PROP_COUNT] = {
    [AZGAAR_PROP_GRASS_TUFT]     = 400.0f,
    [AZGAAR_PROP_CONIFER]        = 800.0f,
    [AZGAAR_PROP_CONIFER_FAR]    = 800.0f,
    [AZGAAR_PROP_DECIDUOUS]      = 800.0f,
    [AZGAAR_PROP_DECIDUOUS_FAR]  = 800.0f,
    [AZGAAR_PROP_ACACIA]         = 800.0f,
    [AZGAAR_PROP_PALM]           = 800.0f,
    [AZGAAR_PROP_CACTUS]         = 400.0f,
    [AZGAAR_PROP_DEAD_TREE]      = 800.0f,
    [AZGAAR_PROP_REED]           = 400.0f,
    [AZGAAR_PROP_SHRUB]          = 400.0f,
    [AZGAAR_PROP_ROCK]           = 500.0f,
    [AZGAAR_PROP_FLOWER]         = 400.0f,
};

// ── Deterministic RNG + world-anchored noise (old engine, verbatim) ────────

static u32 propsHash3(u32 a, u32 b, u32 c) {
    u32 h = a * 0x8da6b343u ^ b * 0xc2b2ae35u ^ c * 0x27d4eb2fu;
    h     = (h ^ (h >> 15)) * 0x2c1b3c6du;
    h     = (h ^ (h >> 12)) * 0x297a2d39u;
    return h ^ (h >> 15);
}

// Stable [0,1) random from a tile seed + texel index + salt. Pure function
// of (mapSeed, tileX, tileZ, texX, texZ, salt) -> eviction + regeneration
// is bit-identical.
static float propsRand(u32 tileSeed, u32 tx, u32 tz, u32 salt) {
    u32 h = propsHash3(tileSeed ^ (tx * 0x9E3779B9u) ^ (tz * 0x85EBCA77u), salt, 0x9E3779B9u);
    return static_cast<float>(h >> 8) / 16777216.0f;
}

static const u32 kFbmSeed = 0x9E3779B9u;

static float smoothstep01(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static float valueNoise10(float x, float z, u32 seed) {
    i32 xi   = static_cast<i32>(floorf(x));
    i32 zi   = static_cast<i32>(floorf(z));
    float xf = x - static_cast<float>(xi);
    float zf = z - static_cast<float>(zi);
    float u  = xf * xf * (3.0f - 2.0f * xf);
    float v  = zf * zf * (3.0f - 2.0f * zf);
    u32 kx = static_cast<u32>(xi), kz = static_cast<u32>(zi);
    float a = static_cast<float>((propsHash3(kx, kz, seed) >> 8) & 0xFFFFFF) / 16777216.0f;
    float b = static_cast<float>((propsHash3(kx + 1, kz, seed) >> 8) & 0xFFFFFF) / 16777216.0f;
    float c = static_cast<float>((propsHash3(kx, kz + 1, seed) >> 8) & 0xFFFFFF) / 16777216.0f;
    float d =
        static_cast<float>((propsHash3(kx + 1, kz + 1, seed) >> 8) & 0xFFFFFF) / 16777216.0f;
    float top    = a + (b - a) * u;
    float bottom = c + (d - c) * u;
    return top + (bottom - top) * v;  // [0,1]
}

// Two-octave value noise in WORLD space for the vegetation clumping gate
// (fixed seed so the patch pattern is map-stable). Output in ~[0,0.9].
static float propsClumpNoise(float wx, float wz) {
    float n = 0.6f * valueNoise10(wx * 0.1f, wz * 0.1f, kFbmSeed);
    n += 0.3f * valueNoise10(wx * 0.05f, wz * 0.05f, kFbmSeed + 1013904223u);
    return n;
}

// Vegetation colour variation (old engine, verbatim): biome tint x coarse
// world-anchored patch noise x per-instance brightness jitter x per-channel
// micro-shifts (dry/yellowed, fresh/blue-green drifts). `jitter` scales the
// per-instance deviation (grass passes 1.0). Pure function of world
// position + tile seed, so re-scatter stays bit-identical.
// `saltBase` must leave the next three salt values free (+1..+3).
static void propsVegetationColor(const AzgaarWorld* world,
                                 u32 biome,
                                 float wx,
                                 float wz,
                                 u32 tileSeed,
                                 u32 tx,
                                 u32 tz,
                                 u32 saltBase,
                                 float out[3],
                                 float jitter = 1.0f) {
    float bc[3];
    azgaarWorldBiomeColor(world, biome, bc);
    float n = 0.6f * valueNoise10(wx * 0.02f, wz * 0.02f, 0x51A7435Fu) +
              0.4f * valueNoise10(wx * 0.008f, wz * 0.008f, 0x51A7435Fu + 1013904223u);
    float patch = 0.78f + 0.44f * n;  // ~+/-22% field-scale clumps
    float r0    = propsRand(tileSeed, tx, tz, saltBase);
    float r1    = propsRand(tileSeed, tx, tz, saltBase + 1u);
    float r2    = propsRand(tileSeed, tx, tz, saltBase + 2u);
    float r3    = propsRand(tileSeed, tx, tz, saltBase + 3u);
    float j     = (1.0f + (2.0f * r0 - 1.0f) * 0.25f * jitter) * patch;
    float jr    = 1.0f + (2.0f * r1 - 1.0f) * 0.18f * jitter;
    float jg    = 1.0f + (2.0f * r2 - 1.0f) * 0.10f * jitter;
    float jb    = 1.0f + (2.0f * r3 - 1.0f) * 0.25f * jitter;
    out[0]      = bc[0] * j * jr;
    out[1]      = bc[1] * j * jg;
    out[2]      = bc[2] * j * jb;
}

// ── Placed-instance hash (overlap gate; job-local, per tile) ───────────────

struct PlacedHash {
    u32 bucket;
    float invBucket;
    float minX, minZ;  // tile origin (world)
    u32 gridW, gridH;
    u32 bucketCount;
    std::vector<u32> head;    // bucket -> first point index (0xFFFFFFFF = empty)
    std::vector<float> pts;   // [x, z] pairs
    std::vector<u32> sp;      // species id per point
    std::vector<float> scale; // instance height (m) per point (canopy radius)
    std::vector<i32> next;    // next point index within the same bucket
    u32 count;
};

static void placedHashBuild(PlacedHash* ph, float minX, float minZ, float sizeM) {
    *ph                = PlacedHash{};
    const float bucket = 8.0f;
    ph->bucket         = static_cast<u32>(bucket);
    ph->invBucket      = 1.0f / bucket;
    ph->minX           = minX;
    ph->minZ           = minZ;
    ph->gridW          = static_cast<u32>(sizeM / bucket) + 1u;
    ph->gridH          = static_cast<u32>(sizeM / bucket) + 1u;
    ph->bucketCount    = ph->gridW * ph->gridH;
    ph->head.assign(ph->bucketCount, 0xFFFFFFFF);
    const u32 cap = AZGAAR_PROPS_TILE_CAP;
    ph->pts.resize(2 * cap);
    ph->sp.resize(cap);
    ph->scale.resize(cap);
    ph->next.resize(cap);
    ph->count = 0;
}

static u32 placedBucket(PlacedHash* ph, float x, float z) {
    i32 bx = static_cast<i32>((x - ph->minX) * ph->invBucket);
    i32 bz = static_cast<i32>((z - ph->minZ) * ph->invBucket);
    if (bx < 0) bx = 0;
    if (bz < 0) bz = 0;
    if (bx >= static_cast<i32>(ph->gridW)) bx = static_cast<i32>(ph->gridW) - 1;
    if (bz >= static_cast<i32>(ph->gridH)) bz = static_cast<i32>(ph->gridH) - 1;
    return static_cast<u32>(bz) * ph->gridW + static_cast<u32>(bx);
}

static void placedHashInsert(PlacedHash* ph, float x, float z, u32 species, float scale) {
    u32 i             = ph->count;
    ph->pts[i * 2]    = x;
    ph->pts[i * 2 + 1] = z;
    ph->sp[i]         = species;
    ph->scale[i]      = scale;
    u32 b             = placedBucket(ph, x, z);
    ph->next[i]       = static_cast<i32>(ph->head[b]);
    ph->head[b]       = i;
    ph->count++;
}

// True if (x, z) is too close to a placed instance. Required separation is
// the max of the fixed per-species floor (kMinDist) and the sum of the two
// canopy radii (kCanopyFactor * scale). Ground-level candidates (canopy
// factor 0) respect only their own min distance, so a placed tree's large
// floor does not block grass at its base.
static bool placedTooClose(PlacedHash* ph, float x, float z, u32 candSpecies, float candScale) {
    if (ph->count == 0) return false;
    i32 bx = static_cast<i32>((x - ph->minX) * ph->invBucket);
    i32 bz = static_cast<i32>((z - ph->minZ) * ph->invBucket);
    for (i32 oz = -1; oz <= 1; oz++) {
        for (i32 ox = -1; ox <= 1; ox++) {
            i32 nx = bx + ox, nz = bz + oz;
            if (nx < 0 || nz < 0 || nx >= static_cast<i32>(ph->gridW) ||
                nz >= static_cast<i32>(ph->gridH))
                continue;
            u32 b = static_cast<u32>(nz) * ph->gridW + static_cast<u32>(nx);
            for (i32 i = static_cast<i32>(ph->head[b]); i >= 0; i = ph->next[i]) {
                float dx = ph->pts[i * 2] - x;
                float dz = ph->pts[i * 2 + 1] - z;
                float sep = kMinDist[candSpecies];
                if (kCanopyFactor[candSpecies] > 0.0f) {
                    if (kMinDist[ph->sp[i]] > sep) sep = kMinDist[ph->sp[i]];
                    float need = kCanopyFactor[candSpecies] * candScale +
                                 kCanopyFactor[ph->sp[i]] * ph->scale[i];
                    if (need > sep) sep = need;
                }
                if (sep > 0.0f && dx * dx + dz * dz < sep * sep) return true;
            }
        }
    }
    return false;
}

// ── Per-biome species mix (icons x repetition x iconsDensity/120) ─────────

struct BiomeSpecies {
    u32 species[AZGAAR_PROP_COUNT];
    u32 weight[AZGAAR_PROP_COUNT];
    u32 count;
    u32 totalWeight;
};

static u32 iconToSpecies(const char* name) {
    if (!name || !name[0]) return (u32)-1;
    if (strcmp(name, "grass") == 0) return AZGAAR_PROP_GRASS_TUFT;
    if (strcmp(name, "conifer") == 0) return AZGAAR_PROP_CONIFER;
    if (strcmp(name, "deciduous") == 0) return AZGAAR_PROP_DECIDUOUS;
    if (strcmp(name, "acacia") == 0) return AZGAAR_PROP_ACACIA;
    if (strcmp(name, "palm") == 0) return AZGAAR_PROP_PALM;
    if (strcmp(name, "cactus") == 0) return AZGAAR_PROP_CACTUS;
    if (strcmp(name, "deadTree") == 0) return AZGAAR_PROP_DEAD_TREE;
    if (strcmp(name, "swamp") == 0) return AZGAAR_PROP_REED;
    if (strcmp(name, "shrub") == 0) return AZGAAR_PROP_SHRUB;
    if (strcmp(name, "flower") == 0) return AZGAAR_PROP_FLOWER;
    if (strcmp(name, "rock") == 0) return AZGAAR_PROP_ROCK;
    return (u32)-1;  // dune and others: no placeholder mesh yet
}

static bool propsIsTreeSpecies(u32 sp) {
    switch (sp) {
        case AZGAAR_PROP_CONIFER:
        case AZGAAR_PROP_DECIDUOUS:
        case AZGAAR_PROP_ACACIA:
        case AZGAAR_PROP_PALM:
        case AZGAAR_PROP_DEAD_TREE:
            return true;
        default:
            return false;
    }
}

// Precompute the per-biome species mix (weighted by icon repetition) from
// the world's biome table. Grassy biomes get a small flower sprinkle
// (10% of the grass weight) added (old engine, verbatim).
static void precomputeBiomeSpecies(const AzgaarWorld* world,
                                   BiomeSpecies* out,
                                   u32 count) {
    for (u32 b = 0; b < count && b < world->biomeCount; b++) {
        out[b]                   = BiomeSpecies{};
        const AzgaarBiome* biome = &world->biomes[b];
        if (biome->iconCount == 0) continue;
        for (u32 k = 0; k < biome->iconCount; k++) {
            u32 sp = iconToSpecies(biome->icons[k]);
            if (sp == (u32)-1 || sp >= AZGAAR_PROP_COUNT) continue;
            u32 slot = (u32)-1;
            for (u32 s = 0; s < out[b].count; s++) {
                if (out[b].species[s] == sp) {
                    slot = s;
                    break;
                }
            }
            if (slot == (u32)-1) {
                slot                 = out[b].count++;
                out[b].species[slot] = sp;
            }
            out[b].weight[slot]++;
            out[b].totalWeight++;
        }
        u32 grassW = 0;
        for (u32 s = 0; s < out[b].count; s++) {
            if (out[b].species[s] == AZGAAR_PROP_GRASS_TUFT) grassW = out[b].weight[s];
        }
        if (grassW > 0) {
            u32 fslot = (u32)-1;
            for (u32 s = 0; s < out[b].count; s++) {
                if (out[b].species[s] == AZGAAR_PROP_FLOWER) {
                    fslot = s;
                    break;
                }
            }
            if (fslot == (u32)-1) {
                fslot                 = out[b].count++;
                out[b].species[fslot] = AZGAAR_PROP_FLOWER;
            }
            out[b].weight[fslot] += static_cast<u32>(grassW * 0.1f + 0.5f);
            out[b].totalWeight += static_cast<u32>(grassW * 0.1f + 0.5f);
        }
    }
}

// ── Scatter-time frustum cull ─────────────────────────────────────────────
// The new renderer only exposes the camera eye + forward, so the build-time
// frustum is reconstructed here from the known projection (kCameraFovYDeg
// vertical fov, aspect from the window). Planes are inside-positive:
// dot(n, p) + w >= 0 is inside.

static void propsBuildFrustumPlanes(const float camPos[3],
                                    const float fwd[3],
                                    float aspect,
                                    float planes[6][4]) {
    const float fx = fwd[0], fy = fwd[1], fz = fwd[2];
    // World up +Y; guard the degenerate near-vertical gaze.
    float ux = 0.0f, uy = 1.0f, uz = 0.0f;
    if (fy > 0.999f) { ux = 0.0f; uy = 0.0f; uz = -1.0f; }
    else if (fy < -0.999f) { ux = 0.0f; uy = 0.0f; uz = 1.0f; }
    // right = normalize(fwd x up); upv = normalize(right x fwd).
    float rx = fy * uz - fz * uy, ry = fz * ux - fx * uz, rz = fx * uy - fy * ux;
    float rl = sqrtf(rx * rx + ry * ry + rz * rz);
    if (rl < 1e-6f) { rx = 1.0f; ry = 0.0f; rz = 0.0f; } else { rx /= rl; ry /= rl; rz /= rl; }
    float vx = ry * fz - rz * fy, vy = rz * fx - rx * fz, vz = rx * fy - ry * fx;
    float vl = sqrtf(vx * vx + vy * vy + vz * vz);
    if (vl < 1e-6f) { vx = 0.0f; vy = 1.0f; vz = 0.0f; } else { vx /= vl; vy /= vl; vz /= vl; }

    const float halfH =
        tanf(0.5f * engine::renderer::kCameraFovYDeg * static_cast<float>(M_PI) / 180.0f);
    const float halfW = halfH * aspect;

    struct Dir {
        float x, y, z;
    };
    auto dirOf = [&](float wx, float wy) {
        // Direction in the camera basis: w * right + v * upv + fwd, normalised.
        Dir d = {fx + wx * rx + wy * vx, fy + wx * ry + wy * vy, fz + wx * rz + wy * vz};
        float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        d.x /= l; d.y /= l; d.z /= l;
        return d;
    };
    auto cross = [](float ax, float ay, float az, float bx, float by, float bz) {
        Dir d = {ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx};
        float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        if (l > 1e-9f) { d.x /= l; d.y /= l; d.z /= l; }
        return d;
    };
    auto setPlane = [&](int i, const Dir& n) {
        planes[i][0] = n.x; planes[i][1] = n.y; planes[i][2] = n.z;
        planes[i][3] = -(n.x * camPos[0] + n.y * camPos[1] + n.z * camPos[2]);
    };

    Dir dL = dirOf(-halfW, 0.0f);
    Dir dR = dirOf(halfW, 0.0f);
    Dir dD = dirOf(0.0f, -halfH);
    Dir dU = dirOf(0.0f, halfH);

    setPlane(0, cross(dL.x, dL.y, dL.z, vx, vy, vz));       // left  (inward +right)
    setPlane(1, cross(vx, vy, vz, dR.x, dR.y, dR.z));       // right (inward -right)
    setPlane(2, cross(rx, ry, rz, dD.x, dD.y, dD.z));       // bottom (inward +up)
    setPlane(3, cross(dU.x, dU.y, dU.z, rx, ry, rz));       // top    (inward -up)
    setPlane(4, Dir{fx, fy, fz});                           // near
    planes[5][0] = fx; planes[5][1] = fy; planes[5][2] = fz;
    planes[5][3] = -(fx * camPos[0] + fy * camPos[1] + fz * camPos[2]);  // far (same plane, generous)
}

static bool propsSphereOutsideFrustum(float cx, float cy, float cz, float r,
                                      const float planes[6][4]) {
    for (int i = 0; i < 6; i++) {
        if (planes[i][0] * cx + planes[i][1] * cy + planes[i][2] * cz + planes[i][3] < -r)
            return true;
    }
    return false;
}

// ── Pure per-tile scatter ─────────────────────────────────────────────────
// Fills `outInstances` (grouped by (species, variant) in species-then-variant
// order) and `outRanges` (the frustum-culled runs + their world AABBs).
// Pure function of the inputs: two calls with the same inputs are
// bit-identical (the one-shot acceptance re-scatter check relies on this).

struct PropsScatterInput {
    const AzgaarWorld* world;
    u32 mapSeed;
    const BiomeSpecies* biomeSpecies;
    u32 grassVariantCount;
    i32 tileX, tileZ;
    float originX, originZ;
    float tileSize;
    const float* heights;      // [HEIGHTMAP_TEX]^2, metres
    const float* physHeights;  // [HEIGHTMAP_PHYSICS_PSN]^2, metres
    float camX, camZ;          // build-time camera xz
    float camPos[3];           // build-time camera position
    float planes[6][4];        // build-time frustum (used when havePlanes)
    bool havePlanes;
};

static void propsScatterTile(const PropsScatterInput& in,
                             std::vector<AzgaarPropInstance>& outInstances,
                             std::vector<AzgaarPropRange>& outRanges,
                             u32 outPerSpecies[AZGAAR_PROP_COUNT]) {
    outInstances.clear();
    outRanges.clear();
    for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) outPerSpecies[s] = 0;

    const AzgaarWorld* world = in.world;
    const u32 TEX        = HEIGHTMAP_TEX;
    const u32 PSN        = HEIGHTMAP_PHYSICS_PSN;
    const float tsz      = in.tileSize / static_cast<float>(TEX - 1u);  // texel world size (m)
    const float psnScale = static_cast<float>(PSN - 1u) / in.tileSize;  // world m -> 256^2 units
    const float originX  = in.originX;
    const float originZ  = in.originZ;
    const u32 tileSeed   = in.mapSeed ^ static_cast<u32>(in.tileX * 374761393u) ^
                           static_cast<u32>(in.tileZ * 668265263u);
    const float seaY     = azgaarSeaLevelMeters(world);
    const float maxLand  = world->maxLandHeightM;
    const float halfW    = static_cast<float>(world->widthPx * 0.5f) *
                           static_cast<float>(world->metersPerPixel);
    const float halfH    = static_cast<float>(world->heightPx * 0.5f) *
                           static_cast<float>(world->metersPerPixel);
    const float invMppPx = 1.0f / static_cast<float>(world->metersPerPixel);
    const u32 grassVc    = in.grassVariantCount;

    PlacedHash ph;
    placedHashBuild(&ph, originX, originZ, in.tileSize);

    const u8* biomeGrid = world->biomeGrid.data();
    const u32 bw        = world->climateGridWidth;
    const u32 bh        = world->climateGridHeight;

    std::vector<AzgaarPropInstance> temp;
    temp.reserve(65536);

    for (u32 tz = 0; tz < TEX; tz++) {
        const float* row  = in.heights + static_cast<size_t>(tz) * TEX;
        const u32 tzN     = (tz + 1u < TEX) ? tz + 1u : TEX - 1u;
        const u32 tzS     = (tz > 0u) ? tz - 1u : 0u;
        const float* rowN = in.heights + static_cast<size_t>(tzN) * TEX;
        const float* rowS = in.heights + static_cast<size_t>(tzS) * TEX;
        for (u32 tx = 0; tx < TEX; tx++) {
            float wx = originX + static_cast<float>(tx) * tsz;
            float wz = originZ + static_cast<float>(tz) * tsz;

            // Ground height on the 256^2 render/physics surface (bilinear at
            // this texel's world position). All instance Ys use this, never the
            // 512^2 texel value, so props sit exactly on the rendered surface.
            float hGround = engine::heightmapGridBilinear(in.physHeights, PSN,
                                                          (wx - originX) * psnScale,
                                                          (wz - originZ) * psnScale);
            if (hGround < seaY + 0.5f) continue;  // water / below the grass line

            // Slope (finite difference over the 512 grid, clamped at borders).
            const u32 txL = (tx > 0u) ? tx - 1u : 0u;
            const u32 txR = (tx + 1u < TEX) ? tx + 1u : TEX - 1u;
            float dhdx    = (row[txR] - row[txL]) * (0.5f / tsz);
            float dhdz    = (rowN[tx] - rowS[tx]) * (0.5f / tsz);
            float slope   = sqrtf(dhdx * dhdx + dhdz * dhdz);

            // Nearest-cell biome (inverse of azgaarMapToWorld).
            u32 biome = AZGAAR_BIOME_NONE;
            if (biomeGrid && bw > 1 && bh > 1) {
                float mapX = (halfW - wx) * invMppPx;
                float mapY = (halfH - wz) * invMppPx;
                u32 gx = static_cast<u32>(mapX *
                                          (static_cast<float>(bw) / static_cast<float>(world->widthPx)) +
                                          0.5f);
                u32 gy = static_cast<u32>(mapY *
                                          (static_cast<float>(bh) / static_cast<float>(world->heightPx)) +
                                          0.5f);
                if (gx >= bw) gx = bw - 1u;
                if (gy >= bh) gy = bh - 1u;
                biome = static_cast<u32>(biomeGrid[static_cast<size_t>(gy) * bw + gx]);
            }

            // Distance from the build-time camera (near-player guarantee).
            float dxc  = wx - in.camX;
            float dzc  = wz - in.camZ;
            float dist = sqrtf(dxc * dxc + dzc * dzc);

            // Clumping gate (world-anchored fBm) + near-player guarantee.
            float clump     = propsClumpNoise(wx, wz);
            float clumpKeep = clump > 0.55f ? 1.0f : 0.0f;
            float near      = 1.0f - smoothstep01(250.0f, 600.0f, dist);
            float keep      = clumpKeep > near ? clumpKeep : near;

            // ── Grass undergrowth (before the main pass so its continue can't skip it) ──
            // Independent density roll, 2 m offset to avoid the tree at the texel
            // centre. No overlap gate: grass-vs-grass overlap is visually
            // harmless.
            if (biome < world->biomeCount && biome < AZGAAR_PROPS_MAX_BIOMES &&
                in.biomeSpecies[biome].count > 0 && grassVc > 0 &&
                slope <= kSpecies[AZGAAR_PROP_GRASS_TUFT].slopeMax) {
                // Lush undergrowth: every eligible texel may spawn a 4-card
                // cluster (0.25/m^2 uniform, no clump gate) so grassland reads
                // as a meadow rather than scattered tufts.
                float underD = 0.12f * (tsz * tsz);
                if (propsRand(tileSeed, tx, tz, 0xD1) < underD) {
                    const PropSpeciesDef* gdef = &kSpecies[AZGAAR_PROP_GRASS_TUFT];
                    float baseAng =
                        propsRand(tileSeed, tx, tz, 0xD6) * 2.0f * static_cast<float>(M_PI);
                    for (u32 gi = 0; gi < 4; gi++) {
                        float ang = baseAng + static_cast<float>(gi) * 0.5f *
                                                     static_cast<float>(M_PI);
                        float gwx = wx + cosf(ang) * 2.0f;
                        float gwz = wz + sinf(ang) * 2.0f;
                        // Ground under THIS card (its own xz, not the cluster
                        // centre): a 2 m arm on a slope changes the ground height
                        // by up to slope * 2 m.
                        float gGround = engine::heightmapGridBilinear(
                            in.physHeights, PSN,
                            (gwx - originX) * psnScale, (gwz - originZ) * psnScale);
                        float gScale =
                            gdef->baseMin + (gdef->baseMax - gdef->baseMin) *
                                            propsRand(tileSeed, tx, tz, 0xD2 + gi);
                        AzgaarPropInstance inst = {};
                        inst.pos[0]             = gwx;
                        inst.pos[1]             = gGround;
                        inst.pos[2]             = gwz;
                        inst.yaw   = propsRand(tileSeed, tx, tz, 0xD4 + gi) * 2.0f *
                                     static_cast<float>(M_PI);
                        inst.scale = gScale;
                        propsVegetationColor(world, biome, gwx, gwz, tileSeed, tx, tz,
                                             0xD7u + gi * 4u, inst.color, 1.0f);
                        inst.color[0] *= kGrassTintScale;
                        inst.color[1] *= kGrassTintScale;
                        inst.color[2] *= kGrassTintScale;
                        inst.phase   = propsRand(tileSeed, tx, tz, 0xD5 + gi) * 2.0f *
                                       static_cast<float>(M_PI);
                        inst.species = AZGAAR_PROP_GRASS_TUFT;
                        // Deterministic per-instance variant pick (one card per
                        // grass texture).
                        inst.variant =
                            (grassVc > 1)
                                ? static_cast<u32>(propsRand(tileSeed, tx, tz, 0xD8 + gi) *
                                                    static_cast<float>(grassVc))
                                : 0;
                        if (temp.size() < AZGAAR_PROPS_TILE_CAP) {
                            temp.push_back(inst);
                            outPerSpecies[AZGAAR_PROP_GRASS_TUFT]++;
                        }
                    }
                }
            }

            // ── Vegetation ──
            if (keep > 0.0f && biome < world->biomeCount && biome < AZGAAR_PROPS_MAX_BIOMES &&
                in.biomeSpecies[biome].count > 0) {
                const BiomeSpecies* bs = &in.biomeSpecies[biome];
                // Weighted species pick (icon repetition = weight).
                u32 roll = static_cast<u32>(propsRand(tileSeed, tx, tz, 0xAB) *
                                            static_cast<float>(bs->totalWeight));
                u32 acc = 0, chosen = bs->species[0];
                for (u32 s = 0; s < bs->count; s++) {
                    acc += bs->weight[s];
                    if (roll < acc) {
                        chosen = bs->species[s];
                        break;
                    }
                    chosen = bs->species[s];
                }
                u32 sp = chosen;
                if (sp >= AZGAAR_PROP_COUNT) continue;
                const PropSpeciesDef* def = &kSpecies[sp];
                if (slope > def->slopeMax) continue;  // too steep for this species
                float density =
                    kSpeciesDensity[sp] *
                    (static_cast<float>(world->biomes[biome].iconsDensity) / 120.0f);
                if (density <= 0.0f) continue;
                float expected = density * keep * (tsz * tsz);
                if (expected <= 0.0f) continue;
                if (propsRand(tileSeed, tx, tz, 0xCD) >= expected) continue;
                float instScale = def->baseMin + (def->baseMax - def->baseMin) *
                                                         propsRand(tileSeed, tx, tz, 0xE2);
                if (placedTooClose(&ph, wx, wz, sp, instScale)) continue;  // overlap gate

                AzgaarPropInstance inst = {};
                inst.pos[0]             = wx;
                inst.pos[1]             = hGround;
                inst.pos[2]             = wz;
                inst.yaw   = propsRand(tileSeed, tx, tz, 0xE1) * 2.0f *
                             static_cast<float>(M_PI);
                inst.scale = instScale;
                if (sp == AZGAAR_PROP_GRASS_TUFT) {
                    propsVegetationColor(world, biome, wx, wz, tileSeed, tx, tz, 0xE6u,
                                         inst.color, 1.0f);
                    inst.color[0] *= kGrassTintScale;
                    inst.color[1] *= kGrassTintScale;
                    inst.color[2] *= kGrassTintScale;
                } else {
                    propsVegetationColor(world, biome, wx, wz, tileSeed, tx, tz, 0xE6u,
                                         inst.color);
                }
                inst.phase   = propsRand(tileSeed, tx, tz, 0xE4) * 2.0f *
                               static_cast<float>(M_PI);
                inst.species = sp;
                inst.variant = 0;  // vegetation: one mesh variant each (grass picks above)
                if (temp.size() < AZGAAR_PROPS_TILE_CAP) {
                    temp.push_back(inst);
                    outPerSpecies[sp]++;
                    placedHashInsert(&ph, wx, wz, sp, instScale);
                }
            }

            // ── Rocks (non-biome, on steep / high ground) ──
            if ((slope > 0.35f || hGround > 0.5f * maxLand)) {
                float rockD = 0.001f * (tsz * tsz);
                if (propsRand(tileSeed, tx, tz, 0xF1) < rockD) {
                    float rockScale = 0.5f + 2.5f * propsRand(tileSeed, tx, tz, 0xF3);
                    if (placedTooClose(&ph, wx, wz, AZGAAR_PROP_ROCK, rockScale))
                        continue;  // overlap gate
                    AzgaarPropInstance inst = {};
                    inst.pos[0]             = wx;
                    inst.pos[1]             = hGround;
                    inst.pos[2]             = wz;
                    inst.yaw = propsRand(tileSeed, tx, tz, 0xF2) * 2.0f * static_cast<float>(M_PI);
                    inst.scale    = rockScale;
                    inst.color[0] = 0.45f;
                    inst.color[1] = 0.43f;
                    inst.color[2] = 0.40f;
                    inst.phase    = 0.0f;
                    inst.species  = AZGAAR_PROP_ROCK;
                    inst.variant  = 0;
                    if (temp.size() < AZGAAR_PROPS_TILE_CAP) {
                        temp.push_back(inst);
                        outPerSpecies[AZGAAR_PROP_ROCK]++;
                        placedHashInsert(&ph, wx, wz, AZGAAR_PROP_ROCK, rockScale);
                    }
                }
            }
        }
    }

    // ── Group the unsorted instances by (species, variant) ──
    u32 n = static_cast<u32>(temp.size());
    u32 totalV = 0;
    u32 base[AZGAAR_PROP_COUNT];
    {
        u32 a = 0;
        for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) {
            base[s] = a;
            a += (s == AZGAAR_PROP_GRASS_TUFT) ? grassVc : 1;
            totalV += (s == AZGAAR_PROP_GRASS_TUFT) ? grassVc : 1;
        }
    }
    std::vector<u32> counts(totalV, 0);
    for (u32 i = 0; i < n; i++) {
        u32 sp = temp[i].species;
        if (sp < AZGAAR_PROP_COUNT) counts[base[sp] + temp[i].variant]++;
    }

    struct SVPair {
        u32 species;
        u32 variant;
        u32 count;
        u32 offset;
        u32 start;
        float aabbMin[3];
        float aabbMax[3];
    };
    std::vector<SVPair> pairs(totalV);
    u32 pairCount = 0;
    u32 acc       = 0;
    for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) {
        u32 vc = (s == AZGAAR_PROP_GRASS_TUFT) ? grassVc : 1;
        for (u32 v = 0; v < vc; v++) {
            u32 c = counts[base[s] + v];
            if (c > 0) {
                pairs[pairCount].species = s;
                pairs[pairCount].variant = v;
                pairs[pairCount].count   = c;
                pairs[pairCount].offset  = acc;
                acc += c;
                pairCount++;
            }
        }
    }

    std::vector<AzgaarPropInstance> grouped(n);
    if (n > 0) {
        std::vector<u32> cursor(totalV, 0);
        std::vector<u32> flatOffset(totalV, 0);
        for (u32 p = 0; p < pairCount; p++) {
            flatOffset[base[pairs[p].species] + pairs[p].variant] = pairs[p].offset;
        }
        for (u32 i = 0; i < n; i++) {
            u32 sp   = temp[i].species;
            u32 flat = base[sp] + temp[i].variant;
            u32 dst  = flatOffset[flat] + cursor[flat]++;
            grouped[dst] = temp[i];
        }
    }

    // ── Scatter-time cull: per-range distance cap + frustum sweep margin ──
    // Compacts each range in place (survivors keep their relative order) and
    // records (start, count) + the survivors' world AABB from instance
    // bounding spheres (centre at half height, radius 0.75 * scale — close
    // enough to the unit-height mesh extents for culling).
    const float rotSweep = sinf(AZGAAR_PROPS_SWEEP_RAD);  // ~0.052
    for (u32 p = 0; p < pairCount; p++) {
        u32 sp  = pairs[p].species;
        u32 c0  = pairs[p].offset;
        u32 c1  = c0 + pairs[p].count;
        float cap   = kCullDist[sp] + AZGAAR_PROPS_CULL_MARGIN;
        float capSq = cap * cap;
        float aabbMin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float aabbMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        u32 w = c0, c = 0;
        for (u32 i = c0; i < c1; i++) {
            const AzgaarPropInstance& inst = grouped[i];
            float dx = inst.pos[0] - in.camPos[0];
            float dy = inst.pos[1] - in.camPos[1];
            float dz = inst.pos[2] - in.camPos[2];
            // Ground-plane (XZ) distance: props hug the terrain, and the
            // validation cameras fly hundreds of metres up, so a 3D cap would
            // cull whole visible tiles from above (the old engine's 3D cap
            // only worked from its eye-level player camera).
            if (dx * dx + dz * dz > capSq) continue;
            float sx = inst.pos[0];
            float sy = inst.pos[1] + 0.5f * inst.scale;
            float sz = inst.pos[2];
            float sr = 0.75f * inst.scale;
            if (in.havePlanes) {
                // Distance-scaled sweep margin: covers the camera motion the
                // re-scatter threshold tolerates between cull builds.
                float cd  = sqrtf(dx * dx + dy * dy + dz * dz);
                float rad = sr + cd * rotSweep + AZGAAR_PROPS_RESCATTER_DIST;
                if (propsSphereOutsideFrustum(sx, sy, sz, rad, in.planes)) continue;
            }
            if (sx - sr < aabbMin[0]) aabbMin[0] = sx - sr;
            if (sy - sr < aabbMin[1]) aabbMin[1] = sy - sr;
            if (sz - sr < aabbMin[2]) aabbMin[2] = sz - sr;
            if (sx + sr > aabbMax[0]) aabbMax[0] = sx + sr;
            if (sy + sr > aabbMax[1]) aabbMax[1] = sy + sr;
            if (sz + sr > aabbMax[2]) aabbMax[2] = sz + sr;
            grouped[w++] = inst;
            c++;
        }
        pairs[p].start = c0;
        pairs[p].count = c;
        memcpy(pairs[p].aabbMin, aabbMin, sizeof(aabbMin));
        memcpy(pairs[p].aabbMax, aabbMax, sizeof(aabbMax));
    }

    // The per-range compaction above leaves gaps; pull the kept ranges
    // contiguously (deterministic: same input order) and drop empty ranges.
    u32 w = 0;
    std::vector<SVPair> kept;
    for (u32 p = 0; p < pairCount; p++) {
        if (pairs[p].count == 0) continue;
        u32 c0 = pairs[p].start;
        u32 c1 = c0 + pairs[p].count;
        for (u32 i = c0; i < c1; i++) grouped[w++] = grouped[i];
        pairs[p].start = w - pairs[p].count;
        kept.push_back(pairs[p]);
    }
    grouped.resize(w);

    outInstances = std::move(grouped);
    outRanges.resize(kept.size());
    for (u32 p = 0; p < kept.size(); p++) {
        const SVPair& pr = kept[p];
        AzgaarPropRange& r = outRanges[p];
        r.species = pr.species;
        r.variant = pr.variant;
        r.start   = pr.start;
        r.count   = pr.count;
        for (u32 i = 0; i < 3; i++) {
            r.aabbMin[i] = pr.aabbMin[i];
            r.aabbMax[i] = pr.aabbMax[i];
        }
    }
}

// ── Per-tile state + background worker ─────────────────────────────────────

struct PropsTileState {
    i32 tileX = 0, tileZ = 0;
    u64 readyStamp = 0;
    float camX = 0.0f, camZ = 0.0f;  // build-time camera (stale check + re-scatter)
    float camPos[3] = {};
    float planes[6][4] = {};
    bool havePlanes = false;
    bool building = false;  // a scatter job is in flight for this stamp
    u32 buildSeq = 0;       // publish counter (survives readyStamp resets)
    AzgaarPropsTile tile;   // immutable while building == false && valid
};

struct PropsScatterJob {
    i32 tileX, tileZ;
    u64 readyStamp;
};

static utils::Thread s_lock = {.cond = {}, .mutex = PTHREAD_MUTEX_INITIALIZER, .thread = {}};
static utils::Thread* s_worker = nullptr;
static bool s_shutdown = false;
static std::vector<PropsScatterJob> s_queue;
static u32 s_inFlight = 0;
static bool s_debug = false;
static bool s_disabled = false;

static const AzgaarWorld* s_world = nullptr;
static u32 s_mapSeed = 0;
static BiomeSpecies s_biomeSpecies[AZGAAR_PROPS_MAX_BIOMES];
static std::vector<PropsTileState> s_tiles;
static std::vector<AzgaarGrassVariantInfo> s_grassVariants;
// Defaults are the render pass' own defaults; the old engine's 0.10 rad/s /
// 0.15 m read as static — 0.60/0.35 makes the idle sway actually visible.
static AzgaarPropsWind s_wind = {0.70710678f, 0.70710678f, 0.60f, 0.35f};
// Window-settled summary state (one-shot per world load; see
// azgaarPropsUpdate): first-claim stamp + "all resident tiles published".
static double s_firstClaimNanos = 0.0;
static bool s_windowSummaryLogged = false;
// Streaming counters for azgaarPropsStats (phase-7 dolly acceptance:
// the follow/evict band under camera motion) + worker throughput.
static u32 s_statClaims = 0;
static u32 s_statRescatters = 0;
static u32 s_statStampRebuilds = 0;
static u32 s_statEvictions = 0;
static double s_buildNanosTotal = 0.0;
static u32 s_buildCount = 0;

// ── Grass texture variants (loaded at init, read by the scatter + task 4) ──

// Measure the EMPTY BOTTOM BAND of a grass card texture: the tuft images do
// not reach the image's bottom edge, so a card placed exactly on the ground
// shows its tuft floating. Returns the V of the lowest row the fragment
// shader's 0.5 alpha test keeps (1.0 = no trim). Private decode: the pixels
// are only needed here.
static float grassMeasureBottomV(const char* path) {
    if (!utils::dataManagerFileExists(path)) return 1.0f;
    utils::Image img = utils::imageLoad(path);
    float bottomV    = 1.0f;
    if (img.data && img.channels >= 4 && img.depth == 1 && img.width > 0 &&
        img.height > 1) {
        const u8* px     = static_cast<const u8*>(img.data);
        u32 w            = static_cast<u32>(img.width);
        u32 h            = static_cast<u32>(img.height);
        u32 ch           = static_cast<u32>(img.channels);
        u32 lastRow      = 0;
        bool foundRow    = false;
        for (u32 y = h - 1; y > 0; y--) {
            bool any = false;
            for (u32 x = 0; x < w; x++) {
                if (px[((y * w + x) * ch) + 3] > 128) {
                    any = true;
                    break;
                }
            }
            if (any) {
                lastRow  = y;
                foundRow = true;
                break;
            }
        }
        if (foundRow) {
            bottomV = (lastRow == 0) ? 0.0f
                                     : static_cast<float>(lastRow) / static_cast<float>(h - 1);
        }
    }
    utils::imageDestory(&img);
    return bottomV;
}

static void propsLoadGrassVariants(void) {
    s_grassVariants.clear();
    for (u32 i = 0; i < kGrassTexCount; i++) {
        if (!utils::dataManagerFileExists(kGrassTexPaths[i])) {
            utils::warn("azgaarProps: grass texture not found: %s", kGrassTexPaths[i]);
            continue;
        }
        utils::Image img = utils::imageLoad(kGrassTexPaths[i]);
        float aspect     = (img.width > 0 && img.height > 0)
                               ? static_cast<float>(img.width) / static_cast<float>(img.height)
                               : 1.0f;
        utils::imageDestory(&img);
        AzgaarGrassVariantInfo v = {};
        v.path    = kGrassTexPaths[i];
        v.aspect  = aspect;
        v.bottomV = grassMeasureBottomV(kGrassTexPaths[i]);
        v.loaded  = true;
        s_grassVariants.push_back(v);
    }
}

// ── Worker (single background thread; jobs are per-tile, so serializing ────
// keeps the scatter bit-identical to the old engine's pool-parallel one) ────

static void* propsWorkerMain(void* _) {
    (void)_;
    utils::threadSetName("azgaarPropsScatter");
    for (;;) {
        PropsScatterJob job;
        bool haveJob = false;
        {
            utils::threadLock(&s_lock);
            while (s_queue.empty() && !s_shutdown) utils::threadWait(&s_lock);
            if (!s_queue.empty()) {
                job     = s_queue.front();
                s_queue.erase(s_queue.begin());
                haveJob = true;
                s_inFlight++;
            }
            utils::threadUnlock(&s_lock);
        }
        if (!haveJob) break;  // shutdown with an empty queue

        double t0            = utils::nanos();
        const AzgaarWorld* world = s_world;
        u32 mapSeed                 = s_mapSeed;
        u32 grassVc                 = static_cast<u32>(s_grassVariants.size());

        // Find the claimed entry (its camera state was captured at enqueue).
        PropsTileState* state = nullptr;
        {
            utils::threadLock(&s_lock);
            for (PropsTileState& t : s_tiles) {
                if (t.tileX == job.tileX && t.tileZ == job.tileZ &&
                    t.readyStamp == job.readyStamp && t.building) {
                    state = &t;
                    break;
                }
            }
            utils::threadUnlock(&s_lock);
        }

        bool done = false;
        if (state && world) {
            engine::HeightmapTerrain* ht = engine::heightmapTerrainGetActive();
            // Lock-safe copies of the tile's grids (the worker must not touch
            // the tile table while the builder thread publishes / eviction
            // frees).
            std::vector<float> heights(HEIGHTMAP_TEX * HEIGHTMAP_TEX);
            std::vector<float> phys(HEIGHTMAP_PHYSICS_PSN * HEIGHTMAP_PHYSICS_PSN);
            done = ht != nullptr &&
                   engine::heightmapTerrainCopyTile(ht, job.tileX, job.tileZ, heights.data()) &&
                   engine::heightmapTerrainCopyPhysicsTile(ht, job.tileX, job.tileZ,
                                                           phys.data());
            if (done) {
                float originX = static_cast<float>(job.tileX) * ht->tileSizeMeters;
                float originZ = static_cast<float>(job.tileZ) * ht->tileSizeMeters;
                PropsScatterInput in = {};
                in.world             = world;
                in.mapSeed           = mapSeed;
                in.biomeSpecies      = s_biomeSpecies;
                in.grassVariantCount = grassVc;
                in.tileX             = job.tileX;
                in.tileZ             = job.tileZ;
                in.originX           = originX;
                in.originZ           = originZ;
                in.tileSize          = ht->tileSizeMeters;
                in.heights           = heights.data();
                in.physHeights       = phys.data();
                in.camX              = state->camX;
                in.camZ              = state->camZ;
                memcpy(in.camPos, state->camPos, sizeof(in.camPos));
                in.havePlanes        = state->havePlanes;
                for (int i = 0; i < 6; i++)
                    for (int c = 0; c < 4; c++) in.planes[i][c] = state->planes[i][c];

                AzgaarPropsTile out;
                u32 perSpecies[AZGAAR_PROP_COUNT] = {};
                propsScatterTile(in, out.instances, out.ranges, perSpecies);

                utils::threadLock(&s_lock);
                // Re-find: the entry may have been replaced (new stamp) or
                // evicted while the job ran.
                PropsTileState* t = nullptr;
                for (PropsTileState& st : s_tiles) {
                    if (st.tileX == job.tileX && st.tileZ == job.tileZ &&
                        st.readyStamp == job.readyStamp && st.building) {
                        t = &st;
                        break;
                    }
                }
                if (t) {
                    t->tile            = std::move(out);
                    t->tile.tileX      = job.tileX;
                    t->tile.tileZ      = job.tileZ;
                    t->tile.readyStamp = job.readyStamp;
                    t->tile.buildSeq   = ++t->buildSeq;
                    t->tile.valid      = true;
                    for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++)
                        t->tile.perSpecies[s] = perSpecies[s];
                    t->building = false;
                    s_buildNanosTotal += utils::nanos() - t0;
                    s_buildCount++;
                    if (s_debug) {
                        utils::info(
                            "azgaarProps: tile(%d,%d) built %zu instances in %.1f ms "
                            "(grass %u, conifer %u, deciduous %u, rock %u, ranges %zu)",
                            job.tileX, job.tileZ,
                            static_cast<size_t>(t->tile.instances.size()),
                            static_cast<float>(utils::nanos() - t0) / 1e6f,
                            perSpecies[AZGAAR_PROP_GRASS_TUFT],
                            perSpecies[AZGAAR_PROP_CONIFER],
                            perSpecies[AZGAAR_PROP_DECIDUOUS],
                            perSpecies[AZGAAR_PROP_ROCK],
                            static_cast<size_t>(t->tile.ranges.size()));
                    }
                }
                s_inFlight--;
                utils::threadSignal(&s_lock);  // unblock destroy's in-flight wait
                utils::threadUnlock(&s_lock);
            }
        }
        if (!done) {
            // Tile was evicted mid-job (or its entry gone): release the claim.
            utils::threadLock(&s_lock);
            for (PropsTileState& t : s_tiles) {
                if (t.tileX == job.tileX && t.tileZ == job.tileZ &&
                    t.readyStamp == job.readyStamp)
                    t.building = false;
            }
            s_inFlight--;
            utils::threadSignal(&s_lock);
            utils::threadUnlock(&s_lock);
        }
    }
    return nullptr;
}

// ── One-shot acceptance self-check ─────────────────────────────────────────
// Runs once the first resident tile is scattered:
//   (i)   re-scattering a tile with its recorded build-time inputs must be
//         bit-identical (the determinism contract);
//   (ii)  every instance Y must equal heightmapGridBilinear over the 256^2
//         physics grid at its own (x,z) (props sit exactly on the surface).
static bool s_acceptanceRan = false;

static void propsAcceptanceCheck(void) {
    if (s_acceptanceRan) return;
    engine::HeightmapTerrain* ht = engine::heightmapTerrainGetActive();
    if (!ht || !s_world) return;

    // First valid tile with instances.
    PropsTileState* t = nullptr;
    {
        utils::threadLock(&s_lock);
        for (PropsTileState& st : s_tiles) {
            if (st.tile.valid && !st.tile.instances.empty()) {
                t = &st;
                break;
            }
        }
        if (!t) {
            utils::threadUnlock(&s_lock);
            return;
        }
        s_acceptanceRan = true;  // fire at most once
        utils::threadUnlock(&s_lock);
    }

    std::vector<float> heights(HEIGHTMAP_TEX * HEIGHTMAP_TEX);
    std::vector<float> phys(HEIGHTMAP_PHYSICS_PSN * HEIGHTMAP_PHYSICS_PSN);
    if (!engine::heightmapTerrainCopyTile(ht, t->tileX, t->tileZ, heights.data()) ||
        !engine::heightmapTerrainCopyPhysicsTile(ht, t->tileX, t->tileZ, phys.data())) {
        utils::warn("azgaarProps acceptance: tile(%d,%d) grids unavailable", t->tileX,
                    t->tileZ);
        return;
    }

    float originX = static_cast<float>(t->tileX) * ht->tileSizeMeters;
    float originZ = static_cast<float>(t->tileZ) * ht->tileSizeMeters;
    PropsScatterInput in = {};
    in.world             = s_world;
    in.mapSeed           = s_mapSeed;
    in.biomeSpecies      = s_biomeSpecies;
    in.grassVariantCount = static_cast<u32>(s_grassVariants.size());
    in.tileX             = t->tileX;
    in.tileZ             = t->tileZ;
    in.originX           = originX;
    in.originZ           = originZ;
    in.tileSize          = ht->tileSizeMeters;
    in.heights           = heights.data();
    in.physHeights       = phys.data();
    in.camX              = t->camX;
    in.camZ              = t->camZ;
    memcpy(in.camPos, t->camPos, sizeof(in.camPos));
    in.havePlanes        = t->havePlanes;
    for (int i = 0; i < 6; i++)
        for (int c = 0; c < 4; c++) in.planes[i][c] = t->planes[i][c];

    std::vector<AzgaarPropInstance> rs;
    std::vector<AzgaarPropRange> rr;
    u32 perSpecies[AZGAAR_PROP_COUNT] = {};
    propsScatterTile(in, rs, rr, perSpecies);

    const AzgaarPropsTile& stored = t->tile;
    bool identical = rs.size() == stored.instances.size() &&
                     (rs.empty() ||
                      memcmp(rs.data(), stored.instances.data(),
                             rs.size() * sizeof(AzgaarPropInstance)) == 0);
    utils::info("azgaarProps acceptance: tile(%d,%d) re-scatter %zu instances %s", t->tileX,
                t->tileZ, rs.size(), identical ? "bit-identical" : "MISMATCH");

    // (ii) every instance Y on the physics-grid surface (epsilon 1e-4 m; both
    // sides evaluate the same bilinear function, so the error is rounding).
    const u32 PSN = HEIGHTMAP_PHYSICS_PSN;
    const float psnScale = static_cast<float>(PSN - 1u) / ht->tileSizeMeters;
    float maxErr = 0.0f;
    for (const AzgaarPropInstance& inst : stored.instances) {
        float y = engine::heightmapGridBilinear(
            phys.data(), PSN,
            (inst.pos[0] - originX) * psnScale, (inst.pos[2] - originZ) * psnScale);
        float err = fabsf(inst.pos[1] - y);
        if (err > maxErr) maxErr = err;
    }
    bool onSurface = maxErr < 1e-4f;
    utils::info("azgaarProps acceptance: Y-on-surface maxErr %.6f m %s", maxErr,
                onSurface ? "PASS" : "FAIL");
}

// ── Public API ──────────────────────────────────────────────────────────────

void azgaarPropsInit(const AzgaarWorld* world) {
    if (!world) return;
    azgaarPropsDestroy();  // idempotent

    s_debug    = getenv("ENGINE_AZGAAR_PROPS_DEBUG") != nullptr;
    s_disabled = getenv("ENGINE_AZGAAR_PROPS_DISABLED") != nullptr;

    // Map seed for deterministic scatter (FNV-1a of the map name, same scheme
    // as the old engine / the heightmap detail seed).
    u32 h = 2166136261u;
    for (const char* p = world->mapName; *p; p++) {
        h ^= static_cast<u32>(static_cast<unsigned char>(*p));
        h *= 16777619u;
    }
    u32 seed = h ? h : 1u;

    utils::threadLock(&s_lock);
    s_world      = world;
    s_mapSeed    = seed;
    precomputeBiomeSpecies(world, s_biomeSpecies, AZGAAR_PROPS_MAX_BIOMES);
    propsLoadGrassVariants();

    // Wind: the old engine drove this from its weather module; use the map's
    // authored wind direction (or a 45 deg default) with the visible-sway
    // defaults (its stock 0.10 rad/s / 0.15 m read as static).
    float deg = world->winds[0] != 0.0f ? world->winds[0] : 45.0f;
    float rad = deg * static_cast<float>(M_PI) / 180.0f;
    s_wind.dirX     = cosf(rad);
    s_wind.dirZ     = sinf(rad);
    s_wind.speed    = 0.60f;
    s_wind.strength = 0.35f;
    utils::threadUnlock(&s_lock);

    // Merged species mesh (task 4). Race-free here: the worker was joined by
    // the destroy() above and is not restarted until below, so nothing else
    // reads s_grassVariants while the build reads it.
    azgaarPropMeshBuild();

    if (!s_worker) {
        s_shutdown = false;
        s_worker   = utils::threadNew(propsWorkerMain, nullptr);
    }
    utils::info("azgaarProps: init (mapSeed=0x%08x, grass variants=%u, biomes=%u)%s", seed,
                static_cast<u32>(s_grassVariants.size()), world->biomeCount,
                s_disabled ? " (DISABLED)" : "");
}

void azgaarPropsUpdate(void) {
    if (!s_world || s_disabled) return;

    engine::HeightmapTerrain* ht = engine::heightmapTerrainGetActive();
    if (!ht || !ht->initialized) return;

    u32 cap = ht->windowSize * ht->windowSize + 4u;
    std::vector<engine::HeightmapTileView> views(cap);
    u32 n = engine::heightmapTerrainSnapshotTiles(ht, views.data(), cap);

    // Build-time camera for the scatter (near-player guarantee) + the
    // frustum for the scatter-time cull.
    f32 camPos[3] = {0.0f, 0.0f, 0.0f};
    f32 camFwd[3] = {0.0f, 0.0f, -1.0f};
    engine::renderer::rendererCameraGet(camPos, camFwd);
    float aspect = (engine::window.height > 0)
                       ? static_cast<float>(engine::window.width) /
                             static_cast<float>(engine::window.height)
                       : 16.0f / 9.0f;
    float planes[6][4];
    propsBuildFrustumPlanes(camPos, camFwd, aspect, planes);

    // Distance (m) from a point to the tile's world AABB (0 when inside).
    auto tileDist = [](float cx, float cz, float ox, float oz, float size) {
        float dx = (cx < ox)          ? (ox - cx)
                   : (cx > ox + size) ? (cx - (ox + size))
                                      : 0.0f;
        float dz = (cz < oz)          ? (oz - cz)
                   : (cz > oz + size) ? (cz - (oz + size))
                                      : 0.0f;
        return sqrtf(dx * dx + dz * dz);
    };

    for (u32 i = 0; i < n; i++) {
        const engine::HeightmapTileView& v = views[i];
        bool claimed = false;
        const char* claimReason = nullptr;  // "new" | "stamp" | "re-scatter"
        {
            utils::threadLock(&s_lock);
            PropsTileState* tile = nullptr;
            for (PropsTileState& st : s_tiles) {
                if (st.tileX == v.tileX && st.tileZ == v.tileZ) {
                    tile = &st;
                    break;
                }
            }
            if (!tile) {
                PropsTileState ns = {};
                ns.tileX        = v.tileX;
                ns.tileZ        = v.tileZ;
                ns.readyStamp   = v.readyStamp;
                ns.camX         = camPos[0];
                ns.camZ         = camPos[2];
                ns.camPos[0]    = camPos[0];
                ns.camPos[1]    = camPos[1];
                ns.camPos[2]    = camPos[2];
                ns.havePlanes   = true;
                ns.building     = true;
                for (int k = 0; k < 6; k++)
                    for (int c = 0; c < 4; c++) ns.planes[k][c] = planes[k][c];
                s_tiles.push_back(ns);
                if (s_firstClaimNanos == 0.0) s_firstClaimNanos = utils::nanos();
                claimed    = true;
                claimReason = "new";
                s_statClaims++;
            } else if (!tile->building) {
                // Camera-stale re-scatter: the tile's baked cull set was made
                // for a camera more than 100 m (AZGAAR_PROPS_RESCATTER_DIST) away,
                // and the tile is
                // near the current or the old camera.
                float dxc = camPos[0] - tile->camX;
                float dzc = camPos[2] - tile->camZ;
                bool camMoved = dxc * dxc + dzc * dzc >
                                AZGAAR_PROPS_RESCATTER_DIST * AZGAAR_PROPS_RESCATTER_DIST;
                bool nearNow = tileDist(camPos[0], camPos[2], v.originX, v.originZ, v.sizeMeters) <
                               AZGAAR_PROPS_RESCATTER_DIST;
                bool nearThen =
                    tileDist(tile->camX, tile->camZ, v.originX, v.originZ, v.sizeMeters) <
                    AZGAAR_PROPS_RESCATTER_DIST;
                bool stale = camMoved && (nearNow || nearThen);
                if (tile->readyStamp != v.readyStamp || stale) {
                    const bool stampChanged = tile->readyStamp != v.readyStamp;
                    claimReason = stampChanged ? "stamp" : "re-scatter";
                    if (stampChanged) {
                        s_statStampRebuilds++;
                    } else {
                        s_statRescatters++;
                    }
                    tile->readyStamp = v.readyStamp;
                    tile->camX       = camPos[0];
                    tile->camZ       = camPos[2];
                    tile->camPos[0]  = camPos[0];
                    tile->camPos[1]  = camPos[1];
                    tile->camPos[2]  = camPos[2];
                    tile->havePlanes = true;
                    for (int k = 0; k < 6; k++)
                        for (int c = 0; c < 4; c++) tile->planes[k][c] = planes[k][c];
                    tile->tile.valid = false;
                    tile->building   = true;
                    claimed          = true;
                }
            }
            utils::threadUnlock(&s_lock);
        }
        if (claimed) {
            if (s_debug)
                utils::info("azgaarProps: claim tile(%d,%d) %s", v.tileX, v.tileZ, claimReason);
            utils::threadLock(&s_lock);
            s_queue.push_back({v.tileX, v.tileZ, v.readyStamp});
            utils::threadSignal(&s_lock);
            utils::threadUnlock(&s_lock);
        }
    }

    // Drop CPU state for tiles that left the window (their scatter is
    // regenerated bit-identically if they come back).
    {
        utils::threadLock(&s_lock);
        for (i32 i = static_cast<i32>(s_tiles.size()) - 1; i >= 0; i--) {
            PropsTileState& t = s_tiles[i];
            if (t.building) continue;
            bool resident = false;
            for (u32 k = 0; k < n; k++) {
                if (views[k].tileX == t.tileX && views[k].tileZ == t.tileZ) {
                    resident = true;
                    break;
                }
            }
            if (!resident) {
                if (s_debug)
                    utils::info("azgaarProps: evict tile(%d,%d) (%zu instances) — left window",
                            t.tileX, t.tileZ, t.tile.instances.size());
                s_statEvictions++;
                s_tiles.erase(s_tiles.begin() + static_cast<u32>(i));
            }
        }
        utils::threadUnlock(&s_lock);
    }

    // One-shot emission summary (phase-7 forensics: "why do only some tiles
    // emit draws?"): the single scatter worker builds every resident tile
    // (~150 ms each); tiles lying entirely beyond the per-species XZ caps
    // publish 0 instances and emit no draws — expected with 2048 m tiles vs
    // 440-840 m caps (at most the camera tile plus a border neighbour
    // survive), NOT a scatter stall.
    {
        utils::threadLock(&s_lock);
        bool allBuilt = n > 0 && s_tiles.size() >= n;
        for (const PropsTileState& t : s_tiles)
            if (t.building) allBuilt = false;
        if (allBuilt && !s_windowSummaryLogged) {
            s_windowSummaryLogged = true;
            u32 withInst = 0, instTotal = 0;
            for (const PropsTileState& t : s_tiles) {
                if (!t.tile.instances.empty()) {
                    withInst++;
                    instTotal += static_cast<u32>(t.tile.instances.size());
                }
            }
            const double firstClaimS = s_firstClaimNanos > 0.0
                                           ? (utils::nanos() - s_firstClaimNanos) / 1e9
                                           : 0.0;
            utils::info(
                    "azgaarProps: %zu resident tiles built in %.1f s, %u kept instances "
                    "and emit draws (%u instances total); %zu kept 0 (fully beyond "
                    "the XZ cull caps)",
                    s_tiles.size(), firstClaimS, withInst, instTotal,
                    s_tiles.size() - withInst);
        }
        utils::threadUnlock(&s_lock);
    }

    propsAcceptanceCheck();
}

void azgaarPropsDestroy(void) {
    {
        utils::threadLock(&s_lock);
        // Wait for in-flight scatter jobs (they hold s_world); the caller
        // releases the world only AFTER this returns.
        s_world = nullptr;
        while (s_inFlight > 0) utils::threadWait(&s_lock);
        s_tiles.clear();
        s_queue.clear();
        for (auto& b : s_biomeSpecies) b = BiomeSpecies{};
        s_grassVariants.clear();
        azgaarPropMeshRelease();
        s_wind         = {0.70710678f, 0.70710678f, 0.60f, 0.35f};
        utils::threadUnlock(&s_lock);
    }
    if (s_worker) {
        utils::threadLock(&s_lock);
        s_shutdown = true;
        utils::threadSignal(&s_lock);
        utils::threadUnlock(&s_lock);
        utils::threadJoin(s_worker);
        utils::threadDestroy(s_worker);
        s_worker = nullptr;
    }
    s_acceptanceRan     = false;
    s_firstClaimNanos   = 0.0;
    s_windowSummaryLogged = false;
    s_statClaims        = 0;
    s_statRescatters    = 0;
    s_statStampRebuilds = 0;
    s_statEvictions     = 0;
    s_buildNanosTotal   = 0.0;
    s_buildCount        = 0;
}

const AzgaarPropsTile* azgaarPropsGetTile(i32 tileX, i32 tileZ, u64 readyStamp) {
    for (const PropsTileState& t : s_tiles) {
        if (t.tileX == tileX && t.tileZ == tileZ && t.readyStamp == readyStamp &&
            t.tile.valid) {
            return &t.tile;
        }
    }
    return nullptr;
}

AzgaarPropsStats azgaarPropsStats(void) {
    AzgaarPropsStats st = {};
    utils::threadLock(&s_lock);
    st.queueDepth = (u32)s_queue.size();
    for (const PropsTileState& t : s_tiles) {
        st.resident++;
        if (t.tile.valid) {
            st.built++;
            st.instances += (u32)t.tile.instances.size();
        }
        st.cpuBytes += t.tile.instances.size() * sizeof(AzgaarPropInstance) +
                       t.tile.ranges.size() * sizeof(AzgaarPropRange);
    }
    st.claims        = s_statClaims;
    st.rescatters    = s_statRescatters;
    st.stampRebuilds = s_statStampRebuilds;
    st.evictions     = s_statEvictions;
    st.workerBuilds  = s_buildCount;
    st.workerAvgMs   = s_buildCount ? s_buildNanosTotal / 1e6 / s_buildCount : 0.0;
    utils::threadUnlock(&s_lock);
    return st;
}

const AzgaarPropsWind* azgaarPropsWind(void) {
    return &s_wind;
}

float azgaarPropsSpeciesSway(u32 species) {
    if (species >= AZGAAR_PROP_COUNT) return 0.0f;
    return kSpecies[species].sway;
}

u32 azgaarPropsSpeciesRenderFlags(u32 species) {
    switch (species) {
        // Thin double-sided vegetation (matches the old engine's per-species
        // Nlight): both faces of a blade face the sky, so both must catch the
        // sun. The built-in lit model's doubleSided normal flip would leave the
        // back of each blade with a down-facing normal (NdotL == 0) and render
        // half of every tuft near-black, so these species light both faces with
        // the unflipped normal.
        case AZGAAR_PROP_GRASS_TUFT:  return AZGAAR_PROPS_FLAG_ALPHA_TEST | AZGAAR_PROPS_FLAG_DOUBLE_SIDED;
        case AZGAAR_PROP_PALM:        return AZGAAR_PROPS_FLAG_DOUBLE_SIDED;
        case AZGAAR_PROP_REED:        return AZGAAR_PROPS_FLAG_DOUBLE_SIDED;
        case AZGAAR_PROP_FLOWER:      return AZGAAR_PROPS_FLAG_FLOWER | AZGAAR_PROPS_FLAG_DOUBLE_SIDED;
        default:                       return 0;
    }
}

u32 azgaarPropsGrassVariantCount(void) {
    return static_cast<u32>(s_grassVariants.size());
}

float azgaarPropsBiomeDensity(const AzgaarWorld* world, u32 biomeId, bool treesOnly) {
    if (!world || !s_world || world != s_world) return 0.0f;
    if (biomeId >= world->biomeCount || biomeId >= AZGAAR_PROPS_MAX_BIOMES) return 0.0f;
    const BiomeSpecies* bs = &s_biomeSpecies[biomeId];
    if (bs->count == 0 || bs->totalWeight == 0) return 0.0f;
    float density = 0.0f;
    for (u32 s = 0; s < bs->count; s++) {
        u32 sp = bs->species[s];
        if (sp >= AZGAAR_PROP_COUNT) continue;
        if (treesOnly && !propsIsTreeSpecies(sp)) continue;
        density += kSpeciesDensity[sp] *
                   (static_cast<float>(bs->weight[s]) / static_cast<float>(bs->totalWeight));
    }
    density *= static_cast<float>(world->biomes[biomeId].iconsDensity) / 120.0f;
    return density;
}

const AzgaarGrassVariantInfo* azgaarPropsGrassVariant(u32 i) {
    if (i >= s_grassVariants.size()) return nullptr;
    return &s_grassVariants[i];
}

}  // namespace game
