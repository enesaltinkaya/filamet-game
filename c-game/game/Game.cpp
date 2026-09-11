#include "Game.h"
#include "Utils.h"
#include "Engine.h"
#include "ecs/system/flyingCamera/FlyingCamera.h"
#include "ecs/system/player/Player.h"
#include "ecs/system/physics/PhysicsSystem.h"
#include "gui/GuiManager.h"
#include "gui/rmlui/GuiManagerRmlUi.h"
#include "gltf/Gltf.h"
#include "renderer/Renderer.h"
#include "renderer/Window.h"
#include "gameState/GameState.h"
#include "mainMenu/MainMenuGui.h"
#include "pauseMenu/PauseMenuGui.h"
#include "cameraGui/CameraGui.h"
#include "playerGui/PlayerGui.h"
#include "playerActionsGui/PlayerActionsGui.h"
#include "settingsGui/SettingsGui.h"

#include <SDL.h>

#include <cmath>
#include <cstring>
#include <algorithm>
#include <unordered_map>

namespace game {

    static bool worldLoaded     = false;

    // Automated validation hook: ENGINE_CAMERA_DOLLY="vx,vy,vz" pans the camera
    // at that velocity (m/s) while the world is up. Combined with
    // ENGINE_SCREENSHOT_FRAME it screenshots a camera that has actually moved,
    // so a headless run exercises the TAA reprojection + the world anchor
    // re-centering (the gltf placement roots re-derive against the moving
    // camera eye every frame).
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
        engine::gui::guiInit();
        if (engine::rmluiDisabled()) {
            // ENGINE_NO_RMLUI: there is no main menu to show — take the
            // menu's ENTER WORLD action directly.
            mainMenuGuiEnterWorld();
        } else {
            gameStateSet(STATE_MAIN_MENU);
            engine::guiManagerAddGuiNextFrame(&mainMenuGui);
        }
    }

    void GameSystem::loadWorld() {
        if (!worldLoaded) {
            engine::gltf::gltfInit();
            worldLoaded = true;
            utils::info("game: world loaded");
        }

        // Oghuzland terrain world (scripts/blender-terrain.py export): the
        // chunked terrain model through the standard PBR path (untextured
        // until the splat UDIM pass lands — plans/blender-terrain.md phase 2)
        // plus its pre-baked Jolt sidecar. The sidecar is only REGISTERED
        // here — the physics system is (re)added deferred a frame later and
        // restores the static terrain bodies in added() once the Jolt world
        // is up.
        bool terrainUp = engine::gltf::gltfSceneLoad("models/terrain/oghuzlands.zstd");
        if (terrainUp) {
            engine::physicsTerrainSidecarSet("models/terrain/oghuzlands.jolt.zstd");
            // Splat resources (phase 2): the same packed GLB re-parsed
            // CPU-side — per-chunk buffers + AABBs, weight UDIM arrays, detail
            // sets. No draw until the splat pass lands (tasks 2/3).
            engine::gltf::splatTerrainLoad("models/terrain/oghuzlands.zstd");
        }

        if (engine::gltf::gltfPropsLoad("models/test2.zstd")) {
            engine::physicsPropsSidecarSet("models/test2.jolt.zstd");
        }

        // Player character (eve): a zstd-compressed glb exported by
        // scripts/export-models.sh. Static spawn — a saved player row
        // (playerDbLoad) or ENGINE_TELEPORT overrides it on world load.
        f32 spawnPt[3] = {-500.0f, 513.0f, 164.0f};
        // Override the spawn with an explicit position (ENGINE_TELEPORT="x,y,z").
        if (const char* tpos = getenv("ENGINE_TELEPORT")) {
            float tx = 0.0f, ty = 0.0f, tz = 0.0f;
            if (sscanf(tpos, "%g,%g,%g", &tx, &ty, &tz) == 3) {
                spawnPt[0] = tx;
                spawnPt[1] = ty;
                spawnPt[2] = tz;
            }
        }
        // gltfInit is idempotent — this re-creates the loader after the
        // menu-return gltfDestroy on re-entry. The model is PLACED after the
        // camera framing below (placement re-expresses the feet relative to
        // the world anchor, which the framing's cameraLookAt establishes).
        // ENGINE_GLTF_MODEL loads a different pak model through the same PBR
        // path (asset validation: chunked terrain exports, props, ...).
        const char* gltfModelPath = "models/eve.zstd";
        const char* gltfModelEnv = getenv("ENGINE_GLTF_MODEL");
        if (gltfModelEnv && gltfModelEnv[0]) {
            gltfModelPath = gltfModelEnv;
        }
        if (engine::gltf::gltfInit() && engine::gltf::gltfLoad(gltfModelPath)) {
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

        // The playerSystem (added deferred by the menu) takes over the model
        // at this point.
        engine::playerSetSpawn(spawnPt[0], spawnPt[1], spawnPt[2]);

        // Camera framing: ENGINE_CAMERA selects a validation vantage; the
        // default frames the spawn on the terrain.
        f32 center[3] = {spawnPt[0], spawnPt[1], spawnPt[2]};

        const char* cameraMode = getenv("ENGINE_CAMERA");
        if (cameraMode && utils::strequals(cameraMode, "topdown")) {
            // top-down view (validation shots): up = -z keeps the frame stable
            f32 eye[3] = {center[0], center[1] + 9000.0f, center[2] + 0.01f};
            f32 up[3]  = {0.0f, 0.0f, -1.0f};
            engine::renderer::rendererCameraLookAt(eye, center, up);
        } else if (cameraMode && utils::strequals(cameraMode, "close")) {
            f32 eye[3]    = {center[0] + 180.0f, center[1] + 60.0f, center[2] + 180.0f};
            f32 lookAt[3] = {center[0], center[1], center[2] + 60.0f};
            f32 up[3]     = {0.0f, 1.0f, 0.0f};
            engine::renderer::rendererCameraLookAt(eye, lookAt, up);
        } else if (cameraMode && utils::strequals(cameraMode, "ground")) {
            f32 eye[3]    = {center[0] + 12.0f, center[1] + 6.0f, center[2] + 12.0f};
            f32 up[3]     = {0.0f, 1.0f, 0.0f};
            engine::renderer::rendererCameraLookAt(eye, center, up);
        } else if (cameraMode && utils::strequals(cameraMode, "cast")) {
            f32 lookAt[3] = {center[0] - 30.0f, center[1] + 0.3f, center[2] - 30.0f};
            f32 eye[3]    = {center[0] - 36.0f, center[1] + 4.3f, center[2] - 36.0f};
            f32 up[3]     = {0.0f, 1.0f, 0.0f};
            engine::renderer::rendererCameraLookAt(eye, lookAt, up);
        } else if (cameraMode && utils::strequals(cameraMode, "shadow")) {
            f32 eye[3]    = {center[0] - 1.66f, center[1] - 0.3f, center[2] + 2.0f};
            f32 lookAt[3] = {center[0] + 0.25f, center[1] - 1.55f, center[2] + 0.21f};
            f32 up[3]     = {0.0f, 1.0f, 0.0f};
            engine::renderer::rendererCameraLookAt(eye, lookAt, up);
        } else if (cameraMode && utils::strequals(cameraMode, "character")) {
            // Portrait of the player character (eve at the old spawn point):
            // ~1 character-height diagonal back, eye slightly above chest,
            // looking at chest height — close enough for a texture/material
            // check, far enough that the whole silhouette is in frame. The
            // framing uses the LOCAL bounds + spawn (the model is placed
            // after framing, once the anchor exists).
            f32 lmin[3], lmax[3];
            if (engine::gltf::gltfLocalBoundingBox(lmin, lmax)) {
                // feet y: the spawn y (terrain surface probe / teleport value)
                const f32 feetY = spawnPt[1];
                // placement pins the local ORIGIN (feet) at the spawn point, so
                // the body centre sits at spawn + (centre - origin) — the
                // local centre relative to the origin, not relative to min
                const double cx   = spawnPt[0] + (lmax[0] + lmin[0]) * 0.5;
                const double cz   = spawnPt[2] + (lmax[2] + lmin[2]) * 0.5;
                const double h    = lmax[1] - lmin[1];
                const double chest[3] = {cx, feetY + h * 0.6, cz};
                const double eye[3]   = {chest[0] + h, chest[1] + h * 0.2, chest[2] - h};
                const double up[3]    = {0.0, 1.0, 0.0};
                utils::info("game: character camera — local bounds [%.2f %.2f %.2f]-[%.2f %.2f %.2f]",
                            lmin[0],
                            lmin[1],
                            lmin[2],
                            lmax[0],
                            lmax[1],
                            lmax[2]);
                engine::renderer::rendererCameraLookAt(eye, chest, up);
            } else {
                utils::warn("game: character camera — no gltf bounds, keeping default camera");
            }
        } else {
            f32 eye[3]    = {center[0] + 180.0f, center[1] + 75.0f, center[2] + 180.0f};
            f32 lookAt[3] = {center[0], center[1] + 30.0f, center[2]};
            f32 up[3]     = {0.0f, 1.0f, 0.0f};
            engine::renderer::rendererCameraLookAt(eye, lookAt, up);
        }

        // Model placement: NOW that the camera (and therefore the world
        // anchor) is framed — the feet are re-expressed relative to it.
        engine::gltf::gltfPlaceAt(spawnPt[0], spawnPt[1], spawnPt[2]);

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
        // In the world and not flying: ESC opens the in-game menu — a
        // separate document from the main menu (the old engine's pauseMenu).
        // Its MAIN MENU button returns to the main menu; ESC while it is
        // showing closes it (the focused document's onkeydown, pauseKeyDown
        // in pauseMenu.lua). ENGINE_NO_RMLUI: no menu to show — ignore ESC.
        if (engine::rmluiDisabled()) return;
        if (engine::input.pressed == SDL_SCANCODE_ESCAPE)
            utils::debug("ESC-DEBUG preUpdate sees esc press (state=%d pause=%d settings=%d) frame=%llu",
                         (int)gameStateCurrent(), (int)pauseMenuGuiIsShowing(), (int)settingsGuiIsShowing(),
                         (unsigned long long)utils::timer.frameCounter);
        if (gameStateCurrent() == STATE_PLAYING && !engine::flyingCameraFlying() &&
            engine::input.pressed == SDL_SCANCODE_ESCAPE) {
            // Guarded like the old engine: while the pause document or the
            // settings slide-over is focused, ESC belongs to that document
            // (re-adding here would race the deferred removal).
            if (!pauseMenuGuiIsShowing() && !settingsGuiIsShowing())
                engine::guiManagerAddGuiNextFrame(&pauseMenuGui);
        }
    }

    void GameSystem::backToMainMenu() {
        utils::info("game: back to main menu");
        gameStateSet(STATE_MAIN_MENU);
        engine::ecsSystemRemoveDeferred(&engine::playerSystem);
        engine::ecsSystemRemoveDeferred(&engine::flyingCameraSystem);
        engine::ecsSystemRemoveDeferred(&engine::physicsSystem);
        // A settings panel left open over the in-game menu must not ride
        // along into the main menu (the same guard as enterWorld).
        if (settingsGuiIsShowing())
            engine::guiManagerRemoveGuiNextFrame(&settingsGui);
        engine::guiManagerAddGuiNextFrame(&mainMenuGui);
        engine::guiManagerRemoveGuiNextFrame(&cameraGui);
        engine::guiManagerRemoveGuiNextFrame(&playerGui);
        engine::guiManagerRemoveGuiNextFrame(&playerActionsGui);
    }

    void GameSystem::removed() {
        engine::systemRemove(&engine::playerSystem);
        engine::systemRemove(&engine::flyingCameraSystem);
        engine::systemRemove(&engine::physicsSystem);
        if (worldLoaded) {
            engine::gltf::splatTerrainDestroy();
            engine::gltf::gltfDestroy();

            // the renderer owns the sun/ambient now; they stay set for the
            // next world load and affect nothing while no geometry is drawn
            worldLoaded = false;
        }
        utils::info("game: removed");
    }

    void GameSystem::update() {
        engine::gltf::gltfUpdate(utils::timer.dt);
        updateCameraDolly();
    }

    GameSystem gameSystem;
}
