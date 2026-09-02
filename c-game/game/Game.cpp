#include "Game.h"
#include "Utils.h"
#include "Engine.h"
#include "ecs/system/flyingCamera/FlyingCamera.h"
#include "gui/GuiManager.h"
#include "gltf/Gltf.h"
#include "renderer/Renderer.h"
#include "renderer/Window.h"
#include "terrain/Terrain.h"
#include "gameState/GameState.h"
#include "mainMenu/MainMenuGui.h"

#include <SDL.h>

#include <cmath>

namespace game {

    static bool worldLoaded = false;

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
        if (worldLoaded) return;

        engine::gltf::gltfInit();
        engine::terrain::terrainInit("models/terrain/oghuzlands.json");
        engine::gltf::gltfLoad("models/terrain/oghuzlands.glb");
        engine::terrain::terrainApplyToAsset();

        // frame the terrain: slightly off-center, looking across it
        f32 min[3];
        f32 max[3];
        engine::gltf::gltfBoundingBox(min, max);
        f32 center[3] = {(min[0] + max[0]) * 0.5f,
                         (min[1] + max[1]) * 0.5f,
                         (min[2] + max[2]) * 0.5f};

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

        worldLoaded = true;
        utils::info("game: world loaded");
    }

    void GameSystem::preUpdate() {
        // In the world and not flying: ESC returns to the main menu
        if (gameStateCurrent() == STATE_PLAYING && !engine::flyingCameraFlying() &&
            engine::input.pressed == SDL_SCANCODE_ESCAPE) {
            utils::info("game: back to main menu");
            gameStateSet(STATE_MAIN_MENU);
            engine::ecsSystemRemoveDeferred(&engine::flyingCameraSystem);
            engine::gui::guiAdd(&mainMenuGui);
        }
    }

    void GameSystem::removed() {
        engine::systemRemove(&engine::flyingCameraSystem);
        if (worldLoaded) {
            // destroy the glTF asset first: its renderables still reference
            // the terrain material
            engine::gltf::gltfDestroy();
            engine::terrain::terrainDestroy();

            // the renderer owns the sun/ambient now; they stay set for the
            // next world load and affect nothing while no geometry is drawn
            worldLoaded = false;
        }
        utils::info("game: removed");
    }

    void GameSystem::update() {
        engine::gltf::gltfUpdate(utils::nanos() / BILLION);
    }

    GameSystem gameSystem;
}  // namespace game
