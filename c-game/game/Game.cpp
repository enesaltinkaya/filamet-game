#include "Game.h"
#include "Utils.h"
#include "Engine.h"
#include "ecs/system/flyingCamera/FlyingCamera.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "ecs/system/player/Player.h"
#include "ecs/system/physics/PhysicsSystem.h"
#include "ecs/system/heightmap/HeightmapTerrainRender.h"
#include "gui/GuiManager.h"
#include "gltf/Gltf.h"
#include "renderer/PropsRender.h"
#include "renderer/Renderer.h"
#include "renderer/Window.h"
#include "gameState/GameState.h"
#include "loadingAzgaar/LoadingAzgaar.h"
#include "mainMenu/MainMenuGui.h"
#include "cameraGui/CameraGui.h"
#include "playerActionsGui/PlayerActionsGui.h"
#include "azgaar/AzgaarPropMesh.h"
#include "azgaar/AzgaarProps.h"
#include "azgaar/AzgaarSettlements.h"

#include <SDL.h>

#include <cmath>
#include <cstring>
#include <algorithm>
#include <unordered_map>

namespace game {

    static bool worldLoaded     = false;
    static bool s_acceptanceRan = false;

    // One HeightmapTerrain per world (file-static; heightmapTerrainInit is
    // idempotent and frees any previously resident tiles). Backed by
    // loadingAzgaar's source, which outlives it.
    static engine::HeightmapTerrain s_terrain = {};

    // Props (phase 7): the CPU scatter (azgaarPropsUpdate) publishes
    // per-tile instance arrays in the background; the render pass consumes
    // them through propsRenderSetTile. This bridge forwards each
    // newly-published scatter exactly once, keyed on the tile's
    // (readyStamp, buildSeq): a camera-stale re-scatter keeps the
    // readyStamp but bumps buildSeq, so it re-uploads exactly once. GPU
    // eviction is the render pass' own job (its window snapshot).
    struct PropsPushRecord {
        i32 tileX = 0, tileZ = 0;
        u64 readyStamp = 0;
        u32 buildSeq   = 0;
    };

    static std::vector<PropsPushRecord> s_propsPushed;

    // Register the props render state once per world load (merged mesh +
    // per-(species, variant) table + wind + enable), after azgaarPropsInit
    // has built the mesh and loaded the grass card textures.
    static void propsRegisterRender(void) {
        const AzgaarPropMesh* mesh = azgaarPropMeshGet();
        if (!mesh || mesh->ranges.empty()) {
            engine::propsRenderSetEnabled(false);
            return;
        }
        // The merged array uploads verbatim (AzgaarPropVertex is 13 floats /
        // 52 B, the pass' expected layout).
        engine::propsRenderSetMesh(reinterpret_cast<const float*>(mesh->vertices.data()),
                                   (u32)mesh->vertices.size(),
                                   mesh->indices.data(),
                                   (u32)mesh->indices.size());

        std::vector<engine::PropsRenderMeshVariant> variants;
        variants.reserve(mesh->ranges.size());
        for (const auto& r : mesh->ranges) {
            engine::PropsRenderMeshVariant v = {};
            v.species                        = r.species;
            v.variant                        = r.variant;
            v.indexOffset                    = r.indexOffset;
            v.indexCount                     = r.indexCount;
            for (u32 c = 0; c < 3; c++) {
                v.boundsMin[c] = r.aabbMin[c];
                v.boundsMax[c] = r.aabbMax[c];
            }
            v.swayFactor = azgaarPropsSpeciesSway(r.species);
            v.flags      = azgaarPropsSpeciesRenderFlags(r.species);
            v.texturePath =
                (r.species == AZGAAR_PROP_GRASS_TUFT && r.variant < azgaarPropsGrassVariantCount())
                    ? azgaarPropsGrassVariant(r.variant)->path
                    : nullptr;
            variants.push_back(v);
        }
        engine::propsRenderSetVariants(variants.data(), (u32)variants.size());

        const AzgaarPropsWind* wind = azgaarPropsWind();
        engine::propsRenderSetWind(wind->dirX, wind->dirZ, wind->speed, wind->strength);
        const char* noProps = getenv("ENGINE_NO_PROPS");
        engine::propsRenderSetEnabled(!noProps || noProps[0] != '1');
        utils::info("game: props render registered (%zu variants, mesh %zu verts / %zu idx)",
                    variants.size(),
                    mesh->vertices.size(),
                    mesh->indices.size());
    }

    // Per-frame props bridge: advance the scatter worker, then forward any
    // newly-published tile scatters to the render pass.
    //
    // Phase-7 acceptance instrumentation wraps it: the game-side pass cost
    // (window tracking + publish forwarding) rolls over ~2 s, a periodic
    // cost/memory line prints under ENGINE_PROPS_PERF=1 (dolly runs watch
    // this one), and a one-shot acceptance log lands at frame 1120 — the
    // terrain pass' phase-5 convention (120-frame warmup, then averages).
    static constexpr u32 kPropsPerfFrames     = 120;
    static constexpr double kPropsCpuBudgetMs = 1.5;    // phase-5 per-pass budget
    static constexpr double kPropsGpuBudgetMB = 150.0;  // phase-5 resident-window
                                                        // budget (terrain + props)
    static u32 s_propsFrame                   = 0;
    static double s_propsMs[kPropsPerfFrames] = {};

