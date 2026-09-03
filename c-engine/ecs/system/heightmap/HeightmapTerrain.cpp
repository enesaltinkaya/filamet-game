#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "thread/Thread.h"
#include "renderer/Renderer.h"

#include <cmath>
#include <climits>
#include <cstring>

/*
 * Threading model
 * ---------------
 * One process-wide builder worker consumes a global job queue
 * (HeightmapTerrain* + tile coords).  `heightmapLock` protects:
 *   - the job queue,
 *   - every HeightmapTerrain's tile table and tile state transitions,
 *   - the registered/inFlight bookkeeping.
 * The heavy grid generation runs with the lock released; the builder only
 * re-locks to publish (or discard) the grids.  Grid memory of a published
 * tile is freed exclusively by the main thread (eviction / destroyData),
 * and destroyData drains inFlight before freeing anything, so no other
 * locking is needed to read a READY grid.
 */

namespace engine {
struct HeightmapJob {
    HeightmapTerrain* ht;
    i32 tileX, tileZ;
};

static HeightmapTerrain* activeHeightmapTerrain = nullptr;
static u64 heightmapReadyCounter = 0; // global; bumped per READY publish

static utils::Thread heightmapLock = {.cond = {}, .mutex = PTHREAD_MUTEX_INITIALIZER, .thread = {}};
static utils::Thread* buildWorker  = nullptr;
static bool workerStarted   = false;
static std::vector<HeightmapJob> buildQueue;

static void* heightmapBuildThreadMain(void* userData);

void heightmapTerrainSetActive(HeightmapTerrain* ht) {
    activeHeightmapTerrain = ht;
}

HeightmapTerrain* heightmapTerrainGetActive(void) {
    return activeHeightmapTerrain;
}

// ── Grid helpers (lock-free: pure math on already-published data) ─────────

// Bilinear sample of a regular grid spanning [0, dim-1] (endpoints included).
float heightmapGridBilinear(const float* grid, u32 dim, float gx, float gz) {
    if (gx < 0.0f) gx = 0.0f;
    else if (gx > static_cast<float>(dim) - 1.0f) gx = static_cast<float>(dim) - 1.0f;
    if (gz < 0.0f) gz = 0.0f;
    else if (gz > static_cast<float>(dim) - 1.0f) gz = static_cast<float>(dim) - 1.0f;

    i32 x1 = static_cast<i32>(gx);
    i32 z1 = static_cast<i32>(gz);
    if (x1 >= static_cast<i32>(dim) - 1) x1 = static_cast<i32>(dim) - 2;
    if (z1 >= static_cast<i32>(dim) - 1) z1 = static_cast<i32>(dim) - 2;
    float tx = gx - static_cast<float>(x1);
    float tz = gz - static_cast<float>(z1);

    size_t stride = dim;
    float a = grid[z1 * stride + x1];
    float b = grid[z1 * stride + x1 + 1];
    float c = grid[(z1 + 1) * stride + x1];
    float d = grid[(z1 + 1) * stride + x1 + 1];
    return a + (b - a) * tx + (c + (d - c) * tx - (a + (b - a) * tx)) * tz;
}

// ── Tiles ──────────────────────────────────────────────────────────────────

static HeightmapTile* heightmapTerrainCreateTile(HeightmapTerrain* ht, i32 tileX, i32 tileZ) {
    HeightmapTile* tile = new HeightmapTile{
        .tileX          = tileX,
        .tileZ          = tileZ,
        .originX        = static_cast<float>(tileX) * ht->tileSizeMeters,
        .originZ        = static_cast<float>(tileZ) * ht->tileSizeMeters,
        .sizeMeters     = ht->tileSizeMeters,
        .state          = HEIGHTMAP_TILE_EMPTY,
        .inWindow       = false,
        .genMs          = 0.0,
        .readyStamp     = 0,
        .gpuData        = nullptr,
        .physicsData    = nullptr,
        .lruStamp       = 0,
    };
    ht->tiles.push_back(tile);
    return tile;
}

static void heightmapTerrainFreeTile(HeightmapTile* tile) {
    // Phase 2/3 hooks land here (GPU texture / Jolt body teardown) before
    // the delete; main thread only.
    delete tile;
}

HeightmapTile* heightmapTerrainGetTile(HeightmapTerrain* ht, i32 tileX, i32 tileZ) {
    if (!ht || !ht->initialized) return nullptr;
    for (HeightmapTile* tile : ht->tiles) {
        if (tile->tileX == tileX && tile->tileZ == tileZ) return tile;
    }
    return nullptr;
}

// Forward declaration (defined below; caller must hold heightmapLock).
static HeightmapTile* heightmapTerrainFindTile(HeightmapTerrain* ht, i32 tileX, i32 tileZ);

u32 heightmapTerrainSnapshotTiles(HeightmapTerrain* ht,
                                  HeightmapTileView* outViews,
                                  u32 cap) {
    if (!ht || !ht->initialized || !outViews) return 0;

    utils::threadLock(&heightmapLock);
    u32 written = 0;
    if (ht->registered) {
        for (HeightmapTile* tile : ht->tiles) {
            if (written >= cap) break;
            if (tile->state != HEIGHTMAP_TILE_READY || tile->heights.empty()) {
                continue;
            }
            outViews[written] = HeightmapTileView{
                .tileX      = tile->tileX,
                .tileZ      = tile->tileZ,
                .readyStamp = tile->readyStamp,
                .originX    = tile->originX,
                .originZ    = tile->originZ,
                .sizeMeters = tile->sizeMeters,
                .heights    = tile->heights.data(),
            };
            written++;
        }
    }
    utils::threadUnlock(&heightmapLock);
    return written;
}

bool heightmapTerrainCopyTile(HeightmapTerrain* ht,
                              i32 tileX,
                              i32 tileZ,
                              float* outHeights) {
    if (!ht || !ht->initialized || !outHeights) return false;

    utils::threadLock(&heightmapLock);
    bool copied = false;
    if (ht->registered) {
        HeightmapTile* tile = heightmapTerrainFindTile(ht, tileX, tileZ);
        if (tile && tile->state == HEIGHTMAP_TILE_READY && !tile->heights.empty()) {
            memcpy(outHeights, tile->heights.data(), sizeof(float) * static_cast<size_t>(HEIGHTMAP_TEX) * HEIGHTMAP_TEX);
            copied = true;
        }
    }
    utils::threadUnlock(&heightmapLock);
    return copied;
}

bool heightmapTerrainCopyPhysicsTile(HeightmapTerrain* ht,
                                     i32 tileX,
                                     i32 tileZ,
                                     float* outHeights) {
    if (!ht || !ht->initialized || !outHeights) return false;

    utils::threadLock(&heightmapLock);
    bool copied = false;
    if (ht->registered) {
        HeightmapTile* tile = heightmapTerrainFindTile(ht, tileX, tileZ);
        if (tile && tile->state == HEIGHTMAP_TILE_READY && !tile->physicsHeights.empty()) {
            memcpy(outHeights,
                   tile->physicsHeights.data(),
                   sizeof(float) * static_cast<size_t>(HEIGHTMAP_PHYSICS_PSN) *
                       static_cast<size_t>(HEIGHTMAP_PHYSICS_PSN));
            copied = true;
        }
    }
    utils::threadUnlock(&heightmapLock);
    return copied;
}

static HeightmapTile* heightmapTerrainFindTile(HeightmapTerrain* ht, i32 tileX, i32 tileZ) {
    // Caller holds heightmapLock.
    for (HeightmapTile* tile : ht->tiles) {
        if (tile->tileX == tileX && tile->tileZ == tileZ) return tile;
    }
    return nullptr;
}

// ── Generation ─────────────────────────────────────────────────────────────

// Runs on the builder thread, lock released. Fills the tile's CPU grids from
// the source. Border columns sample the exact same world coordinates as the
// neighbouring tiles' borders, so the surface is watertight.
static void heightmapTerrainGenerateGrids(HeightmapTerrain* ht, HeightmapTile* tile) {
    const u32 tex = HEIGHTMAP_TEX;
    const u32 psn = HEIGHTMAP_PHYSICS_PSN;

    double t0 = utils::nanos();

    std::vector<float> heights(static_cast<size_t>(tex) * tex);
    float step = tile->sizeMeters / static_cast<float>(tex - 1);
    for (u32 z = 0; z < tex; ++z) {
        float wz = tile->originZ + static_cast<float>(z) * step;
        for (u32 x = 0; x < tex; ++x) {
            float wx = tile->originX + static_cast<float>(x) * step;
            heights[static_cast<size_t>(z) * tex + x] = ht->source.heightAt(ht->source.userData, wx, wz);
        }
    }

    // Physics grid: bilinear resample of the fine grid. Endpoints coincide
    // with the fine grid's endpoints, so the physics surface shares tile
    // borders exactly (and borders with neighbouring physics grids).
    std::vector<float> physics(static_cast<size_t>(psn) * psn);
    float scale    = static_cast<float>(tex - 1) / static_cast<float>(psn - 1);
    for (u32 z = 0; z < psn; ++z) {
        float gz = static_cast<float>(z) * scale;
        for (u32 x = 0; x < psn; ++x) {
            physics[static_cast<size_t>(z) * psn + x] = heightmapGridBilinear(heights.data(), tex, static_cast<float>(x) * scale, gz);
        }
    }

    tile->heights        = std::move(heights);
    tile->physicsHeights = std::move(physics);
    tile->genMs          = (utils::nanos() - t0) / 1e6;
}

// Shared-border consistency check between a freshly READY tile and its
// already-READY neighbours. Border columns sample identical world positions,
// so any mismatch means a non-deterministic source (would surface as cracks
// or physics gaps at tile edges).  Caller holds heightmapLock.
static void heightmapTerrainSeamCheck(HeightmapTerrain* ht, HeightmapTile* tile) {
    const u32   tex     = HEIGHTMAP_TEX;
    const float tol     = 1e-4f;
    const char* pairs[2] = {"west", "north"};
    const i32   dx[2]    = {-1, 0};
    const i32   dz[2]    = {0, -1};

    for (int i = 0; i < 2; ++i) {
        HeightmapTile* nb = heightmapTerrainFindTile(ht, tile->tileX + dx[i], tile->tileZ + dz[i]);
        if (!nb || nb->state != HEIGHTMAP_TILE_READY || nb->heights.empty()) continue;

        bool bad = false;
        float diff = 0.0f;
        if (i == 0) {
            // West neighbour: its last column (x = tex-1) vs our first column.
            for (u32 z = 0; z < tex && !bad; ++z) {
                diff = fabsf(nb->heights[static_cast<size_t>(z) * tex + (tex - 1)] - tile->heights[static_cast<size_t>(z) * tex]);
                if (diff > tol) bad = true;
            }
        } else {
            // North neighbour (tileZ-1): its last row (z = tex-1) vs our first row.
            for (u32 x = 0; x < tex && !bad; ++x) {
                diff = fabsf(nb->heights[(size_t)(tex - 1) * tex + x] - tile->heights[x]);
                if (diff > tol) bad = true;
            }
        }
        if (bad) {
            ht->seamFailures++;
            utils::warn("heightmapTerrain: SEAM MISMATCH vs %s neighbour tile(%d,%d) at tile(%d,%d): diff=%.5f m",
                 pairs[i],
                 tile->tileX + dx[i],
                 tile->tileZ + dz[i],
                 tile->tileX,
                 tile->tileZ,
                 diff);
        }
    }
}

// Enqueue a job for an EMPTY tile and make sure the builder is running.
// Caller holds heightmapLock.
static void heightmapTerrainQueueJobLocked(HeightmapTerrain* ht, HeightmapTile* tile) {
    if (!ht->registered || tile->state != HEIGHTMAP_TILE_EMPTY) return;

    for (u32 i = 0; i < buildQueue.size(); ++i) {
        HeightmapJob* job = &buildQueue[i];
        if (job->ht == ht && job->tileX == tile->tileX && job->tileZ == tile->tileZ) return;
    }

    HeightmapJob job = {.ht = ht, .tileX = tile->tileX, .tileZ = tile->tileZ};
    buildQueue.push_back(job);

    if (!workerStarted) {
        workerStarted = true;
        buildWorker   = utils::threadNew(heightmapBuildThreadMain, nullptr);
    }
    utils::threadSignal(&heightmapLock);
}

static void* heightmapBuildThreadMain(void* _) {
    (void)_;
    utils::threadSetName("heightmapBuild");

    for (;;) {
        utils::threadLock(&heightmapLock);
        while (static_cast<i32>(buildQueue.size()) == 0) utils::threadWait(&heightmapLock);

        // Pop and claim in the same critical section so destroyData cannot
        // unregister/free the tile between the pop and the claim.
        HeightmapJob job = std::move(buildQueue.front());
        buildQueue.erase(buildQueue.begin());
        HeightmapTerrain* ht   = job.ht;
        HeightmapTile*    tile = nullptr;
        bool              claimed = false;
        if (ht->registered) {
            tile = heightmapTerrainFindTile(ht, job.tileX, job.tileZ);
            if (tile && tile->state == HEIGHTMAP_TILE_EMPTY) {
                tile->state   = HEIGHTMAP_TILE_GENERATING;
                ht->inFlight++;
                claimed       = true;
            }
        }
        utils::threadUnlock(&heightmapLock);

        if (!claimed) continue;

        heightmapTerrainGenerateGrids(ht, tile);

        utils::threadLock(&heightmapLock);
        ht->inFlight--;
        bool published = (tile->state == HEIGHTMAP_TILE_GENERATING);
        if (published) {
            tile->state        = HEIGHTMAP_TILE_READY;
            tile->readyStamp   = ++heightmapReadyCounter;
            ht->tilesReady++;
            ht->generatedTiles++;
            heightmapTerrainSeamCheck(ht, tile);
        } else {
            // Evicted or destroyed while generating: discard the grids.
            tile->heights.clear();
            tile->physicsHeights.clear();
            tile->state          = HEIGHTMAP_TILE_EMPTY;
        }
        u32 ready           = ht->tilesReady;
        u32 resident        = static_cast<u32>(static_cast<i32>(ht->tiles.size()));
        u32 window          = ht->windowSize;
        u32 seams           = ht->seamFailures;
        u64 total           = ht->generatedTiles;
        i32 logTx           = tile->tileX;
        i32 logTz           = tile->tileZ;
        double logMs        = tile->genMs;
        bool  logSeamsDirty = (seams > 0);
        utils::threadUnlock(&heightmapLock);

        if (published) {
            utils::info("heightmapTerrain: tile(%d,%d) ready in %.1f ms (ready=%u/%u resident=%u total=%llu)",
                 logTx,
                 logTz,
                 static_cast<float>(logMs),
                 ready,
                 window * window,
                 resident,
                 (unsigned long long)total);
        } else {
            utils::info("heightmapTerrain: tile(%d,%d) discarded after %.1f ms (evicted while generating)",
                 logTx,
                 logTz,
                 static_cast<float>(logMs));
        }
        if (logSeamsDirty) {
            utils::warn("heightmapTerrain: %u seam mismatch(es) so far", seams);
        }
    }
    return nullptr;
}

// ── Window / LRU (main thread or builder via updateWindow) ─────────────────

void heightmapTerrainUpdateWindow(HeightmapTerrain* ht, float anchorX, float anchorZ) {
    if (!ht || !ht->initialized) return;

    i32 cx   = heightmapWorldToTileCoord(ht, anchorX);
    i32 cz   = heightmapWorldToTileCoord(ht, anchorZ);
    i32 half = static_cast<i32>(ht->windowSize / 2);

    utils::threadLock(&heightmapLock);

    // Mark the window and create missing tiles.
    for (i32 z = cz - half; z <= cz + half; ++z) {
        for (i32 x = cx - half; x <= cx + half; ++x) {
            HeightmapTile* tile = heightmapTerrainFindTile(ht, x, z);
            if (!tile) tile = heightmapTerrainCreateTile(ht, x, z);
            tile->inWindow = true;
            tile->lruStamp = ++ht->lruCounter;
        }
    }

    // Queue generation for in-window tiles that have no data yet. Must run
    // before the eviction pass below, which clears the inWindow flags.
    // Queue nearest-to-anchor first so the tile under the player (and thus its
    // Jolt heightfield body) is generated and created before the outer ring,
    // which is what the character controller needs to stand on at spawn.
    {
        HeightmapTile* pending[64];
        u32           pendingCount = 0;
        for (HeightmapTile* tile : ht->tiles) {
            if (tile->inWindow && tile->state == HEIGHTMAP_TILE_EMPTY && pendingCount < 64) {
                pending[pendingCount++] = tile;
            }
        }
        // Stable-ish insertion sort by squared distance to the anchor tile.
        for (u32 i = 1; i < pendingCount; ++i) {
            HeightmapTile* key = pending[i];
            float         keyD = static_cast<float>(key->tileX - cx) * static_cast<float>(key->tileX - cx)
                                + static_cast<float>(key->tileZ - cz) * static_cast<float>(key->tileZ - cz);
            u32         j      = i;
            while (j > 0) {
                float prevD = static_cast<float>(pending[j - 1]->tileX - cx) * static_cast<float>(pending[j - 1]->tileX - cx)
                             + static_cast<float>(pending[j - 1]->tileZ - cz) * static_cast<float>(pending[j - 1]->tileZ - cz);
                if (prevD <= keyD) break;
                pending[j] = pending[j - 1];
                --j;
            }
            pending[j] = key;
        }
        for (u32 i = 0; i < pendingCount; ++i) {
            heightmapTerrainQueueJobLocked(ht, pending[i]);
        }
    }

    // Evict everything outside the window. GENERATING tiles stay resident
    // until the builder finishes (it may hold their buffers); they are
    // evicted by the next update.
    for (i32 i = static_cast<i32>(static_cast<i32>(ht->tiles.size())) - 1; i >= 0; --i) {
        HeightmapTile* tile = ht->tiles[i];
        if (tile->inWindow) {
            tile->inWindow = false;
            continue;
        }
        if (tile->state == HEIGHTMAP_TILE_GENERATING) continue;
        if (tile->state == HEIGHTMAP_TILE_READY) --ht->tilesReady;
        heightmapTerrainFreeTile(tile);
        ht->tiles.erase(ht->tiles.begin() + i);
    }

    i32  logCx        = (cx != ht->lastLoggedCx || cz != ht->lastLoggedCz) ? cx : -999999;
    i32  logCz        = (cx != ht->lastLoggedCx || cz != ht->lastLoggedCz) ? cz : -999999;
    u32  resident     = static_cast<u32>(static_cast<i32>(ht->tiles.size()));
    u32  ready        = ht->tilesReady;
    u32  inFlightN    = ht->inFlight;
    u64  queued       = static_cast<i32>(buildQueue.size());
    ht->lastLoggedCx  = cx;
    ht->lastLoggedCz  = cz;

    utils::threadUnlock(&heightmapLock);

    if (logCx != -999999) {
        utils::info("heightmapTerrain: window @ tile(%d,%d) anchor(%.0f,%.0f) resident=%u ready=%u inFlight=%u queued=%llu",
             logCx,
             logCz,
             anchorX,
             anchorZ,
             resident,
             ready,
             inFlightN,
             (unsigned long long)queued);
    }
}

void heightmapTerrainRequestGeneration(HeightmapTerrain* ht, i32 tileX, i32 tileZ) {
    if (!ht || !ht->initialized) return;
    utils::threadLock(&heightmapLock);
    HeightmapTile* tile = heightmapTerrainFindTile(ht, tileX, tileZ);
    if (tile) heightmapTerrainQueueJobLocked(ht, tile);
    utils::threadUnlock(&heightmapLock);
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

void heightmapTerrainInit(HeightmapTerrain* ht,
                          const HeightmapSource* source,
                          float tileSizeMeters,
                          u32 windowSize) {
    if (!ht || !source || !source->heightAt) {
        utils::error("heightmapTerrainInit: invalid args (heightAt is required)");
        return;
    }

    heightmapTerrainDestroyData(ht);

    ht->source         = *source;
    ht->tileSizeMeters = tileSizeMeters > 0.0f ? tileSizeMeters : HEIGHTMAP_TILE_SIZE_M;
    ht->windowSize     = windowSize < 3 ? 3 : windowSize;
    if ((ht->windowSize & 1u) == 0u) ht->windowSize++;
    ht->lruCounter     = 0;
    ht->tilesReady     = 0;
    ht->inFlight       = 0;
    ht->seamFailures   = 0;
    ht->generatedTiles = 0;
    ht->initialized    = true;
    ht->registered     = true;
    ht->lastLoggedCx   = INT32_MIN;
    ht->lastLoggedCz   = INT32_MIN;
}

void heightmapTerrainDestroyData(HeightmapTerrain* ht) {
    if (!ht) return;

    utils::threadLock(&heightmapLock);
    ht->registered = false;
    // Drop pending jobs for this instance.
    for (i32 i = static_cast<i32>(static_cast<i32>(buildQueue.size())) - 1; i >= 0; --i) {
        if (buildQueue[i].ht == ht) {
            buildQueue[i] = std::move(buildQueue.back());
            buildQueue.pop_back();
        }
    }
    utils::threadUnlock(&heightmapLock);

    // Wait for in-flight generation to finish (the builder holds no lock
    // while generating, so we poll).
    for (;;) {
        utils::threadLock(&heightmapLock);
        bool busy = ht->inFlight > 0;
        utils::threadUnlock(&heightmapLock);
        if (!busy) break;
        utils::gotoSleepNS(1000000); // 1 ms
    }

    for (i32 i = static_cast<i32>(static_cast<i32>(ht->tiles.size())) - 1; i >= 0; --i) {
        heightmapTerrainFreeTile(ht->tiles[i]);
    }
    ht->tiles.clear();
    ht->tilesReady   = 0;
    ht->lruCounter   = 0;
    ht->inFlight     = 0;
    ht->initialized  = false;
}

// ── Coordinates ────────────────────────────────────────────────────────────

i32 heightmapWorldToTileCoord(const HeightmapTerrain* ht, float worldCoord) {
    return static_cast<i32>(floorf(worldCoord / ht->tileSizeMeters));
}

void heightmapTileToWorldOrigin(const HeightmapTerrain* ht,
                                i32 tileX,
                                i32 tileZ,
                                float* outOriginX,
                                float* outOriginZ) {
    if (outOriginX) *outOriginX = static_cast<float>(tileX) * ht->tileSizeMeters;
    if (outOriginZ) *outOriginZ = static_cast<float>(tileZ) * ht->tileSizeMeters;
}

// ── Sampling ───────────────────────────────────────────────────────────────

float heightmapTerrainSample(const HeightmapTerrain* ht, float wx, float wz) {
    if (!ht || !ht->initialized) return 0.0f;

    utils::threadLock(&heightmapLock);
    float y    = 0.0f;
    bool  have = false;
    if (ht->registered) {
        HeightmapTile* tile =
            heightmapTerrainFindTile(const_cast<HeightmapTerrain*>(ht),
                                     heightmapWorldToTileCoord(ht, wx),
                                     heightmapWorldToTileCoord(ht, wz));
        if (tile && tile->state == HEIGHTMAP_TILE_READY && !tile->heights.empty()) {
            // Grid coords in [0, TEX-1]; the grid spans the full tile edge.
            float gx = (wx - tile->originX) / tile->sizeMeters * static_cast<float>(HEIGHTMAP_TEX - 1);
            float gz = (wz - tile->originZ) / tile->sizeMeters * static_cast<float>(HEIGHTMAP_TEX - 1);
            y        = heightmapGridBilinear(tile->heights.data(), HEIGHTMAP_TEX, gx, gz);
            have     = true;
        }
    }
    utils::threadUnlock(&heightmapLock);

    return have ? y : ht->source.heightAt(ht->source.userData, wx, wz);
}

// ── Self-test (ENGINE_HEIGHTMAP_TEST) ────────────────────────────────────

// Deterministic analytic source: a pure function of world coords.
static float heightmapSelfTestHeightAt(void* ud, float wx, float wz) {
    (void)ud;
    return 1200.0f * sinf(wx * 0.00021f) * cosf(wz * 0.00017f)
         + 400.0f * sinf(wx * 0.00043f + 1.7f) * sinf(wz * 0.00037f)
         + 120.0f * cosf((wx + wz) * 0.0009f);
}

// Poll snapshotTiles until at least expect tiles are READY or the timeout
// expires. Views from the successful call are left in outViews.
static bool heightmapSelfTestWaitReady(HeightmapTerrain* ht, u32 expect,
                                       double timeoutSec,
                                       HeightmapTileView* outViews,
                                       u32 cap) {
    double deadline = utils::nanos() + timeoutSec * 1e9; // nanos() is ns
    for (;;) {
        if (heightmapTerrainSnapshotTiles(ht, outViews, cap) >= expect) return true;
        if (utils::nanos() > deadline) return false;
        utils::gotoSleepNS(1000000); // 1 ms
    }
}

static const HeightmapTileView* heightmapSelfTestFindView(const HeightmapTileView* views,
                                                          u32 n,
                                                          i32 tileX,
                                                          i32 tileZ) {
    for (u32 i = 0; i < n; ++i) {
        if (views[i].tileX == tileX && views[i].tileZ == tileZ) return &views[i];
    }
    return nullptr;
}

bool heightmapTerrainSelfTest(void) {
    static const char* checkNames[] = {
        "snapshotCount",
        "sampleBilinear",
        "sampleSourceAbsent",
        "borders",
        "regenBitIdentical",
    };
    static const u32 nChecks = 5;
    bool ok[nChecks] = {true, true, true, true, true};

    // 1022 m tiles => 1022/511 = 2 m grid step, exact in f32: tile borders
    // sample bit-identical world coords, so border columns and regenerated
    // grids must be bit-identical (not merely close).
    const u32   window   = 3;
    const float tileSize = 1022.0f;
    const float ax = 0.5f * tileSize;  // window centred on tile (0,0)
    const float az = 0.5f * tileSize;

    HeightmapTerrain ht = {};
    HeightmapSource  src = {heightmapSelfTestHeightAt, nullptr};
    heightmapTerrainInit(&ht, &src, tileSize, window);

    heightmapTerrainUpdateWindow(&ht, ax, az);

    static HeightmapTileView views[25];
    bool allReady = heightmapSelfTestWaitReady(&ht, window * window, 15.0, views, 25);
    u32  n        = allReady ? heightmapTerrainSnapshotTiles(&ht, views, 25) : 0;

    // 1. snapshot count == window^2
    if (!(ok[0] = ((n == window * window)))) {
        utils::warn("heightmapTerrain self-test: snapshotCount: %u READY after wait (expected %u)", n, window * window);
    }

    // 2. heightmapTerrainSample at an off-lattice point == bilinear of the
    //    resident grid copy (same arithmetic the sample fast path runs).
    if (ok[0]) {
        const HeightmapTileView* v = heightmapSelfTestFindView(views, n, 0, 0);
        float wx                   = 373.7f;  // inside tile (0,0), not on a grid node
        float wz                   = 913.4f;
        if (!v) {
            ok[1] = false;
            utils::warn("heightmapTerrain self-test: sampleBilinear: view for tile(0,0) missing");
        } else {
            float gx = (wx - v->originX) / v->sizeMeters * static_cast<float>(HEIGHTMAP_TEX - 1);
            float gz = (wz - v->originZ) / v->sizeMeters * static_cast<float>(HEIGHTMAP_TEX - 1);
            float expected = heightmapGridBilinear(v->heights, HEIGHTMAP_TEX, gx, gz);
            float got      = heightmapTerrainSample(&ht, wx, wz);
            if (!(ok[1] = ((fabsf(got - expected) < 1e-3f)))) {
                utils::warn("heightmapTerrain self-test: sampleBilinear: got %.6f expected %.6f", got, expected);
            }
        }
    } else {
        ok[1] = false;
    }

    // 3. sample of a point on an ABSENT tile falls back to the source exactly.
    {
        float wx = 999.0f * tileSize + 123.4f;  // tile (999,-500): far outside the window
        float wz = -500.0f * tileSize - 77.2f;
        float expected = heightmapSelfTestHeightAt(nullptr, wx, wz);
        float got      = heightmapTerrainSample(&ht, wx, wz);
        if (!(ok[2] = ((got == expected)))) {
            utils::warn("heightmapTerrain self-test: sampleSourceAbsent: got %.6f expected %.6f", got, expected);
        }
    }

    // 4. adjacent READY tiles share bit-identical border columns/rows.
    if (ok[0]) {
        const HeightmapTileView* v00 = heightmapSelfTestFindView(views, n, 0, 0);
        const HeightmapTileView* v10 = heightmapSelfTestFindView(views, n, 1, 0);  // east neighbour
        const HeightmapTileView* v01 = heightmapSelfTestFindView(views, n, 0, 1);  // south neighbour
        const u32                tex = HEIGHTMAP_TEX;
        bool borderOk               = (v00 && v10 && v01);
        if (borderOk) {
            for (u32 z = 0; z < tex && borderOk; ++z) {
                borderOk = (v00->heights[static_cast<size_t>(z) * tex + (tex - 1)] == v10->heights[static_cast<size_t>(z) * tex]);
            }
            for (u32 x = 0; x < tex && borderOk; ++x) {
                borderOk = (v00->heights[static_cast<size_t>(tex - 1) * tex + x] == v01->heights[x]);
            }
        }
        if (!(ok[3] = (borderOk))) {
            utils::warn("heightmapTerrain self-test: borders: tile(0,0) border mismatch vs tile(1,0)/tile(0,1) or views missing");
        }
    } else {
        ok[3] = false;
    }

    // 5. eviction + regeneration is bit-identical.
    {
        std::vector<float> copyA(static_cast<size_t>(HEIGHTMAP_TEX) * HEIGHTMAP_TEX);
        std::vector<float> copyB(copyA.size());
        if (!heightmapTerrainCopyTile(&ht, 0, 0, copyA.data())) {
            ok[4] = false;
            utils::warn("heightmapTerrain self-test: regenBitIdentical: copy of tile(0,0) failed");
        } else {
            float ex = 100.0f * tileSize + 0.5f * tileSize;  // re-anchor far away: evicts the window
            float ez = ex;
            heightmapTerrainUpdateWindow(&ht, ex, ez);
            heightmapTerrainUpdateWindow(&ht, ax, az);  // back: recreates + queues generation
            static HeightmapTileView views2[25];
            bool ready2 = heightmapSelfTestWaitReady(&ht, window * window, 15.0, views2, 25);
            bool  copied2 = heightmapTerrainCopyTile(&ht, 0, 0, copyB.data());
            if (!(ok[4] = ((ready2 && copied2 && copyA == copyB)))) {
                utils::warn("heightmapTerrain self-test: regenBitIdentical: ready=%d copied=%d", (int)ready2, (int)copied2);
            }
        }
    }

    heightmapTerrainDestroyData(&ht);

    bool pass = true;
    char failed[256] = "";
    for (u32 i = 0; i < nChecks; ++i) {
        if (ok[i]) continue;
        pass = false;
        if (failed[0]) strcat(failed, ",");
        strcat(failed, checkNames[i]);
    }
    if (pass) {
        utils::info("heightmapTerrain self-test: PASS (snapshotCount, sampleBilinear, sampleSourceAbsent, borders, regenBitIdentical)");
    } else {
        utils::error("heightmapTerrain self-test: FAIL (%s)", failed);
    }
    return pass;
}

// ── System ─────────────────────────────────────────────────────────────────
// Tracks the streaming window around the camera. Does nothing while no world
// has an active heightmap terrain (e.g. regular-mesh worlds).

void HeightmapTerrainSystem::added() {}
void HeightmapTerrainSystem::removed() {}
void HeightmapTerrainSystem::preUpdate() {}
void HeightmapTerrainSystem::postUpdate() {}

void HeightmapTerrainSystem::update() {
    HeightmapTerrain* ht = heightmapTerrainGetActive();
    if (!ht || !ht->initialized) return;

    float pos[3] = {0.0f, 0.0f, 0.0f};
    float fwd[3] = {0.0f, 0.0f, 0.0f};
    renderer::rendererCameraGet(pos, fwd);

    heightmapTerrainUpdateWindow(ht, pos[0], pos[2]);
}

HeightmapTerrainSystem heightmapTerrainSystem;

HeightmapTerrainSystem::HeightmapTerrainSystem() : System("heightmapTerrain") {}
}  // namespace engine
