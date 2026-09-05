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

void settingsGuiHide(void) {
    rmlHideDocument(document);
}

void settingsGuiShow(void) {
    rmlShowDocument(document);
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
            engine::guiManagerRemoveGuiNextFrame(&settingsGui);
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
    rmlUnloadDocument(document);
    document = nullptr;
}

int settingsClose(void* _) {
    engine::guiManagerRemoveGuiNextFrame(&settingsGui);
    return 0;
}

// The sub-menus open OVER the main settings page: hide its document and add
// the sub-page gui (the old engine deferred the hide by one frame via
// futureTask; here it is synchronous — the click handler runs inside the
// manager's input phase, and rmlHideDocument mid-event is safe, see
// MainMenuGui::luaPlayGame). BACK on the sub-page re-shows this document.
int showAudioSettings(void* _) {
    settingsGuiHide();
    engine::guiManagerAddGuiNextFrame(&settingsAudioGui);
    return 0;
}

int showVideoSettings(void* _) {
    settingsGuiHide();
    engine::guiManagerAddGuiNextFrame(&settingsVideoGui);
    return 0;
}

int showGraphicsSettings(void* _) {
    settingsGuiHide();
    engine::guiManagerAddGuiNextFrame(&settingsGraphicsGui);
    return 0;
}

char settingsGuiIsShowing(void) {
    return document != nullptr;
}
}  // namespace game