    static void propsBridgeUpdateWork(void) {
        azgaarPropsUpdate();

        engine::HeightmapTerrain* ht = engine::heightmapTerrainGetActive();
        if (!ht || !ht->initialized) return;

        const u32 cap = ht->windowSize * ht->windowSize;
        std::vector<engine::HeightmapTileView> views(cap);
        const u32 n = engine::heightmapTerrainSnapshotTiles(ht, views.data(), cap);

        // Forget records for tiles that left the window (the render pass
        // evicts the GPU state itself; this keeps the record small).
        for (auto it = s_propsPushed.begin(); it != s_propsPushed.end();) {
            bool resident = false;
            for (u32 i = 0; i < n; i++) {
                if (views[i].tileX == it->tileX && views[i].tileZ == it->tileZ) {
                    resident = true;
                    break;
                }
            }
            it = resident ? std::next(it) : s_propsPushed.erase(it);
        }

        for (const auto& v : views) {
            const AzgaarPropsTile* t = azgaarPropsGetTile(v.tileX, v.tileZ, v.readyStamp);
            if (!t || t->instances.empty()) continue;

            PropsPushRecord* rec = nullptr;
            for (auto& r : s_propsPushed) {
                if (r.tileX == t->tileX && r.tileZ == t->tileZ) {
                    rec = &r;
                    break;
                }
            }
            if (rec && rec->readyStamp == t->readyStamp && rec->buildSeq == t->buildSeq)
                continue;  // already forwarded

            // The 10-float instance prefix is identical in layout.
            std::vector<engine::PropsRenderInstance> insts(t->instances.size());
            for (u32 i = 0; i < t->instances.size(); i++)
                memcpy(&insts[i], &t->instances[i], sizeof(engine::PropsRenderInstance));

            std::vector<engine::PropsRenderRange> ranges(t->ranges.size());
            for (u32 i = 0; i < t->ranges.size(); i++) {
                ranges[i].species = t->ranges[i].species;
                ranges[i].variant = t->ranges[i].variant;
                ranges[i].start   = t->ranges[i].start;
                ranges[i].count   = t->ranges[i].count;
                for (u32 c = 0; c < 3; c++) {
                    ranges[i].aabbMin[c] = t->ranges[i].aabbMin[c];
                    ranges[i].aabbMax[c] = t->ranges[i].aabbMax[c];
                }
            }

            engine::propsRenderSetTile(t->tileX,
                                       t->tileZ,
                                       t->readyStamp,
                                       insts.data(),
                                       (u32)insts.size(),
                                       ranges.data(),
                                       (u32)ranges.size());

            if (rec) {
                rec->readyStamp = t->readyStamp;
                rec->buildSeq   = t->buildSeq;
            } else {
                s_propsPushed.push_back({t->tileX, t->tileZ, t->readyStamp, t->buildSeq});
            }
        }
    }

    static void propsBridgeUpdate(void) {
        const double t0 = utils::elapsedBegin();
        propsBridgeUpdateWork();
        s_propsMs[s_propsFrame % kPropsPerfFrames] = utils::elapsedEnd(t0);
        s_propsFrame++;

        const AzgaarPropsStats ps         = azgaarPropsStats();
        const engine::PropsRenderStats rs = engine::propsRenderStats();
        const u32 n  = s_propsFrame < kPropsPerfFrames ? s_propsFrame : kPropsPerfFrames;
        double sumMs = 0.0;
        for (u32 i = 0; i < n; i++) sumMs += s_propsMs[i];
        const double gameAvgMs = n ? sumMs / n : 0.0;

        static const bool perfLine = getenv("ENGINE_PROPS_PERF") != nullptr;
        if (perfLine && s_propsFrame % kPropsPerfFrames == 0) {
            utils::info(
                "props perf f=%u: game %.3f ms + pass %.3f ms (apply %.3f) avg/%u; "
                "%u/%u tiles built, %u inst, %u draws, %u queued, "
                "claims %u (rescatter %u, stamp %u), evictions %u; "
                "cpu %.1f MB, gpu %.1f MB, staging %.1f MB, worker %.1f ms/tile",
                s_propsFrame,
                gameAvgMs,
                rs.renderAvgMs,
                rs.applyAvgMs,
                n,
                ps.built,
                ps.resident,
                ps.instances,
                rs.gpuDraws,
                ps.queueDepth,
                ps.claims,
                ps.rescatters,
                ps.stampRebuilds,
                ps.evictions,
                (double)ps.cpuBytes / (1024.0 * 1024.0),
                (double)rs.gpuBytes / (1024.0 * 1024.0),
                (double)rs.cpuStagingBytes / (1024.0 * 1024.0),
                ps.workerAvgMs);
        }

        // One-shot phase-7 acceptance: steady-state cost + memory vs budget
        // (the verification screenshot runs land well before this frame —
        // perf runs use a later ENGINE_SCREENSHOT_FRAME).
        static constexpr u32 kAcceptFrame = 1120;  // 120 warmup + 1000
        static bool acceptanceLogged      = false;
        if (!acceptanceLogged && s_propsFrame == kAcceptFrame) {
            acceptanceLogged                             = true;
            const engine::HeightmapTerrainRenderStats ts = engine::heightmapTerrainRenderStats();
            const double cpuMB                           = (double)ps.cpuBytes / (1024.0 * 1024.0);
            const double gpuMB                           = (double)rs.gpuBytes / (1024.0 * 1024.0);
            const double stagMB = (double)rs.cpuStagingBytes / (1024.0 * 1024.0);
            const double totMB  = gpuMB + (double)ts.gpuBytes / (1024.0 * 1024.0);
            const double passMs = gameAvgMs + rs.renderAvgMs;
            utils::info(
                "props: steady cost f=%u: game %.3f ms + render pass %.3f ms "
                "(apply %.3f) = %.3f ms/frame (rolling %u-frame avg; worker "
                "%.1f ms/tile, %u builds) — %.2f ms budget",
                s_propsFrame,
                gameAvgMs,
                rs.renderAvgMs,
                rs.applyAvgMs,
                passMs,
                n,
                ps.workerAvgMs,
                ps.workerBuilds,
                kPropsCpuBudgetMs);
            utils::info(
                "props: memory: cpu %.1f MB (%u/%u tiles built, %u instances) + "
                "gpu %.1f MB (%u tex tiles, %u draws, %u inst) + %.1f MB staging; "
                "terrain gpu %.1f MB -> window %.1f MB of %.0f MB budget (props "
                "instance count vs the old engine's 5M cap: %.2f%%)",
                cpuMB,
                ps.built,
                ps.resident,
                ps.instances,
                gpuMB,
                rs.gpuTiles,
                rs.gpuDraws,
                rs.gpuInstances,
                stagMB,
                (double)ts.gpuBytes / (1024.0 * 1024.0),
                totMB,
                kPropsGpuBudgetMB,
                100.0 * (double)ps.instances / 5.0e6);
        }
    }

