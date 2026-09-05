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

// Set while the settings panel was opened from this pause menu: the pause
// document is hidden underneath it and update() re-shows it when the
// settings gui is removed (BACK / ESC). Values: 0 = not involved,
// 1 = opened it but the deferred add has not landed yet (must not
// re-show during that gap), 2 = the settings gui is actually up.
// The old engine instead removed the pause menu here and resumed gameplay
// on close; both documents use a ~63% black background (#0000009f), so
// stacking them ghosted (same reason the main menu hides, see MainMenuGui).
static char settingsOpenFromPause = 0;

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
    settingsOpenFromPause = 0;
    engine::luaRegisterFunction("pauseReturnToGame", pauseReturnToGame);
    engine::luaRegisterFunction("pauseSettingsOpen", pauseSettingsOpen);
    engine::luaRegisterFunction("pauseExitGame", pauseExitGame);

    document = rmlNewDocument("gui/pauseMenu/pauseMenu.html");
    rmlLoadDocument(document);
    rmlShowDocument(document);
    shownFrame = utils::timer.frameCounter;

    // Headless action testing (the same one-shot pattern as the main
    // menu's ENGINE_AUTOTEST): ENGINE_PAUSE_AUTOTEST=back|settings|mainmenu
    // fires once. "settings" opens the settings panel over the (hidden)
    // pause menu, for the close / re-show screenshot runs.
    if (const char* at = getenv("ENGINE_PAUSE_AUTOTEST")) {
        if (utils::strequals(at, "back")) pauseReturnToGame(nullptr);
        else if (utils::strequals(at, "settings")) pauseSettingsOpen(nullptr);
        else if (utils::strequals(at, "mainmenu")) pauseExitGame(nullptr);
    }
}

void PauseMenuGui::removed() {
    settingsOpenFromPause = 0;
    if (document) {
        rmlUnloadDocument(document);
        document = nullptr;
    }
}

void PauseMenuGui::update() {
    // Atomic swap: the manager applies the deferred settings add in
    // postUpdate BEFORE this update(), so 1 -> 2 hides the pause doc the frame
    // the settings doc first shows (state 1 set by the click a frame earlier),
    // and 2 -> 0 + re-show is the frame settings is unloaded. One doc visible
    // per frame — no both-visible, no blank (the old synchronous hide in
    // pauseSettingsOpen left the click frame blank).
    if (settingsOpenFromPause && document) {
        if (settingsGuiIsShowing()) {
            if (settingsOpenFromPause == 1) rmlHideDocument(document);
            settingsOpenFromPause = 2;
        } else if (settingsOpenFromPause == 2) {
            settingsOpenFromPause = 0;
            rmlShowDocument(document);
        }
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

// SETTINGS opens the settings panel over the pause menu, hiding this
// document underneath (re-shown by update() when the panel closes).
int pauseSettingsOpen(void* _) {
    (void)_;
    if (!settingsGuiIsShowing()) {
        // No rmlHideDocument here: this click frame still renders the pause
        // doc; the hide is done in update() (state 1 -> 2) on the frame the
        // settings doc shows, so the swap is atomic (no blank frame).
        settingsOpenFromPause = 1;
        engine::guiManagerAddGuiNextFrame(&settingsGui);
    }
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
