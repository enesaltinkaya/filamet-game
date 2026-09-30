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
#include "loading/LoadingGui.h"
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

    static bool worldLoaded = false;

    static bool loadTimingOn() {
        static const bool on = [] {
            const char* e = getenv("ENGINE_LOAD_TIMING");
            return e && e[0] && e[0] != '0';
        }();
        return on;
    }

    static const f32 dollyDistance      = 2.0f;
    static const double dollyLegSeconds = [] {
        if (const char* e = getenv("ENGINE_CAMERA_DOLLY_LEG")) {
            const double v = atof(e);
            if (v > 0.0) return v;
        }
        return 3.5;
    }();

    static f32 dollyBase[3]   = {};
    static f32 dollyFacing[3] = {};
    static f32 dollyEye[3]    = {};
    static double dollyTime   = 0.0;
    static bool dollyAnchored = false;

    static char dollyOn = [] {
        const char* value = getenv("ENGINE_CAMERA_DOLLY");
        return (value != nullptr && value[0] != 0 && value[0] != '0') ? 1 : 0;
    }();
    static char dollyGaveUpPlayer = 0;

    static void dollySet(char on) {
        if (dollyOn == on) return;
        if (on) {
            if (engine::flyingCameraFlying()) {
                utils::info("game: camera dolly: V ignored while flying (ESC ends the fly first)");
                return;
            }
            dollyOn             = 1;
            dollyGaveUpPlayer   = engine::playerMode() ? 1 : 0;
            if (dollyGaveUpPlayer) engine::playerModeSet(0);
            dollyAnchored       = false;
            dollyTime           = 0.0;
            utils::info("game: camera dolly ON — anchored at the current camera, %.0f m per leg (V off, C back to the player)",
                        dollyDistance);
            return;
        }
        const char restored = dollyAnchored;
        dollyOn             = 0;
        dollyAnchored       = false;
        dollyTime           = 0.0;
        if (restored) {
            const f32 target[3] = {dollyBase[0] + dollyFacing[0] * 100.0f,
                                   dollyBase[1] + dollyFacing[1] * 100.0f,
                                   dollyBase[2] + dollyFacing[2] * 100.0f};
            const f32 up[3]     = {0.0f, 1.0f, 0.0f};
            engine::renderer::rendererCameraLookAt(dollyBase, target, up);
        }
        utils::info("game: camera dolly OFF%s", restored ? " — camera back at the anchor" : "");
        if (dollyGaveUpPlayer) {
            dollyGaveUpPlayer = 0;
            engine::playerModeSet(1);
        }
    }
    static void updateCameraDolly(void) {
        if (!dollyOn) return;
        if (gameStateCurrent() != STATE_PLAYING) return;
        if (engine::flyingCameraFlying() || engine::playerMode()) return;

        f32 pos[3], facing[3];
        engine::renderer::rendererCameraGet(pos, facing);
        const f32 facingLength =
            std::sqrt(facing[0] * facing[0] + facing[1] * facing[1] + facing[2] * facing[2]);
        if (facingLength < 1e-6f) return;

        if (!dollyAnchored || std::fabs(pos[0] - dollyEye[0]) > 1e-4f ||
            std::fabs(pos[1] - dollyEye[1]) > 1e-4f || std::fabs(pos[2] - dollyEye[2]) > 1e-4f) {
            const f32 inverse = 1.0f / facingLength;
            for (int i = 0; i < 3; i++) {
                dollyBase[i]   = pos[i];
                dollyEye[i]    = pos[i];
                dollyFacing[i] = facing[i] * inverse;
            }
            dollyTime     = 0.0;
            dollyAnchored = true;
            utils::info("game: camera dolly %.0f m back and forth, %.0f s per leg",
                        dollyDistance,
                        dollyLegSeconds);
        }
        // ENGINE_CAMERA_DOLLY_FRAME_DT: advance the dolly by a FIXED dt per
        // frame (seconds/frame, e.g. 0.0154 = the 65 fps dolly step at the
        // default leg). Wall-clock dt makes the pose lattice fps-dependent,
        // so screenshot runs of two builds never share a pose and the TAA
        // A/B floor (~3 gray) swamps the effect being measured.
        static const double frameDt = [] {
            if (const char* e = getenv("ENGINE_CAMERA_DOLLY_FRAME_DT")) {
                const double v = atof(e);
                return v > 0.0 ? v : 0.0;
            }
            return 0.0;
        }();
        dollyTime += frameDt > 0.0 ? frameDt : (double)utils::timer.dt;

        const double cycle = 2.0 * dollyLegSeconds;
        const double phase = std::fmod(dollyTime, cycle) / cycle;
        const f32 travel   = (f32)(0.5 * (1.0 - std::cos(2.0 * M_PI * phase))) * dollyDistance;
        dollyEye[0]        = dollyBase[0] - dollyFacing[0] * travel;
        dollyEye[1]        = dollyBase[1] - dollyFacing[1] * travel;
        dollyEye[2]        = dollyBase[2] - dollyFacing[2] * travel;

        const f32 target[3] = {dollyEye[0] + dollyFacing[0] * 100.0f,
                               dollyEye[1] + dollyFacing[1] * 100.0f,
                               dollyEye[2] + dollyFacing[2] * 100.0f};
        const f32 up[3]     = {0.0f, 1.0f, 0.0f};
        engine::renderer::rendererCameraLookAt(dollyEye, target, up);
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
        const bool lt        = loadTimingOn();
        const double ltStart = lt ? utils::nanos() : 0.0;
        double ltPrev        = ltStart;
        auto ltLog           = [&](const char* phase) {
            if (!lt) return;
            const double now = utils::nanos();
            utils::info("load timing: %s %.1f ms", phase, (now - ltPrev) / MILLION);
            ltPrev = now;
        };
        if (!worldLoaded) {
            engine::gltf::gltfInit();
            worldLoaded = true;
            utils::info("game: world loaded");
        }

        // Player character (eve) spawn: a saved player row (playerDbLoad) or
        // ENGINE_TELEPORT overrides the static default on world load.
        f32 spawnPt[3] = {-500.0f, 513.0f, 164.0f};
        if (const char* tpos = getenv("ENGINE_TELEPORT")) {
            float tx = 0.0f, ty = 0.0f, tz = 0.0f;
            if (sscanf(tpos, "%g,%g,%g", &tx, &ty, &tz) == 3) {
                spawnPt[0] = tx;
                spawnPt[1] = ty;
                spawnPt[2] = tz;
            }
        }

        // World assets (phase 5): the Oghuzland terrain (scripts/blender-
        // terrain.py export, the standard PBR path), the props model, the
        // character (eve), and the animation source are CPU-parsed in parallel
        // on the thread pool, then their GPU resources are created serially on
        // the render thread. gltfInit is idempotent — it re-creates the loader
        // after the menu-return gltfDestroy on re-entry. ENGINE_GLTF_MODEL
        // swaps the character pak model (asset validation); ENGINE_NO_ANIM
        // skips the animation source.
        const char* gltfModelPath = "models/eve.zstd";
        const char* gltfModelEnv  = getenv("ENGINE_GLTF_MODEL");
        if (gltfModelEnv && gltfModelEnv[0]) {
            gltfModelPath = gltfModelEnv;
        }
        const char* animPath        = getenv("ENGINE_NO_ANIM") ? nullptr : "models/animations.zstd";
        const char* prewarmPaths[4] = {"models/terrain/oghuzlands.zstd",
                                       "models/test2.zstd",
                                       gltfModelPath,
                                       animPath ? animPath : "models/terrain/oghuzlands.zstd"};
        engine::gltf::gltfModelBytesPrewarmLaunch(prewarmPaths, 4);
        engine::gltf::gltfTextureCachePrewarm();
        engine::gltf::gltfModelBytesPrewarmWait();
        ltLog("bytes prewarm + texture fill");
        // The splat CPU parse (the terrain GLB's 2nd, jansson pass) overlaps
        // the serial model loads below; splatTerrainLoad waits + uploads.
        engine::gltf::splatTerrainParseLaunch("models/terrain/oghuzlands.zstd");
        const bool terrainUp = engine::gltf::gltfSceneLoad("models/terrain/oghuzlands.zstd");
        ltLog("terrain");
        const bool propsUp = terrainUp && engine::gltf::gltfPropsLoad("models/test2.zstd");
        ltLog("props");
        const bool charUp = propsUp && engine::gltf::gltfLoad(gltfModelPath);
        ltLog("character");
        const bool animUp =
            charUp && (animPath == nullptr || engine::gltf::gltfLoadAnimations(animPath));
        ltLog("animations");
        engine::physicsTerrainSidecarSet("models/terrain/oghuzlands.jolt.zstd");
        engine::physicsPropsSidecarSet("models/test2.jolt.zstd");
        ltLog("physics sidecars");
        const bool worldUp = terrainUp && propsUp && charUp && animUp;
        if (worldUp) {
            // The Jolt sidecars are only REGISTERED here — the physics system
            // is (re)added deferred a frame later and restores the static
            // terrain/props bodies in added() once the Jolt world is up.
            engine::physicsTerrainSidecarSet("models/terrain/oghuzlands.jolt.zstd");
            engine::physicsPropsSidecarSet("models/test2.jolt.zstd");
            // Splat resources (phase 2): the same packed GLB re-parsed
            // CPU-side — per-chunk buffers + AABBs, weight UDIM arrays, detail
            // sets. No draw until the splat pass lands (tasks 2/3).
            engine::gltf::splatTerrainLoad("models/terrain/oghuzlands.zstd");
            // Animation source: gltfUpdate plays the selected clip on it and
            // syncs the joint transforms onto the visible model; the player
            // system drives clip selection from here on (Player.cpp).
            if (animPath) {
                engine::gltf::gltfPlayAnimation("eve_idle1", 1.0f, true);
            }
        }
        ltLog("splatTerrainLoad");
        ltPrev = ltStart;
        ltLog("loadWorld total");
        utils::info("game: player spawn at (%.0f, %.0f, %.0f)", spawnPt[0], spawnPt[1], spawnPt[2]);

        // The playerSystem (added deferred by the menu) takes over the model
        // at this point.
        engine::playerSetSpawn(spawnPt[0], spawnPt[1], spawnPt[2]);

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
        engine::engineMarkWorldLoaded();
    }

    void GameSystem::finishWorldLoad() {
        gameStateSet(STATE_PLAYING);
        // Jolt world first: the terrain's heightfield sync and the player's
        // character controller both need it alive before they run.
        engine::ecsSystemAddDeferred(100, &engine::physicsSystem);
        engine::ecsSystemAddDeferred(100, &engine::flyingCameraSystem);
        // Third-person player: spawns at the point set by loadWorld (the gltf
        // model is already placed there) and takes the camera in player mode.
        engine::ecsSystemAddDeferred(100, &engine::playerSystem);
        // The loading screen (if any) goes; the in-world readouts + actions
        // panel come up. Both are no-ops for the gui that isn't showing.
        engine::guiManagerRemoveGuiNextFrame(&loadingGui);
        engine::guiManagerAddGuiNextFrame(&cameraGui);
        engine::guiManagerAddGuiNextFrame(&playerGui);
        engine::guiManagerAddGuiNextFrame(&playerActionsGui);
    }

    void GameSystem::preUpdate() {
        if (gameStateCurrent() == STATE_PLAYING && engine::input.pressed == SDL_SCANCODE_V &&
            !pauseMenuGuiIsShowing() && !settingsGuiIsShowing())
            dollySet(!dollyOn);
        if (dollyOn && engine::playerMode()) {
            dollyOn           = 0;
            dollyAnchored     = false;
            dollyTime         = 0.0;
            dollyGaveUpPlayer = 0;
            utils::info("game: camera dolly OFF — player mode owns the camera");
        }

        // In the world and not flying: ESC opens the in-game menu — a
        // separate document from the main menu (the old engine's pauseMenu).
        // Its MAIN MENU button returns to the main menu; ESC while it is
        // showing closes it (the focused document's onkeydown, pauseKeyDown
        // in pauseMenu.lua). ENGINE_NO_RMLUI: no menu to show — ignore ESC.
        if (engine::rmluiDisabled()) return;
        if (engine::input.pressed == SDL_SCANCODE_ESCAPE)
            utils::debug(
                "ESC-DEBUG preUpdate sees esc press (state=%d pause=%d settings=%d) frame=%llu",
                (int)gameStateCurrent(),
                (int)pauseMenuGuiIsShowing(),
                (int)settingsGuiIsShowing(),
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
        if (settingsGuiIsShowing()) engine::guiManagerRemoveGuiNextFrame(&settingsGui);
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
        updateCameraDolly();
        engine::gltf::gltfUpdate(utils::timer.dt);
    }

    GameSystem gameSystem;
}  // namespace game