    static void propsRelease(void) {
        // Join the scatter worker (it holds the world pointer) and drop all
        // per-tile GPU state; the mesh/variant table stays until the next
        // world load re-sets it.
        azgaarPropsDestroy();
        engine::propsRenderClearAll();
        s_propsPushed.clear();
    }

    // One-shot acceptance log (phase 4): the Azgaar source surface must be a
    // pure function of (x,z) and the tiled grids must match it.
    //   1. src heightAt called twice at fixed points -> bit-identical
    //   2. heightmapTerrainSample (bilinear over 4 m texels) vs heightAt < ~1 m
    //   3. shared border of adjacent READY tiles -> bit-identical
    // Probe points sit near the default camera window anchor (500, 2700), so
    // their tiles are the first ones generated. Returns true once the check
    // has actually run (it is retried every frame until all probe tiles AND
    // an adjacent READY pair exist, so a partial window never fails it).
    static bool heightmapAcceptanceLog() {
        static const f32 probes[5][2] = {
            {400.0f, 2600.0f},
            {500.0f, 2700.0f},
            {560.0f, 2740.0f},
            {340.0f, 2820.0f},
            {620.0f, 2580.0f},
        };
        const engine::HeightmapSource* src = &s_terrain.source;
        bool allOk                         = true;

        // Thread-safe view of the READY tiles (never touches the tile table
        // while the builder thread publishes).
        engine::HeightmapTileView views[25];
        const u32 n = engine::heightmapTerrainSnapshotTiles(&s_terrain, views, 25);

        // Wait (silently) until every probe tile is READY and some adjacent
        // READY pair exists, so the one-shot only fires when all checks can run.
        for (const auto& p : probes) {
            const bool have = [&]() {
                const i32 tx = engine::heightmapWorldToTileCoord(&s_terrain, p[0]);
                const i32 tz = engine::heightmapWorldToTileCoord(&s_terrain, p[1]);
                for (u32 i = 0; i < n; ++i)
                    if (views[i].tileX == tx && views[i].tileZ == tz) return true;
                return false;
            }();
            if (!have) return false;
        }
        bool havePair = false;
        for (u32 i = 0; i < n && !havePair; ++i)
            for (u32 j = 0; j < n; ++j) {
                if (i == j) continue;
                if ((views[j].tileX == views[i].tileX + 1 && views[j].tileZ == views[i].tileZ) ||
                    (views[j].tileZ == views[i].tileZ + 1 && views[j].tileX == views[i].tileX))
                    havePair = true;
            }
        if (!havePair) return false;

        // 1 + 2: fixed-point determinism and sample-vs-source agreement.
        for (const auto& p : probes) {
            const f32 wx = p[0], wz = p[1];
            const f32 a        = src->heightAt(src->userData, wx, wz);
            const f32 b        = src->heightAt(src->userData, wx, wz);
            const bool twiceOk = (memcmp(&a, &b, sizeof(a)) == 0);
            const f32 sampled  = engine::heightmapTerrainSample(&s_terrain, wx, wz);
            const f32 diff     = fabsf(sampled - a);
            allOk              = allOk && twiceOk && (diff < 1.0f);
            utils::info(
                "heightmap acceptance: probe (%.0f, %.0f): heightAt %.6f twice %s, "
                "sample %.6f diff %.4f m",
                wx,
                wz,
                a,
                twiceOk ? "bit-identical" : "MISMATCH",
                sampled,
                diff);
        }

        // 3: adjacent READY tiles share a bit-identical border.
        bool borderChecked = false;
        for (u32 i = 0; i < n && !borderChecked; ++i) {
            for (u32 j = 0; j < n; ++j) {
                if (i == j) continue;
                const engine::HeightmapTileView& a = views[i];
                const engine::HeightmapTileView& b = views[j];
                bool ok = false, found = false;
                if (b.tileX == a.tileX + 1 && b.tileZ == a.tileZ) {
                    // shared column: a.x = TEX-1 vs b.x = 0 (grid is [z*dim + x])
                    found = true;
                    for (u32 z = 0; z < HEIGHTMAP_TEX && ok; ++z)
                        ok = (a.heights[z * HEIGHTMAP_TEX + (HEIGHTMAP_TEX - 1)] ==
                              b.heights[z * HEIGHTMAP_TEX]);
                } else if (b.tileZ == a.tileZ + 1 && b.tileX == a.tileX) {
                    // shared row: a.z = TEX-1 vs b.z = 0
                    found = true;
                    ok    = (memcmp(a.heights + (HEIGHTMAP_TEX - 1) * HEIGHTMAP_TEX,
                                    b.heights,
                                    sizeof(float) * HEIGHTMAP_TEX) == 0);
                }
                if (found) {
                    borderChecked = true;
                    allOk         = allOk && ok;
                    utils::info("heightmap acceptance: border tiles (%d,%d)/(%d,%d): %s",
                                a.tileX,
                                a.tileZ,
                                b.tileX,
                                b.tileZ,
                                ok ? "bit-identical" : "MISMATCH");
                }
            }
        }
        if (!borderChecked) allOk = false;  // unreachable: gated above

        utils::info("heightmap acceptance: %s", allOk ? "PASS" : "FAIL");
        return true;
    }

    // env-overridable blend thresholds (same pattern as the old engine's
    // ENGINE_AZGAAR_SNOW_LO/HI, ENGINE_AZGAAR_BEACH_H, CLIMATE_DISABLED).
    static f32 azgaarEnvFloat(const char* name, f32 fallback) {
        const char* v = getenv(name);
        if (!v || !*v) return fallback;
        return (f32)atof(v);
    }

