#include "SettingsGui.h"
#include "audio/SettingsAudioGui.h"
#include "graphics/SettingsGraphicsGui.h"
#include "video/SettingsVideoGui.h"
#include "ecs/system/lua/LuaSystem.h"
#include "gui/rmlui/GuiManagerRmlUi.h"
#include "Utils.h"

#include "crmlui.h"

#include <cstdlib>

namespace game {

SettingsGui settingsGui;

SettingsGui::SettingsGui() : engine::System("settingsGui") {}

static void* document = nullptr;

static int settingsClose(void* _);
static int showAudioSettings(void* _);
static int showVideoSettings(void* _);
static int showGraphicsSettings(void* _);

// ENGINE_SETTINGS_AUTOTEST=close, armed in added() and fired from update():
// a remove queued inside added() would be applied by the manager in the
// SAME frame (the removes loop runs after the adds loop), so the gui would
// never actually show — the opener menu's hide/re-show tracking would not
// reach its "settings is up" state and never re-opened its document.
// One real "showing" frame keeps the cycle honest.
static char autotestClosePending = 0;

// Null-safe: called from the sub-pages' added()/removed() atomic-swap
// points (see showAudioSettings) — a sub-page removed during a state
// transition (settings already torn down) must be a no-op, not a crash.
void settingsGuiHide(void) {
    if (document) rmlHideDocument(document);
}

void settingsGuiShow(void) {
    if (document) rmlShowDocument(document);
}

void SettingsGui::added() {
    document = rmlNewDocument("gui/settings/settings.html");
    rmlLoadDocument(document);
    rmlShowDocument(document);

    // onclick handlers in settings.html. settingsKeyDown (body onkeydown:
    // ESC closes) lives in settings.lua itself.
    engine::luaRegisterFunction("settingsClose", settingsClose);
    engine::luaRegisterFunction("showAudioSettings", showAudioSettings);
    engine::luaRegisterFunction("showVideoSettings", showVideoSettings);
    engine::luaRegisterFunction("showGraphicsSettings", showGraphicsSettings);

    // Headless testing: ENGINE_SETTINGS_AUTOTEST=close verifies the whole
    // add -> document -> remove cycle without an input device (the open side
    // is covered by ENGINE_AUTOTEST=settings + a screenshot run);
    // =audio / =video jump straight to the sub-page (for its screenshot / apply
    // runs, see ENGINE_AUDIO_SETTINGS_AUTOTEST / ENGINE_VIDEO_SETTINGS_AUTOTEST).
    if (getenv("ENGINE_SETTINGS_AUTOTEST")) {
        if (utils::strequals(getenv("ENGINE_SETTINGS_AUTOTEST"), "close")) {
            autotestClosePending = 1;
        } else if (utils::strequals(getenv("ENGINE_SETTINGS_AUTOTEST"), "audio")) {
            showAudioSettings(nullptr);
        } else if (utils::strequals(getenv("ENGINE_SETTINGS_AUTOTEST"), "video")) {
            showVideoSettings(nullptr);
        } else if (utils::strequals(getenv("ENGINE_SETTINGS_AUTOTEST"), "graphics")) {
            showGraphicsSettings(nullptr);
        }
    }
}

void SettingsGui::removed() {
    autotestClosePending = 0;
    rmlUnloadDocument(document);
    document = nullptr;
}

void SettingsGui::update() {
    if (autotestClosePending) {
        autotestClosePending = 0;
        engine::guiManagerRemoveGuiNextFrame(&settingsGui);
    }
}

int settingsClose(void* _) {
    engine::guiManagerRemoveGuiNextFrame(&settingsGui);
    return 0;
}

// The sub-menus REPLACE the main settings page, with an atomic swap: this
// click only queues the sub-page add; the manager applies it next frame, and
// the sub-page's added() hides this document on that same frame (no frame
// with both pages visible, no blank frame — the old synchronous
// settingsGuiHide() here left a blank frame, and the old synchronous
// settingsGuiShow() on BACK left a both-visible frame). BACK on the sub-page
// queues its remove; its removed() re-shows this document on that same frame.
int showAudioSettings(void* _) {
    engine::guiManagerAddGuiNextFrame(&settingsAudioGui);
    return 0;
}

int showVideoSettings(void* _) {
    engine::guiManagerAddGuiNextFrame(&settingsVideoGui);
    return 0;
}

int showGraphicsSettings(void* _) {
    engine::guiManagerAddGuiNextFrame(&settingsGraphicsGui);
    return 0;
}

char settingsGuiIsShowing(void) {
    return document != nullptr;
}
}  // namespace game
