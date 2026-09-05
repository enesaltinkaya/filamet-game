#include "PauseMenuGui.h"
#include "Game.h"
#include "settingsGui/SettingsGui.h"
#include "ecs/system/lua/LuaSystem.h"
#include "gui/rmlui/GuiManagerRmlUi.h"
#include "Utils.h"

#include "crmlui.h"

#include <cstdlib>

namespace game {
PauseMenuGui pauseMenuGui;

PauseMenuGui::PauseMenuGui() : engine::System("pauseMenu") {}

static void* document = nullptr;

// Frame the document was shown. The ESC press that opened the menu is pumped
// into the document on that same frame (the rmlui input pump runs after the
// deferred adds are applied in postUpdate) — a close action landing on the
// shown frame is that press, not a second one, so it must not close the menu
// (one tap used to open+close it instantly). Real presses land on later
// frames; repeats don't (Window.cpp filters SDL key repeat).
static u64 shownFrame = 0;

static int pauseReturnToGame(void* _);
static int pauseSettingsOpen(void* _);
static int pauseExitGame(void* _);

void PauseMenuGui::added() {
    // onclick handlers in pauseMenu.html. pauseKeyDown (body onkeydown:
    // ESC -> RETURN TO GAME) lives in pauseMenu.lua itself.
    engine::luaRegisterFunction("pauseReturnToGame", pauseReturnToGame);
    engine::luaRegisterFunction("pauseSettingsOpen", pauseSettingsOpen);
    engine::luaRegisterFunction("pauseExitGame", pauseExitGame);

    document = rmlNewDocument("gui/pauseMenu/pauseMenu.html");
    rmlLoadDocument(document);
    rmlShowDocument(document);
    shownFrame = utils::timer.frameCounter;

    // Headless action testing (the same one-shot pattern as the main
    // menu's ENGINE_AUTOTEST): ENGINE_PAUSE_AUTOTEST=back|mainmenu fires
    // once, for the close / return-to-menu screenshot runs.
    if (const char* at = getenv("ENGINE_PAUSE_AUTOTEST")) {
        if (utils::strequals(at, "back")) pauseReturnToGame(nullptr);
        else if (utils::strequals(at, "mainmenu")) pauseExitGame(nullptr);
    }
}

void PauseMenuGui::removed() {
    if (document) {
        rmlUnloadDocument(document);
        document = nullptr;
    }
}

char pauseMenuGuiIsShowing(void) {
    return document != nullptr;
}

int pauseReturnToGame(void* _) {
    (void)_;  // pauseMenu.lua's ESC handler + the RETURN TO GAME button
    utils::debug("ESC-DEBUG pauseReturnToGame frame=%llu shownFrame=%llu",
                 (unsigned long long)utils::timer.frameCounter, (unsigned long long)shownFrame);
    if (utils::timer.frameCounter <= shownFrame) return 0;  // the opening press
    engine::guiManagerRemoveGuiNextFrame(&pauseMenuGui);
    return 0;
}

// SETTINGS opens the settings slide-over OVER the pause menu (the new
// engine's main-menu pattern: the menu stays underneath and BACK just
// closes the panel, re-exposing this document).
int pauseSettingsOpen(void* _) {
    (void)_;
    if (!settingsGuiIsShowing())
        engine::guiManagerAddGuiNextFrame(&settingsGui);
    return 0;
}

// MAIN MENU (the old engine's "EXIT GAME" action): tear the world down and
// bring up the main menu.
int pauseExitGame(void* _) {
    (void)_;
    engine::guiManagerRemoveGuiNextFrame(&pauseMenuGui);
    gameSystem.backToMainMenu();
    return 0;
}
}  // namespace game