    // Validation probe: ENGINE_AZGAAR_DUMP_TEXTURES=/path dumps the packed
    // per-world textures (exactly what the terrain pass uploads) as PPMs.
    static void dumpPpm(const char* path, const u8* px, u32 w, u32 h, bool rgb) {
        if (!px || !w || !h) return;
        FILE* f = fopen(path, "wb");
        if (!f) {
            utils::warn("game: dump texture failed: %s", path);
            return;
        }
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (u32 i = 0; i < (u32)w * (u32)h; i++) {
            if (rgb) {
                fputc(px[(size_t)i * 4u + 0u], f);
                fputc(px[(size_t)i * 4u + 1u], f);
                fputc(px[(size_t)i * 4u + 2u], f);
            } else {
                // single-channel dump: R,G,B,A written as 4 consecutive ppm
                // frames is overkill — write channel 0 (R) only; callers that
                // want another channel should dump a dedicated buffer
                fputc(px[(size_t)i * 4u], f);
            }
        }
        fclose(f);
        utils::info("game: dumped %s (%ux%u)", path, w, h);
    }

    static void dumpWorldLook(const engine::HeightmapTerrainLook& look) {
        const char* dir = getenv("ENGINE_AZGAAR_DUMP_TEXTURES");
        if (!dir || !dir[0]) return;
        char path[1024];
        snprintf(path, sizeof(path), "%s/biome_color.ppm", dir);
        dumpPpm(path, look.biomeColorPixels, look.biomeColorW, look.biomeColorH, true);
        snprintf(path, sizeof(path), "%s/climate_temp.ppm", dir);  // R = temp + 64
        dumpPpm(path, look.climatePixels, look.climateW, look.climateH, false);
    }

    // Build the terrain pass' per-world look from the loaded Azgaar world
    // (packed biome-colour + climate textures, map bounds, thresholds) and
    // register it with the active render backend.
    static void terrainRegisterWorldLook(const AzgaarWorld* world) {
        engine::HeightmapTerrainLook look = {};

        u32 w = 0, h = 0;
        std::vector<u8> biomePixels = azgaarWorldPackBiomeColorTexture(world, &w, &h);
        if (!biomePixels.empty()) {
            look.biomeColorPixels = biomePixels.data();
            look.biomeColorW      = w;
            look.biomeColorH      = h;
        }
        w                             = 0;
        h                             = 0;
        std::vector<u8> climatePixels = azgaarWorldPackClimateTexture(world, &w, &h);
        if (!climatePixels.empty()) {
            look.climatePixels = climatePixels.data();
            look.climateW      = w;
            look.climateH      = h;
        }
        look.climateEnabled = look.biomeColorPixels != nullptr && look.climatePixels != nullptr;

        // Map bounds in world metres (azgaarMapToWorld centres the map at
        // the world origin).
        const f32 halfW     = (f32)world->widthPx * 0.5f * (f32)world->metersPerPixel;
        const f32 halfH     = (f32)world->heightPx * 0.5f * (f32)world->metersPerPixel;
        look.mapMinX        = -halfW;
        look.mapMinZ        = -halfH;
        look.mapMaxX        = halfW;
        look.mapMaxZ        = halfH;
        look.maxLandHeightM = world->maxLandHeightM;

        look.snowLoC      = azgaarEnvFloat("ENGINE_AZGAAR_SNOW_LO", -1.0f);
        look.snowHiC      = azgaarEnvFloat("ENGINE_AZGAAR_SNOW_HI", 3.0f);
        look.beachHeightM = azgaarEnvFloat("ENGINE_AZGAAR_BEACH_H", 2.5f);
        if (getenv("ENGINE_AZGAAR_CLIMATE_DISABLED")) {
            look.snowLoC = look.snowHiC = look.beachHeightM = 0.0f;
            look.climateEnabled                             = false;
        }

        dumpWorldLook(look);
        engine::heightmapTerrainRenderRegisterLook(&look);

        // ENGINE_TERRAIN_DEBUG=<off|ramp|biome>: validation views (ramp =
        // periodic hue per 256 m of height, biome = raw biome-colour
        // texture through the map-space UV).
        const char* dbg = getenv("ENGINE_TERRAIN_DEBUG");
        u32 debugMode   = 0;
        if (dbg && utils::strequals(dbg, "ramp")) {
            debugMode = 1;
        } else if (dbg && utils::strequals(dbg, "biome")) {
            debugMode = 2;
        }
        // TEMP round-5 (misplaced-terrain diagnosis): shader probes.
        const char* probe = getenv("ENGINE_TERRAIN_PROBE");
        if (probe && utils::strequals(probe, "vs")) {
            debugMode = 10;
        } else if (probe && utils::strequals(probe, "nrm")) {
            debugMode = 11;
        } else if (probe && utils::strequals(probe, "clip")) {
            debugMode = 12;
        } else if (probe && utils::strequals(probe, "vt")) {
            debugMode = 13;
        } else if (probe && utils::strequals(probe, "wpos")) {
            debugMode = 17;
        } else if (probe && utils::strequals(probe, "vspace")) {
            debugMode = 18;
        }
        engine::heightmapTerrainRenderSetDebugView(debugMode);
    }

    // Highest land point of the world in world metres (the map-space height
    // grid scanned for the tallest texel, mapped back through
    // azgaarMapToWorld/azgaarHeightToMeters). Returns false when the grid is
    // unavailable. Used to frame validation shots on land.
    static bool worldHighestLandPoint(const AzgaarWorld* world, f32 out[3]) {
        if (!world || world->heightGrid.empty() || !world->heightGridWidth ||
            !world->heightGridHeight) {
            return false;
        }
        size_t best = 0;
        for (size_t i = 1; i < world->heightGrid.size(); i++) {
            if (world->heightGrid[i] > world->heightGrid[best]) best = i;
        }
        const u32 gx = (u32)(best % world->heightGridWidth);
        const u32 gy = (u32)(best / world->heightGridWidth);
        // Texel centres: grid texel gx covers map px (gx + 0.5) / xScale.
        const f32 xPx = ((f32)gx + 0.5f) * (f32)world->widthPx / (f32)world->heightGridWidth;
        const f32 yPx = ((f32)gy + 0.5f) * (f32)world->heightPx / (f32)world->heightGridHeight;
        azgaarMapToWorld(world, xPx, yPx, &out[0], &out[2]);
        out[1] = azgaarHeightToMeters(world, world->heightGrid[best]);
        return true;
    }

