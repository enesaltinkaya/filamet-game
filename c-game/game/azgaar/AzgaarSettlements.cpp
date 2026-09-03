#include "azgaar/AzgaarSettlements.h"
#include <math.h>
#include <stdlib.h>

// Minimal phase-4 port of the old engine's plateau half of AzgaarSettlements
// (workstream D of plans/azgaar-world-population.md): the D8 terrain
// plateau — blends the natural height toward the settlement's flatY so
// towns sit on level ground:  y' = y + (flatY - y) * (1 - smoothstep(0.55r,
// r, d)).  Building clusters / props upload / nearest query are phase 8.
namespace game {
static float settSmoothstep01(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// ── State ──────────────────────────────────────────────────────────────────

static bool g_disabled = false;

// Plateau spatial grid (D8): 1024 m buckets over the map AABB.
struct SettGrid {
    float  invBucket;
    float  minX, minZ;
    u32    gridW, gridH;
    std::vector<u32> starts; // bucket -> start index into cells
    std::vector<u32> cells;  // settlement indices sorted by bucket
    u32    count;
};
static SettGrid g_grid = {};

// ── Grid builder ───────────────────────────────────────────────────────────

// Builds the plateau grid into `out` (caller zeroed).  `out->count` is set
// LAST so a concurrent reader (the heightmap build thread, which calls
// azgaarHeightmapHeightAt -> azgaarSettlementsPlateauY) only ever sees either
// count==0 (returns the natural height) or a fully built grid.  Publishing
// `count` last makes the publication safe.
static void settGridBuild(const AzgaarWorld* world, SettGrid* out) {
    *out = SettGrid{};
    if (!world || world->settlementCount == 0) return;

    const float BUCKET = 1024.0f;
    float halfW = static_cast<float>(world->widthPx * 0.5) * static_cast<float>(world->metersPerPixel) + 40.0f;
    float halfH = static_cast<float>(world->heightPx * 0.5) * static_cast<float>(world->metersPerPixel) + 40.0f;
    out->invBucket = 1.0f / BUCKET;
    out->minX      = -halfW;
    out->minZ      = -halfH;
    out->gridW     = static_cast<u32>(2.0f * halfW / BUCKET) + 1u;
    out->gridH     = static_cast<u32>(2.0f * halfH / BUCKET) + 1u;
    u32 buckets = out->gridW * out->gridH;
    out->starts.resize(buckets + 1u);
    out->cells.resize(world->settlementCount);

    std::vector<u32> counts(buckets, 0u);
    for (u32 i = 0; i < world->settlementCount; i++) {
        const AzgaarSettlement* s = &world->settlements[i];
        i32 bx = static_cast<i32>((s->wx - out->minX) * out->invBucket);
        i32 bz = static_cast<i32>((s->wz - out->minZ) * out->invBucket);
        if (bx < 0) bx = 0;
        if (bz < 0) bz = 0;
        if (bx >= static_cast<i32>(out->gridW)) bx = static_cast<i32>(out->gridW) - 1;
        if (bz >= static_cast<i32>(out->gridH)) bz = static_cast<i32>(out->gridH) - 1;
        counts[static_cast<u32>(bz) * out->gridW + static_cast<u32>(bx)]++;
    }
    u32 acc = 0;
    for (u32 b = 0; b < buckets; b++) {
        out->starts[b] = acc;
        acc += counts[b];
    }
    out->starts[buckets] = acc;
    std::vector<u32> cursor(out->starts.begin(), out->starts.end() - 1);
    for (u32 i = 0; i < world->settlementCount; i++) {
        const AzgaarSettlement* s = &world->settlements[i];
        i32 bx = static_cast<i32>((s->wx - out->minX) * out->invBucket);
        i32 bz = static_cast<i32>((s->wz - out->minZ) * out->invBucket);
        if (bx < 0) bx = 0;
        if (bz < 0) bz = 0;
        if (bx >= static_cast<i32>(out->gridW)) bx = static_cast<i32>(out->gridW) - 1;
        if (bz >= static_cast<i32>(out->gridH)) bz = static_cast<i32>(out->gridH) - 1;
        u32 b = static_cast<u32>(bz) * out->gridW + static_cast<u32>(bx);
        out->cells[cursor[b]++] = i;
    }
    // Publish last: concurrent readers only see count==0 or the finished grid.
    out->count = world->settlementCount;
}

// ── API ────────────────────────────────────────────────────────────────────

void azgaarSettlementsPlateauInit(const AzgaarWorld* world) {
    // Same convention as the old engine: env var set to any value disables.
    g_disabled = getenv("ENGINE_AZGAAR_SETTLE_DISABLED") != nullptr;
    settGridBuild(world, &g_grid);
}

void azgaarSettlementsPlateauClear(void) {
    g_grid     = SettGrid{};
    g_disabled = false;
}

float azgaarSettlementsPlateauY(const AzgaarWorld* world, float wx, float wz, float naturalY) {
    if (g_disabled || !world) return naturalY;

    // Snapshot the grid state at entry: an in-flight call keeps its snapshot
    // even if the game thread swaps in a fresh grid mid-call.
    u32   count    = g_grid.count;
    const u32*  starts   = g_grid.starts.empty() ? nullptr : g_grid.starts.data();
    const u32*  cells    = g_grid.cells.empty() ? nullptr : g_grid.cells.data();
    if (count == 0 || !starts || !cells) return naturalY;
    float invBucket = g_grid.invBucket;
    float minX      = g_grid.minX;
    float minZ      = g_grid.minZ;
    u32   gridW     = g_grid.gridW;
    u32   gridH     = g_grid.gridH;
    u32   buckets   = gridW * gridH;

    float y = naturalY;
    i32 bx = static_cast<i32>((wx - minX) * invBucket);
    i32 bz = static_cast<i32>((wz - minZ) * invBucket);
    for (i32 oz = -1; oz <= 1; oz++) {
        for (i32 ox = -1; ox <= 1; ox++) {
            i32 nx = bx + ox, nz = bz + oz;
            if (nx < 0 || nz < 0 || nx >= static_cast<i32>(gridW) || nz >= static_cast<i32>(gridH)) continue;
            u32 b = static_cast<u32>(nz) * gridW + static_cast<u32>(nx);
            u32 lo = starts[b];
            u32 hi = (b + 1 < buckets) ? starts[b + 1] : count;
            for (u32 i = lo; i < hi; i++) {
                const AzgaarSettlement* s = &world->settlements[cells[i]];
                float dx = wx - s->wx;
                float dz = wz - s->wz;
                float d = sqrtf(dx * dx + dz * dz);
                if (d < s->radiusM) {
                    // D8: y' = mix(y, flatY, 1 - smoothstep(0.55r, r, d))
                    float t = 1.0f - settSmoothstep01(0.55f * s->radiusM, s->radiusM, d);
                    if (t > 0.0f) y += (s->flatY - y) * t;
                }
            }
        }
    }
    return y;
}
}  // namespace game
