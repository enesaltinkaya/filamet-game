#include "MainMenuGui.h"
#include "Game.h"
#include "cameraGui/CameraGui.h"
#include "pauseMenu/PauseMenuGui.h"
#include "playerGui/PlayerGui.h"
#include "playerActionsGui/PlayerActionsGui.h"
#include "settingsGui/SettingsGui.h"
#include "settingsGui/audio/SettingsAudioGui.h"
#include "settingsGui/video/SettingsVideoGui.h"
#include "gui/GuiManager.h"
#include "gui/rmlui/GuiManagerRmlUi.h"
#include "gameState/GameState.h"
#include "credits/CreditsGui.h"
#include "ecs/system/lua/LuaSystem.h"
#include "ecs/system/flyingCamera/FlyingCamera.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "ecs/system/player/Player.h"
#include "ecs/system/physics/PhysicsSystem.h"
#include "Engine.h"
#include "Utils.h"

#include "crmlui.h"

#include <cstdlib>

namespace game {
MainMenuGui mainMenuGui;

MainMenuGui::MainMenuGui() : engine::System("mainMenu") {}

static void* document = nullptr;
static void* model    = nullptr;

// SETTINGS opens the settings panel OVER the menu (the old engine's
// slide-over: a right-side panel over the dimmed menu, per settings.css) —
// the menu stays underneath and BACK just closes the panel.
static void openSettings(void) {
    if (!settingsGuiIsShowing())
        engine::guiManagerAddGuiNextFrame(&settingsGui);
}

static void enterWorld(void) {
    utils::info("mainMenu: ENTER WORLD");
    // A settings page left open over the menu must not ride along into the
    // world (the old engine hid it on the state transition). The sub-pages
    // (audio/video so far) stay registered while their document shows, so
    // they are removed too.
    if (settingsGuiIsShowing())
        engine::guiManagerRemoveGuiNextFrame(&settingsGui);
    if (settingsAudioGuiIsShowing())
        engine::guiManagerRemoveGuiNextFrame(&settingsAudioGui);
    if (settingsVideoGuiIsShowing())
        engine::guiManagerRemoveGuiNextFrame(&settingsVideoGui);
    gameSystem.loadWorld();  // blocks a moment on first entry only
    gameStateSet(STATE_PLAYING);
    // Jolt world first: the terrain's heightfield sync and the player's
    // character controller both need it alive before they run.
    engine::ecsSystemAddDeferred(100, &engine::physicsSystem);
    engine::ecsSystemAddDeferred(100, &engine::flyingCameraSystem);
    // no-op until a world sets an active HeightmapTerrain (phase 4)
    engine::ecsSystemAddDeferred(100, &engine::heightmapTerrainSystem);
    // Third-person player: spawns at the point set by loadWorld (the gltf
    // model is already placed there) and takes the camera in player mode.
    engine::ecsSystemAddDeferred(100, &engine::playerSystem);
    engine::guiManagerRemoveGuiNextFrame(&mainMenuGui);
    // the old engine showed the camera + player debug readouts + player
    // actions panel while in the world
    engine::guiManagerAddGuiNextFrame(&cameraGui);
    engine::guiManagerAddGuiNextFrame(&playerGui);
    engine::guiManagerAddGuiNextFrame(&playerActionsGui);
}

static void exitGame(void) {
    utils::info("mainMenu: EXIT");
    engine::engineStop();
}

void mainMenuGuiEnterWorld(void) {
    enterWorld();
}

// ── Lua callbacks (onclick handlers in mainMenu.html) ────────────────────
// EXIT is handled by the engine-global luacExit (LuaSystem); CREDITS just
// opens the overlay gui on top of the menu, like the old engine did.

static int luaSettingsOpen(void* _) {
    (void)_;
    openSettings();
    return 0;
}

static int luaCreditsOpen(void* _) {
    (void)_;
    engine::gui::guiAdd(&creditsGui);
    return 0;
}

static int luaPlayGame(void* _) {
    (void)_;
    // Hide the document synchronously so it can't take a second click while
    // the state tears down; the actual document unload happens on the next
    // frame via removed() (safe to call mid-RML-event).
    rmlHideDocument(document);
    enterWorld();
    return 0;
}

void MainMenuGui::added() {
    engine::luaRegisterFunction("settingsOpen", luaSettingsOpen);
    engine::luaRegisterFunction("creditsOpen", luaCreditsOpen);
    engine::luaRegisterFunction("playGame", luaPlayGame);

    document = rmlNewDocument("gui/mainMenu/mainMenu.html");
    model    = rmlCreateModel("mainMenu");

    rmlLoadDocument(document);
    rmlShowDocument(document);

    // Headless action testing (mirrors the ENGINE_SCREENSHOT/ENGINE_LOG_TIMEOUT
    // automated-run pattern): ENGINE_AUTOTEST=enter|pause|settings|credits|exit
    // fires once. "pause" enters the world and opens the in-game menu (the
    // pause menu screenshot run; its own actions live in
    // ENGINE_PAUSE_AUTOTEST=back|mainmenu).
    static char autotestRan = 0;
    if (!autotestRan) {
        const char* at = getenv("ENGINE_AUTOTEST");
        if (at && at[0]) {
            autotestRan = 1;
            if (utils::strequals(at, "enter")) enterWorld();
            else if (utils::strequals(at, "pause")) {
                enterWorld();
                engine::guiManagerAddGuiNextFrame(&pauseMenuGui);
            }
            else if (utils::strequals(at, "settings")) openSettings();
            else if (utils::strequals(at, "credits")) engine::gui::guiAdd(&creditsGui);
            else if (utils::strequals(at, "exit")) exitGame();
        }
    }
}

void MainMenuGui::removed() {
    if (document) {
        rmlUnloadDocument(document);
        document = nullptr;
    }
    if (model) {
        rmlUnloadModel(model);
        model = nullptr;
    }
}
}  // namespace game