    // Densest prop-bearing land point (phase-7 props validation framing):
    // accumulates the scatter's expected vegetation density
    // (azgaarPropsBiomeDensity) over each 2048 m terrain tile's land texels,
    // then returns the densest tile's interior point (world metres) — trees
    // first (they are the sparse, biome-telling species; grass undergrowth
    // grows on every land texel of any prop-bearing biome and dominates a
    // total-density score). Falls back to total density when the map has no
    // tree species anywhere. Returns false when the grids or the props
    // tables are unavailable (the caller then falls back to the peak).
    static bool worldDensestPropsPoint(const AzgaarWorld* world,
                                       f32 out[3],
                                       f32* outTreeDensity,
                                       f32* outTotalDensity,
                                       i32* outTileX,
                                       i32* outTileZ) {
        if (!world || world->heightGrid.empty() || world->biomeGrid.empty() ||
            !world->climateGridWidth || !world->climateGridHeight ||
            world->heightGridWidth != world->climateGridWidth ||
            world->heightGridHeight != world->climateGridHeight) {
            return false;
        }
        const u32 gw      = world->climateGridWidth;
        const u32 gh      = world->climateGridHeight;
        const f32 seaY    = azgaarSeaLevelMeters(world);
        const u64 kNoTile = ~(u64)0;

        // Pass 1: per-tile density sums (expected instances = density x area).
        // texTile caches each land texel's tile key so pass 2 stays O(tile).
        struct TileAcc {
            f64 sumTree  = 0.0;
            f64 sumTotal = 0.0;
        };

        std::unordered_map<u64, TileAcc> tiles;
        tiles.reserve(256);
        std::vector<u64> texTile((size_t)gw * gh, kNoTile);
        for (u32 gy = 0; gy < gh; gy++) {
            for (u32 gx = 0; gx < gw; gx++) {
                const size_t gi = (size_t)gy * gw + gx;
                if (azgaarHeightToMeters(world, world->heightGrid[gi]) < seaY + 0.5f)
                    continue;  // water
                const u32 biome  = world->biomeGrid[gi];
                const f32 dTree  = azgaarPropsBiomeDensity(world, biome, true);
                const f32 dTotal = azgaarPropsBiomeDensity(world, biome, false);
                if (dTotal <= 0.0f) continue;
                f32 wx, wz;
                azgaarMapToWorld(world,
                                 ((f32)gx + 0.5f) * (f32)world->widthPx / (f32)gw,
                                 ((f32)gy + 0.5f) * (f32)world->heightPx / (f32)gh,
                                 &wx,
                                 &wz);
                const i32 tx  = (i32)floorf(wx / HEIGHTMAP_TILE_SIZE_M);
                const i32 tz  = (i32)floorf(wz / HEIGHTMAP_TILE_SIZE_M);
                const u64 key = ((u64)(u32)tx << 32) | (u32)tz;
                texTile[gi]   = key;
                TileAcc& acc  = tiles[key];
                acc.sumTree += dTree;
                acc.sumTotal += dTotal;
            }
        }
        if (tiles.empty()) return false;

        const bool treeScored = std::any_of(tiles.begin(), tiles.end(), [](const auto& kv) {
            return kv.second.sumTree > 0.0;
        });
        u64 bestKey           = 0;
        f64 bestScore         = -1.0;
        for (const auto& kv : tiles) {
            f64 score = treeScored ? kv.second.sumTree : kv.second.sumTotal;
            if (score > bestScore) {
                bestScore = score;
                bestKey   = kv.first;
            }
        }
        const i32 btx = (i32)(u32)(bestKey >> 32);
        const i32 btz = (i32)(u32)bestKey;

        // Pass 2: interior texel of that tile with the best 5x5 mean density
        // (same scorer), so the camera does not land on a biome-border sliver.
        f64 bestSum   = -1.0;
        size_t bestGi = (size_t)-1;
        for (u32 gy = 2; gy + 2 < gh; gy++) {
            for (u32 gx = 2; gx + 2 < gw; gx++) {
                const size_t ci = (size_t)gy * gw + gx;
                if (texTile[ci] != bestKey) continue;
                f64 sum = 0.0;
                u32 cnt = 0;
                for (i32 oy = -2; oy <= 2; oy++) {
                    for (i32 ox = -2; ox <= 2; ox++) {
                        const size_t gi = (size_t)((i32)gy + oy) * (i32)gw + ((i32)gx + ox);
                        if (texTile[gi] != bestKey) continue;
                        const u32 biome = world->biomeGrid[gi];
                        sum += treeScored ? azgaarPropsBiomeDensity(world, biome, true)
                                          : azgaarPropsBiomeDensity(world, biome, false);
                        cnt++;
                    }
                }
                if (cnt < 12) continue;  // border sliver: not an interior point
                if (sum > bestSum) {
                    bestSum = sum;
                    bestGi  = ci;
                }
            }
        }
        if (bestGi == (size_t)-1) return false;

        const u32 bgx = (u32)(bestGi % gw);
        const u32 bgy = (u32)(bestGi / gw);
        azgaarMapToWorld(world,
                         ((f32)bgx + 0.5f) * (f32)world->widthPx / (f32)gw,
                         ((f32)bgy + 0.5f) * (f32)world->heightPx / (f32)gh,
                         &out[0],
                         &out[2]);
        out[1]          = azgaarHeightToMeters(world, world->heightGrid[bestGi]);
        const u32 biome = world->biomeGrid[bestGi];
        if (outTreeDensity) *outTreeDensity = azgaarPropsBiomeDensity(world, biome, true);
        if (outTotalDensity) *outTotalDensity = azgaarPropsBiomeDensity(world, biome, false);
        if (outTileX) *outTileX = btx;
        if (outTileZ) *outTileZ = btz;
        return true;
    }

