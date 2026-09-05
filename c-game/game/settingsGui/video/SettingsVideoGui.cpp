#include "SettingsVideoGui.h"
#include "../SettingsGui.h"
#include "ecs/system/lua/LuaSystem.h"
#include "gui/rmlui/GuiManagerRmlUi.h"
#include "renderer/Window.h"
#include "Utils.h"

#include "crmlui.h"

#include <cstdlib>

namespace game {

SettingsVideoGui settingsVideoGui;

SettingsVideoGui::SettingsVideoGui() : engine::System("settingsVideoGui") {}

static void* document = nullptr;
static void* model    = nullptr;

// Bound in added() (rmlBind*): the DOM writes into these on rmlUpdate
// (after update()), and the labels' {{format(x,N)}} read them back.
static char  fullScreen      = 0;
static char  vsync           = 0;
static char  showFps         = 0;
static char  fpsLimitChecked = 0;
static float fpsLimit        = 0.0f;
static float uiScale         = 1.0f;
static float cursorScale     = 1.0f;

// The old engine deferred every onchange through futureTask(0, ...Later)
// because the bound variables only sync in rmlUpdate (which runs after
// update()); applying inside the click handler read the stale value. Same
// deferral here: onchange marks the control dirty and update() applies it
// after a 50 ms settle (always at least one frame later, i.e. synced).
static double lastChange = 0.0;
static char   dirtyFullScreen  = 0;
static char   dirtyVsync       = 0;
static char   dirtyShowFps     = 0;
static char   dirtyFpsChecked  = 0;
static char   dirtyFpsLimit    = 0;
static char   dirtyUiScale     = 0;
static char   dirtyCursorScale = 0;

static void reinitTimer(void) {
    utils::timerInit(utils::settingsGetDouble("fpsLimit"),
                     utils::settingsGetBool("fpsLimitChecked"),
                     utils::settingsGetBool("busyLoopLinux"));
}

// Persist the pending changes, one settingsWrite at the end. toEngine applies
// them to the live engine (update()); 0 from removed() at shutdown, where the
// window/renderer/gui subsystems may already be torn down (docs/lessons.md
// removed()-ordering entry) — persistence is still safe (plain c-utils).
// Vsync is live: the renderer reads the setting on every present and passes
// it to Diligent's Present interval (swapchain recreated once on change,
// see DiligentRenderer draw). Cursor scaling has no engine support in this
// build: it is persisted only, and the warn fires while the audio/UI stack
// is alive.
static void applyChanges(char toEngine) {
    char wrote = 0;

    if (dirtyFullScreen && fullScreen != (char)utils::settingsGetBool("fullScreen")) {
        utils::settingsSetBool("fullScreen", fullScreen);
        if (toEngine) {
            engine::windowToggleFullscreen(fullScreen);
        }
        dirtyFullScreen = 0;
        wrote           = 1;
    }
    if (dirtyVsync && vsync != (char)utils::settingsGetBool("vsync")) {
        utils::settingsSetBool("vsync", vsync);
        dirtyVsync = 0;
        wrote      = 1;
    }
    if (dirtyShowFps && showFps != (char)utils::settingsGetBool("showFps")) {
        utils::settingsSetBool("showFps", showFps);
        if (toEngine) {
            engine::guiManagerToggleShowFps();
        }
        dirtyShowFps = 0;
        wrote        = 1;
    }
    if (dirtyFpsChecked && fpsLimitChecked != (char)utils::settingsGetBool("fpsLimitChecked")) {
        utils::settingsSetBool("fpsLimitChecked", fpsLimitChecked);
        if (toEngine) {
            reinitTimer();
        }
        dirtyFpsChecked = 0;
        wrote           = 1;
    }
    if (dirtyFpsLimit && fpsLimit != (float)utils::settingsGetDouble("fpsLimit")) {
        utils::settingsSetDouble("fpsLimit", fpsLimit);
        if (toEngine) {
            reinitTimer();
        }
        dirtyFpsLimit = 0;
        wrote         = 1;
    }
    if (dirtyUiScale && uiScale != (float)utils::settingsGetDouble("uiScale")) {
        utils::settingsSetDouble("uiScale", uiScale);
        if (toEngine) {
            engine::guiManagerUpdateScale();
        }
        dirtyUiScale = 0;
        wrote        = 1;
    }
    if (dirtyCursorScale && cursorScale != (float)utils::settingsGetDouble("cursorScale")) {
        utils::settingsSetDouble("cursorScale", cursorScale);
        if (toEngine) {
            utils::warn("videoSettings: cursor scaling not supported by this engine (system cursors are fixed-size)");
        }
        dirtyCursorScale = 0;
        wrote            = 1;
    }

    if (wrote) {
        utils::settingsWrite();
    }
}

static int toggleFullScreen(void* _);
static int toggleVsync(void* _);
static int uiScaleChange(void* _);
static int cursorScaleChange(void* _);
static int fpsLimitChange(void* _);
static int fpsLimitCheckedChange(void* _);
static int showFpsChange(void* _);
static int videoClose(void* _);

void SettingsVideoGui::added() {
    engine::luaRegisterFunction("toggleFullScreen", toggleFullScreen);
    engine::luaRegisterFunction("toggleVsync", toggleVsync);
    engine::luaRegisterFunction("uiScaleChange", uiScaleChange);
    engine::luaRegisterFunction("cursorScaleChange", cursorScaleChange);
    engine::luaRegisterFunction("fpsLimitChange", fpsLimitChange);
    engine::luaRegisterFunction("fpsLimitCheckedChange", fpsLimitCheckedChange);
    engine::luaRegisterFunction("showFpsChange", showFpsChange);
    engine::luaRegisterFunction("videoClose", videoClose);

    fullScreen      = (char)utils::settingsGetBool("fullScreen");
    vsync           = (char)utils::settingsGetBool("vsync");
    showFps         = (char)utils::settingsGetBool("showFps");
    fpsLimitChecked = (char)utils::settingsGetBool("fpsLimitChecked");
    fpsLimit        = (float)utils::settingsGetDouble("fpsLimit");
    uiScale         = (float)utils::settingsGetDouble("uiScale");
    cursorScale     = (float)utils::settingsGetDouble("cursorScale");

    model    = rmlCreateModel("video");
    rmlBindBool(model, "fullScreen", &fullScreen);
    rmlBindBool(model, "vsync", &vsync);
    rmlBindBool(model, "showFps", &showFps);
    rmlBindBool(model, "fpsLimitChecked", &fpsLimitChecked);
    rmlBindFloat(model, "fpsLimit", &fpsLimit);
    rmlBindFloat(model, "uiScale", &uiScale);
    rmlBindFloat(model, "cursorScale", &cursorScale);

    document = rmlNewDocument("gui/settings/video/video.html");
    rmlLoadDocument(document);
    rmlShowDocument(document);

    // Headless testing: ENGINE_VIDEO_SETTINGS_AUTOTEST=close exercises the
    // real BACK path; =uiscale15 verifies slider -> apply -> persist;
    // =fullscreen verifies the window toggle (the screenshot shows the
    // fullscreen window).
    const char* at = getenv("ENGINE_VIDEO_SETTINGS_AUTOTEST");
    if (at && at[0]) {
        if (utils::strequals(at, "close")) {
            videoClose(nullptr);
        } else if (utils::strequals(at, "uiscale15")) {
            utils::info("videoSettings: autotest — apply uiScale=1.5");
            uiScale      = 1.5f;
            dirtyUiScale = 1;
            lastChange   = 0.0;  // apply on the first update()
        } else if (utils::strequals(at, "fullscreen")) {
            utils::info("videoSettings: autotest — toggle fullscreen on");
            fullScreen      = 1;
            dirtyFullScreen = 1;
            lastChange      = 0.0;
        }
    }
}

void SettingsVideoGui::update() {
    if (model) {
        rmlUpdateDirtyAll(model);
    }
    char any = dirtyFullScreen | dirtyVsync | dirtyShowFps | dirtyFpsChecked |
               dirtyFpsLimit | dirtyUiScale | dirtyCursorScale;
    if (any && utils::millies() > lastChange + 50.0) {
        applyChanges(1);
    }
}

void SettingsVideoGui::removed() {
    // A BACK within the settle window still persists the last change (without
    // engine application — see applyChanges). The old engine's windowResized
    // re-sync of fullScreen is gone with the old signal system: in this engine
    // the toggle is the only writer of the window state, so the setting stays
    // the source of truth.
    applyChanges(0);
    rmlUnloadDocument(document);
    document = nullptr;
    rmlUnloadModel(model);
    model    = nullptr;
}

int toggleFullScreen(void* _) {
    lastChange      = utils::millies();
    dirtyFullScreen = 1;
    return 0;
}

int toggleVsync(void* _) {
    lastChange = utils::millies();
    dirtyVsync = 1;
    return 0;
}

int uiScaleChange(void* _) {
    lastChange   = utils::millies();
    dirtyUiScale = 1;
    return 0;
}

int cursorScaleChange(void* _) {
    lastChange       = utils::millies();
    dirtyCursorScale = 1;
    return 0;
}

int fpsLimitChange(void* _) {
    lastChange    = utils::millies();
    dirtyFpsLimit = 1;
    return 0;
}

int fpsLimitCheckedChange(void* _) {
    lastChange      = utils::millies();
    dirtyFpsChecked = 1;
    return 0;
}

int showFpsChange(void* _) {
    lastChange   = utils::millies();
    dirtyShowFps = 1;
    return 0;
}

// BACK / ESC: re-show the main settings page (synchronously — safe mid-input-
// event, see MainMenuGui::luaPlayGame) and remove this page next frame.
int videoClose(void* _) {
    settingsGuiShow();
    engine::guiManagerRemoveGuiNextFrame(&settingsVideoGui);
    return 0;
}

char settingsVideoGuiIsShowing(void) {
    return document != nullptr;
}
}  // namespace game
