#include "Game.h"
#include "Utils.h"
#include "Engine.h"
#include "ecs/system/flyingCamera/FlyingCamera.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "ecs/system/heightmap/HeightmapTerrainRender.h"
#include "gui/GuiManager.h"
#include "gltf/Gltf.h"
#include "renderer/Renderer.h"
#include "renderer/Window.h"
#include "gameState/GameState.h"
#include "loadingAzgaar/LoadingAzgaar.h"
#include "mainMenu/MainMenuGui.h"
#include "azgaar/AzgaarSettlements.h"

#include <SDL.h>

#include <cmath>
#include <cstring>

namespace game {

    static bool worldLoaded     = false;
    static bool s_acceptanceRan = false;

    // One HeightmapTerrain per world (file-static; heightmapTerrainInit is
    // idempotent and frees any previously resident tiles). Backed by
    // loadingAzgaar's source, which outlives it.
    static engine::HeightmapTerrain s_terrain = {};

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
            {400.0f, 2600.0f}, {500.0f, 2700.0f}, {560.0f, 2740.0f},
            {340.0f, 2820.0f}, {620.0f, 2580.0f},
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
            const f32 a  = src->heightAt(src->userData, wx, wz);
            const f32 b    = src->heightAt(src->userData, wx, wz);
            const bool twiceOk = (memcmp(&a, &b, sizeof(a)) == 0);
            const f32 sampled  = engine::heightmapTerrainSample(&s_terrain, wx, wz);
            const f32 diff     = fabsf(sampled - a);
            allOk = allOk && twiceOk && (diff < 1.0f);
            utils::info("heightmap acceptance: probe (%.0f, %.0f): heightAt %.6f twice %s, "
                        "sample %.6f diff %.4f m",
                        wx, wz, a, twiceOk ? "bit-identical" : "MISMATCH", sampled, diff);
        }

        // 3: adjacent READY tiles share a bit-identical border.
        bool borderChecked = false;
        for (u32 i = 0; i < n && !borderChecked; ++i) {
            for (u32 j = 0; j < n; ++j) {
                if (i == j) continue;
                const engine::HeightmapTileView& a = views[i];
                const engine::HeightmapTileView& b = views[j];
                bool ok                            = false, found = false;
                if (b.tileX == a.tileX + 1 && b.tileZ == a.tileZ) {
                    // shared column: a.x = TEX-1 vs b.x = 0 (grid is [z*dim + x])
                    found = true;
                    for (u32 z = 0; z < HEIGHTMAP_TEX && ok; ++z)
                        ok = (a.heights[z * HEIGHTMAP_TEX + (HEIGHTMAP_TEX - 1)] == b.heights[z * HEIGHTMAP_TEX]);
                } else if (b.tileZ == a.tileZ + 1 && b.tileX == a.tileX) {
                    // shared row: a.z = TEX-1 vs b.z = 0
                    found = true;
                    ok = (memcmp(a.heights + (HEIGHTMAP_TEX - 1) * HEIGHTMAP_TEX,
                                 b.heights, sizeof(float) * HEIGHTMAP_TEX) == 0);
                }
                if (found) {
                    borderChecked = true;
                    allOk         = allOk && ok;
                    utils::info("heightmap acceptance: border tiles (%d,%d)/(%d,%d): %s",
                                a.tileX, a.tileZ, b.tileX, b.tileZ,
                                ok ? "bit-identical" : "MISMATCH");
                }
            }
        }
        if (!borderChecked) allOk = false; // unreachable: gated above

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
        snprintf(path, sizeof(path), "%s/climate_temp.ppm", dir);   // R = temp + 64
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
            look.biomeColorW = w;
            look.biomeColorH = h;
        }
        w = 0;
        h = 0;
        std::vector<u8> climatePixels = azgaarWorldPackClimateTexture(world, &w, &h);
        if (!climatePixels.empty()) {
            look.climatePixels = climatePixels.data();
            look.climateW = w;
            look.climateH = h;
        }
        look.climateEnabled = look.biomeColorPixels != nullptr && look.climatePixels != nullptr;

        // Map bounds in world metres (azgaarMapToWorld centres the map at
        // the world origin).
        const f32 halfW = (f32)world->widthPx * 0.5f * (f32)world->metersPerPixel;
        const f32 halfH = (f32)world->heightPx * 0.5f * (f32)world->metersPerPixel;
        look.mapMinX = -halfW;
        look.mapMinZ = -halfH;
        look.mapMaxX = halfW;
        look.mapMaxZ = halfH;
        look.maxLandHeightM = world->maxLandHeightM;

        look.snowLoC = azgaarEnvFloat("ENGINE_AZGAAR_SNOW_LO", -1.0f);
        look.snowHiC = azgaarEnvFloat("ENGINE_AZGAAR_SNOW_HI", 3.0f);
        look.beachHeightM = azgaarEnvFloat("ENGINE_AZGAAR_BEACH_H", 2.5f);
        if (getenv("ENGINE_AZGAAR_CLIMATE_DISABLED")) {
            look.snowLoC = look.snowHiC = look.beachHeightM = 0.0f;
            look.climateEnabled = false;
        }

        dumpWorldLook(look);
        engine::heightmapTerrainRenderRegisterLook(&look);

        // ENGINE_TERRAIN_DEBUG=<off|ramp|biome>: validation views (ramp =
        // periodic hue per 256 m of height, biome = raw biome-colour
        // texture through the map-space UV).
        const char* dbg = getenv("ENGINE_TERRAIN_DEBUG");
        u32 debugMode = 0;
        if (dbg && utils::strequals(dbg, "ramp")) {
            debugMode = 1;
        } else if (dbg && utils::strequals(dbg, "biome")) {
            debugMode = 2;
        }
        engine::heightmapTerrainRenderSetDebugView(debugMode);
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
            engine::heightmapTerrainInit(&s_terrain, &src->vtable,
                                         HEIGHTMAP_TILE_SIZE_M, HEIGHTMAP_WINDOW_SIZE);
            engine::heightmapTerrainSetActive(&s_terrain);

            // Terrain render pass: per-world look (biome/climate textures +
            // bounds + thresholds), registered before any tile renders.
            terrainRegisterWorldLook(loadingAzgaarGetWorld());
        }

        // TODO(azgaar): frame the camera from the world bounds instead of this center.
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
        } else {
            f32 eye[3] = {center[0] + 500.0f, center[1] + 1590.0f, center[2] + 2700.0f};
            f32 up[3]  = {0.0f, 1.0f, 0.0f};
            engine::renderer::rendererCameraLookAt(eye, center, up);
        }

        // sun (directional) + constant ambient, backend-agnostic
        f32 sunDirection[3] = {-0.6f, -1.0f, -0.5f};
        f32 sunColor[3]     = {1.0f, 0.97f, 0.92f};
        engine::renderer::rendererSetSun(sunDirection, sunColor, 110000.0f);

        f32 ambient[3] = {0.32f, 0.35f, 0.38f};
        engine::renderer::rendererSetAmbient(ambient, 30000.0f);

    }

    void GameSystem::preUpdate() {
        // In the world and not flying: ESC returns to the main menu
        if (gameStateCurrent() == STATE_PLAYING && !engine::flyingCameraFlying() &&
            engine::input.pressed == SDL_SCANCODE_ESCAPE) {
            utils::info("game: back to main menu");
            gameStateSet(STATE_MAIN_MENU);
            engine::ecsSystemRemoveDeferred(&engine::flyingCameraSystem);
            engine::ecsSystemRemoveDeferred(&engine::heightmapTerrainSystem);
            // Drop the terrain's tile data + render look while the menu is
            // up (world and source stay retained; re-entering the world
            // re-inits the terrain and re-registers the look). The
            // settlement plateau grid is cleared only at world release — it
            // is valid as long as the world is.
            engine::heightmapTerrainRenderReleaseLook();
            engine::heightmapTerrainDestroyData(&s_terrain);
            engine::heightmapTerrainSetActive(nullptr);
            engine::gui::guiAdd(&mainMenuGui);
        }
    }

    void GameSystem::removed() {
        engine::systemRemove(&engine::flyingCameraSystem);
        engine::systemRemove(&engine::heightmapTerrainSystem);
        if (worldLoaded) {
            // Terrain before world: heightAt dereferences the world, and the
            // plateau grid indexes into world->settlements.
            engine::heightmapTerrainRenderReleaseLook();
            engine::heightmapTerrainDestroyData(&s_terrain);
            engine::heightmapTerrainSetActive(nullptr);
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
        engine::gltf::gltfUpdate(utils::nanos() / BILLION);

        // One-shot acceptance probe, fired once its tiles are up.
        if (!s_acceptanceRan && worldLoaded && s_terrain.initialized && s_terrain.tilesReady > 0)
            s_acceptanceRan = heightmapAcceptanceLog();
    }

    GameSystem gameSystem;
}  // namespace game