    // Automated validation hook: ENGINE_CAMERA_DOLLY="vx,vy,vz" pans the camera
    // at that velocity (m/s) while the world is up. Combined with
    // ENGINE_SCREENSHOT_FRAME it screenshots a camera that has actually moved,
    // so a headless run exercises the terrain pass' follow/evict/re-upload path.
    static void updateCameraDolly() {
        static f32 vel[3]  = {};
        static bool parsed = false;
        if (!parsed) {
            parsed        = true;
            const char* v = getenv("ENGINE_CAMERA_DOLLY");
            if (v && v[0]) {
                char buf[128];
                snprintf(buf, sizeof(buf), "%s", v);
                for (char* c = buf; *c; c++) {
                    if (*c == ',') *c = ' ';
                }
                sscanf(buf, "%f %f %f", &vel[0], &vel[1], &vel[2]);
                utils::info("game: camera dolly (%.1f, %.1f, %.1f) m/s", vel[0], vel[1], vel[2]);
            }
        }
        if (vel[0] == 0.0f && vel[1] == 0.0f && vel[2] == 0.0f) return;
        if (engine::flyingCameraFlying()) return;  // the player owns the camera

        f32 pos[3], fwd[3];
        engine::renderer::rendererCameraGet(pos, fwd);
        const f32 step   = (f32)utils::timer.dt;
        const f32 eye[3] = {pos[0] + vel[0] * step, pos[1] + vel[1] * step, pos[2] + vel[2] * step};
        const f32 target[3] = {eye[0] + fwd[0], eye[1] + fwd[1], eye[2] + fwd[2]};
        const f32 up[3]     = {0.0f, 1.0f, 0.0f};
        engine::renderer::rendererCameraLookAt(eye, target, up);
    }

    GameSystem::GameSystem() : System("Game") {}

    void GameSystem::added() {
        // World (terrain etc.) is loaded on ENTER WORLD, not here — the menu
        // boots fast over the clear background.
        // prove the pak system works: read a file shipped in pak_0
        utils::String version = utils::dataManagerRead("version.txt");
        utils::stringTrim(&version);
        utils::info("game: added — pak version: %s", version.data);
        utils::stringDestroy(&version);

        // GUI: bring up the main menu (ImGui via the active backend)
        gameStateSet(STATE_MAIN_MENU);
        engine::gui::guiInit();
        engine::gui::guiAdd(&mainMenuGui);
    }

    void GameSystem::loadWorld() {
        if (!worldLoaded) {
            engine::gltf::gltfInit();

            if (!loadingAzgaarLoad()) return;

            // CPU self-test of the heightmap streaming core (ENGINE_HEIGHTMAP_TEST=1)
            if (getenv("ENGINE_HEIGHTMAP_TEST")) {
                engine::heightmapTerrainSelfTest();
            }

            worldLoaded = true;
            utils::info("game: world loaded");
        }

        // Streaming heightmap terrain over the Azgaar world. Re-init on
        // re-entry: the menu-return teardown destroyed this instance's data,
        // and heightmapTerrainInit frees any previously resident tiles.
        // The heightmapTerrainSystem is added by the menu (deferred) and
        // picks the active instance up on its next update.
        const AzgaarHeightmapSource* src = loadingAzgaarGetHeightmapSource();
        if (src) {
            engine::heightmapTerrainInit(&s_terrain,
                                         &src->vtable,
                                         HEIGHTMAP_TILE_SIZE_M,
                                         HEIGHTMAP_WINDOW_SIZE);
            engine::heightmapTerrainSetActive(&s_terrain);

            // Terrain render pass: per-world look (biome/climate textures +
            // bounds + thresholds), registered before any tile renders.
            terrainRegisterWorldLook(loadingAzgaarGetWorld());

            // Props (phase 7): CPU scatter + render pass. Re-init on re-entry
            // (azgaarPropsInit is idempotent, like the terrain above).
            azgaarPropsInit(loadingAzgaarGetWorld());
            propsRegisterRender();
        }

        // Player character (eve): a zstd-compressed glb exported by
        // scripts/export-models.sh. Stood at the densest prop-bearing land
        // point (the "props"/"propsground" camera's vantage — the
        // old-engine reference view was a player view over that same dry
        // land; the old engine's hardcoded spawn xz now maps to open seabed
        // in this world, so it is only the last-resort fallback).
        f32 spawnPt[3] = {0.0f, 0.0f, 0.0f};
        if (!worldDensestPropsPoint(loadingAzgaarGetWorld(),
                                   spawnPt,
                                   nullptr,
                                   nullptr,
                                   nullptr,
                                   nullptr) &&
            !worldHighestLandPoint(loadingAzgaarGetWorld(), spawnPt)) {
            spawnPt[0] = -881.88f;  // old engine's spawn xz
            spawnPt[1] = 511.55f;
            spawnPt[2] = 1691.46f;
        }
        // gltfInit is idempotent — this re-creates the loader after the
        // menu-return gltfDestroy on re-entry.
        if (engine::gltf::gltfInit() && engine::gltf::gltfLoad("models/eve.zstd")) {
            engine::gltf::gltfPlaceAt(spawnPt[0], spawnPt[1], spawnPt[2]);
        }
        // Animation source (the old engine's models/animations.dat): a second
        // glb carrying eve's skeleton + all clips (no textures). Not added to
        // the scene — gltfUpdate plays the selected clip on it and syncs the
        // joint transforms onto the visible model. The player system drives
        // clip selection from here on (Player.cpp state machine).
        if (!getenv("ENGINE_NO_ANIM") &&
            engine::gltf::gltfLoadAnimations("models/animations.zstd")) {
            engine::gltf::gltfPlayAnimation("eve_idle1", 1.0f, true);
        }
        utils::info("game: player spawn at (%.0f, %.0f, %.0f)", spawnPt[0], spawnPt[1], spawnPt[2]);

        // The playerSystem (added deferred by the menu) ground-snaps to the
        // heightmap surface at this point and takes over the model from here.
        engine::playerSetSpawn(spawnPt[0], spawnPt[1], spawnPt[2]);

        // Camera framing: ENGINE_CAMERA selects a validation vantage; the
        // default frames the world's highest land point (see the else branch).
        f32 center[3] = {0.0f, 0.0f, 0.0f};

        const char* cameraMode = getenv("ENGINE_CAMERA");
        if (cameraMode && utils::strequals(cameraMode, "topdown")) {
            // top-down map view (validation shots): up = -z keeps the frame stable
            f32 eye[3] = {center[0], center[1] + 9000.0f, center[2] + 0.01f};
            f32 up[3]  = {0.0f, 0.0f, -1.0f};
            engine::renderer::rendererCameraLookAt(eye, center, up);
        } else if (cameraMode && utils::strequals(cameraMode, "close")) {
            f32 eye[3]    = {center[0] + 180.0f, center[1] + 60.0f, center[2] + 180.0f};
            f32 lookAt[3] = {center[0], center[1], center[2] + 60.0f};
            f32 up[3]     = {0.0f, 1.0f, 0.0f};
            engine::renderer::rendererCameraLookAt(eye, lookAt, up);
        } else if (cameraMode && utils::strequals(cameraMode, "land")) {
            // Validation shots: frame the highest land point — the only place
            // where every altitude band of the look shows (beach, turf, cliff
            // rock, snow line). The map centre the default camera looks at can
            // well be open sea, which says nothing about the look.
            f32 peak[3] = {center[0], 0.0f, center[2]};
            if (!worldHighestLandPoint(loadingAzgaarGetWorld(), peak)) {
                peak[0] = center[0];
                peak[2] = center[2];
            }
            utils::info("game: camera framing highest land point (%.0f, %.0f, %.0f)",
                        peak[0],
                        peak[1],
                        peak[2]);
            f32 eye[3] = {peak[0] + 2600.0f, peak[1] + 900.0f, peak[2] + 2600.0f};
            f32 up[3]  = {0.0f, 1.0f, 0.0f};
            engine::renderer::rendererCameraLookAt(eye, peak, up);
        } else if (cameraMode && utils::strequals(cameraMode, "landtop")) {
            // Top-down over the same highest land point: with
            // ENGINE_TERRAIN_DEBUG=ramp this is the seam check — tile borders
            // are straight axis-aligned lines, so any height discontinuity
            // between neighbouring tiles shows up as a straight colour step.
            f32 peak[3] = {center[0], 0.0f, center[2]};
            if (!worldHighestLandPoint(loadingAzgaarGetWorld(), peak)) {
                peak[0] = center[0];
                peak[2] = center[2];
            }
            f32 eye[3] = {peak[0], peak[1] + 5000.0f, peak[2] + 0.01f};
            f32 up[3]  = {0.0f, 0.0f, -1.0f};
            engine::renderer::rendererCameraLookAt(eye, peak, up);
        } else if (cameraMode && utils::strequals(cameraMode, "props")) {
            // Phase-7 props validation: higher/closer oblique view over the
            // DENSEST prop-bearing land tile (tree density first — the old
            // highest-land framing sat on a rocky peak with zero tree
            // species: 63k instances, every one grass). The scatter-time
            // cull caps are XZ ground-plane distances (trees 840 m, grass
            // 440 m), so the camera must sit inside the tile it judges.
            f32 p[3]  = {center[0], 0.0f, center[2]};
            f32 treeD = 0.0f, totalD = 0.0f;
            i32 tileX = 0, tileZ = 0;
            if (!worldDensestPropsPoint(loadingAzgaarGetWorld(),
                                        p,
                                        &treeD,
                                        &totalD,
                                        &tileX,
                                        &tileZ) &&
                !worldHighestLandPoint(loadingAzgaarGetWorld(), p)) {
                p[0] = center[0];
                p[2] = center[2];
            }
            utils::info(
                "game: props camera over densest tile(%d,%d) point "
                "(%.0f, %.0f, %.0f): tree density %.5f, total %.5f instances/m^2",
                tileX,
                tileZ,
                p[0],
                p[1],
                p[2],
                treeD,
                totalD);
            f32 eye[3]    = {p[0] + 55.0f, p[1] + 70.0f, p[2] + 55.0f};
            f32 lookAt[3] = {p[0], p[1] + 5.0f, p[2]};
            f32 up[3]     = {0.0f, 1.0f, 0.0f};
            engine::renderer::rendererCameraLookAt(eye, lookAt, up);
        } else if (cameraMode && utils::strequals(cameraMode, "propsground")) {
            // Phase-7 visual acceptance companion to "props": the old-engine
            // reference (docs/azgaar-terrain/old-engine-reference.jpg) is a
            // ~7 m eye-height player view, so density/distribution are judged
            // vantage-to-vantage from eye height over the same densest-props
            // point. Absolute density still differs: the reference's dry-turf
            // savanna is a sparser biome than this woodland tile.
            f32 p[3]  = {center[0], 0.0f, center[2]};
            f32 treeD = 0.0f, totalD = 0.0f;
            i32 tileX = 0, tileZ = 0;
            if (!worldDensestPropsPoint(loadingAzgaarGetWorld(),
                                        p,
                                        &treeD,
                                        &totalD,
                                        &tileX,
                                        &tileZ) &&
                !worldHighestLandPoint(loadingAzgaarGetWorld(), p)) {
                p[0] = center[0];
                p[2] = center[2];
            }
            utils::info(
                "game: props ground camera at densest tile(%d,%d) point "
                "(%.0f, %.0f, %.0f): eye height 7 m, tree density %.5f, "
                "total %.5f instances/m^2",
                tileX,
                tileZ,
                p[0],
                p[1],
                p[2],
                treeD,
                totalD);
            f32 eye[3]    = {p[0] + 14.0f, p[1] + 7.0f, p[2] + 14.0f};
            f32 lookAt[3] = {p[0] + 114.0f, p[1] + 3.0f, p[2] + 114.0f};
            f32 up[3]     = {0.0f, 1.0f, 0.0f};
            engine::renderer::rendererCameraLookAt(eye, lookAt, up);
        } else if (cameraMode && utils::strequals(cameraMode, "character")) {
            // Portrait of the player character (eve at the old spawn point):
            // ~1 character-height diagonal back, eye slightly above chest,
            // looking at chest height — close enough for a texture/material
            // check, far enough that the whole silhouette is in frame.
            f32 bmin[3], bmax[3];
            if (engine::gltf::gltfBoundingBox(bmin, bmax)) {
                f32 cx       = (bmin[0] + bmax[0]) * 0.5f;
                f32 cz       = (bmin[2] + bmax[2]) * 0.5f;
                f32 h        = bmax[1] - bmin[1];
                f32 chest[3] = {cx, bmin[1] + h * 0.6f, cz};
                f32 eye[3]   = {chest[0] + h, chest[1] + h * 0.2f, chest[2] - h};
                f32 up[3]    = {0.0f, 1.0f, 0.0f};
                utils::info("game: character camera — bounds [%.2f %.2f %.2f]-[%.2f %.2f %.2f]",
                            bmin[0],
                            bmin[1],
                            bmin[2],
                            bmax[0],
                            bmax[1],
                            bmax[2]);
                engine::renderer::rendererCameraLookAt(eye, chest, up);
            } else {
                utils::warn("game: character camera — no gltf bounds, keeping default camera");
            }
        } else {
            f32 eye[3]    = {-100, 2, -100};
            f32 lookAt[3] = {-101, 1.5, -101};
            f32 up[3]     = {0.0f, 1.0f, 0.0f};
            engine::renderer::rendererCameraLookAt(eye, lookAt, up);
        }

        // sun (directional) + constant ambient, backend-agnostic.
        // Ambient is ~1/9 of the sun (clear-sky ratio): the earlier 30000
        // (27% of the sun) washed the NdotL contrast out of every slope and
        // the terrain read as un-shaded flat sheet — no shape-from-shading
        // cue for flight. Keep it a small fraction of the sun intensity.
        f32 sunDirection[3] = {-0.6f, -1.0f, -0.5f};
        f32 sunColor[3]     = {1.0f, 0.97f, 0.92f};
        engine::renderer::rendererSetSun(sunDirection, sunColor, 110000.0f);

        f32 ambient[3] = {0.32f, 0.35f, 0.38f};
        engine::renderer::rendererSetAmbient(ambient, 12000.0f);

        // Atmospheric distance haze (aerial perspective): near terrain stays
        // crisp, far terrain recedes into the sky color, which both gives the
        // frame depth layers that move against each other in flight AND hides
        // the streaming window's far edge / the 20 km far-plane cut behind a
        // soft fade instead of a hard line. The color must match the sky
        // clear color (RenderBackend.h kClearColor) or the horizon seams.
        // density 3.5e-4/m: 20% extinction at 640 m, 1/e at ~2.9 km, ~99.9%
        // at the 20 km far plane — near/mid terrain keeps its detail, the
        // far field reads as depth, and the clipped window edge / far-plane
        // cut never shows a hard line. ENGINE_FOG_DENSITY overrides for
        // tuning/validation.
        f32 fogColor[3] = {0.02f, 0.04f, 0.09f};
        f32 fogDensity  = 0.00035f;
        if (const char* fd = getenv("ENGINE_FOG_DENSITY")) fogDensity = (f32)atof(fd);
        engine::renderer::rendererSetFog(fogColor, fogDensity);
    }

    void GameSystem::preUpdate() {
        // In the world and not flying: ESC returns to the main menu
        if (gameStateCurrent() == STATE_PLAYING && !engine::flyingCameraFlying() &&
            engine::input.pressed == SDL_SCANCODE_ESCAPE) {
            utils::info("game: back to main menu");
            gameStateSet(STATE_MAIN_MENU);
            engine::ecsSystemRemoveDeferred(&engine::playerSystem);
            engine::ecsSystemRemoveDeferred(&engine::flyingCameraSystem);
            engine::ecsSystemRemoveDeferred(&engine::heightmapTerrainSystem);
            engine::ecsSystemRemoveDeferred(&engine::physicsSystem);
            // Drop the terrain's tile data + render look while the menu is
            // up (world and source stay retained; re-entering the world
            // re-inits the terrain and re-registers the look). The
            // settlement plateau grid is cleared only at world release — it
            // is valid as long as the world is.
            engine::heightmapTerrainRenderReleaseLook();
            engine::heightmapTerrainDestroyData(&s_terrain);
            engine::heightmapTerrainSetActive(nullptr);
            // Drop the props scatter + its GPU state while the menu is up
            // (the world stays retained; re-entering re-inits both).
            propsRelease();
            engine::gui::guiAdd(&mainMenuGui);
            engine::gui::guiRemove(&cameraGui);
            engine::gui::guiRemove(&playerActionsGui);
        }
    }

    void GameSystem::removed() {
        engine::systemRemove(&engine::playerSystem);
        engine::systemRemove(&engine::flyingCameraSystem);
        engine::systemRemove(&engine::heightmapTerrainSystem);
        engine::systemRemove(&engine::physicsSystem);
        if (worldLoaded) {
            // Terrain before world: heightAt dereferences the world, and the
            // plateau grid indexes into world->settlements.
            engine::heightmapTerrainRenderReleaseLook();
            engine::heightmapTerrainDestroyData(&s_terrain);
            engine::heightmapTerrainSetActive(nullptr);
            // Props before world: the scatter worker holds the world pointer
            // until azgaarPropsDestroy returns.
            propsRelease();
            azgaarSettlementsPlateauClear();
            engine::gltf::gltfDestroy();
            loadingAzgaarReleaseWorld();

            // the renderer owns the sun/ambient now; they stay set for the
            // next world load and affect nothing while no geometry is drawn
            worldLoaded = false;
        }
        utils::info("game: removed");
    }

    void GameSystem::update() {
        engine::gltf::gltfUpdate(utils::timer.dt);
        updateCameraDolly();

        // Props (phase 7): advance the scatter + forward fresh tiles.
        if (worldLoaded) propsBridgeUpdate();

        // One-shot acceptance probe, fired once its tiles are up.
        if (!s_acceptanceRan && worldLoaded && s_terrain.initialized && s_terrain.tilesReady > 0)
            s_acceptanceRan = heightmapAcceptanceLog();
    }

    GameSystem gameSystem;
}  // namespace game
