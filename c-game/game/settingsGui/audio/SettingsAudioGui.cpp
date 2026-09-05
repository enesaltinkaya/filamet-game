#include "SettingsAudioGui.h"
#include "../SettingsGui.h"
#include "ecs/system/lua/LuaSystem.h"
#include "ecs/system/sound/SoundSystem.h"
#include "gui/rmlui/GuiManagerRmlUi.h"
#include "Utils.h"

#include "crmlui.h"

#include <cstdlib>

namespace game {

SettingsAudioGui settingsAudioGui;

SettingsAudioGui::SettingsAudioGui() : engine::System("settingsAudioGui") {}

static void* document = nullptr;
static void* model    = nullptr;

// The slider values. The data model binds these (rmlBindFloat): the DOM
// (input range data-value) writes into them on rmlUpdate, and the labels'
// {{format(effects,0)}} read them back.
static float effects = 0.0f;
static float music   = 0.0f;

// The old engine debounced each slider with futureTaskAdd(50, ...); this
// engine never runs utils::futureTaskRun, so it is a plain timestamp flushed
// in update() (the same pattern the old ImGui graphics screen used). The
// onchange handlers only set the flags — the bound floats are synced by the
// manager's rmlUpdate, which runs after update(), and the 50 ms debounce is
// always at least one frame, so the flush reads fresh values.
static double lastChange = 0.0;
static char   dirtyEffects = 0;
static char   dirtyMusic   = 0;

// ENGINE_AUDIO_SETTINGS_AUTOTEST=close, armed in added() and fired from
// update(): closing inside added() would remove the gui in the SAME frame it
// is added (the manager's removes loop runs right after its adds loop), so
// the audio page would never render — and the BACK swap would never be
// exercised.
static char autotestClosePending = 0;

static int effectsChange(void* _);
static int musicChange(void* _);
static int audioSettingsClose(void* _);

// Persist the pending changes: settings key + (while the audio stack is up)
// the matching feedback sound (the old engine's effectsChangeLater /
// musicChangeLater), one settingsWrite at the end. soundFeedback is 0 from
// removed(): at shutdown the gui manager is removed AFTER the sound system
// (ecs priority order), so the click must not play on a destroyed SoLoud —
// persisting the last change is still fine (the settings module is plain
// c-utils and has no teardown).
static void flush(char soundFeedback) {
    char wrote = 0;
    if (dirtyEffects) {
        utils::settingsSetDouble("effects", effects);
        if (soundFeedback) engine::soundPlayClick();
        dirtyEffects = 0;
        wrote        = 1;
    }
    if (dirtyMusic) {
        utils::settingsSetDouble("music", music);
        if (soundFeedback) engine::soundPlayClickOnMusicLevel();
        dirtyMusic = 0;
        wrote      = 1;
    }
    if (wrote) {
        utils::settingsWrite();
    }
}

void SettingsAudioGui::added() {
    engine::luaRegisterFunction("effectsChange", effectsChange);
    engine::luaRegisterFunction("musicChange", musicChange);
    engine::luaRegisterFunction("audioSettingsClose", audioSettingsClose);

    effects = (float)utils::settingsGetDouble("effects");
    music   = (float)utils::settingsGetDouble("music");

    model    = rmlCreateModel("audio");
    rmlBindFloat(model, "effects", &effects);
    rmlBindFloat(model, "music", &music);

    document = rmlNewDocument("gui/settings/audio/audio.html");
    rmlLoadDocument(document);
    rmlShowDocument(document);

    // Atomic swap point: the manager applies this add (its adds loop) before
    // the frame's render, so hiding the main settings page here lands both
    // changes on the same rendered frame — the click frame still shows the
    // main page, this frame shows the audio page; never both, never blank.
    if (settingsGuiIsShowing()) settingsGuiHide();

    // Headless testing: ENGINE_AUDIO_SETTINGS_AUTOTEST=effects50 applies a
    // slider change programmatically (bind -> debounce -> settings.json),
    // =close exercises the real BACK path (audioSettingsClose: re-shows the
    // settings page + deferred remove).
    const char* at = getenv("ENGINE_AUDIO_SETTINGS_AUTOTEST");
    if (at && at[0]) {
        if (utils::strequals(at, "close")) {
            autotestClosePending = 1;
        } else if (utils::strequals(at, "effects50")) {
            utils::info("audioSettings: autotest — persist effects=50");
            effects      = 50.0f;
            dirtyEffects = 1;
            lastChange   = 0.0;  // flush on the first update()
        }
    }
}

void SettingsAudioGui::update() {
    if (autotestClosePending && document) {
        autotestClosePending = 0;
        audioSettingsClose(nullptr);
    }
    if (model) {
        rmlUpdateDirtyAll(model);
    }
    if ((dirtyEffects || dirtyMusic) && utils::millies() > lastChange + 50.0) {
        flush(1);
    }
}

void SettingsAudioGui::removed() {
    autotestClosePending = 0;
    flush(0);  // a BACK within the debounce window still persists the last change (no click: the sound stack may already be gone at shutdown)
    rmlUnloadDocument(document);
    document = nullptr;
    // Atomic swap point (BACK / ESC): re-show the main settings page on this
    // same frame our document is unloaded, so the user never sees both pages
    // (the old synchronous settingsGuiShow() in audioSettingsClose left a
    // both-visible frame) nor a blank one. Guarded: during a state transition
    // settings may already be torn down (settingsGuiShow is a no-op then).
    if (settingsGuiIsShowing()) settingsGuiShow();
    rmlUnloadModel(model);
    model    = nullptr;
}

int effectsChange(void* _) {
    lastChange   = utils::millies();
    dirtyEffects = 1;
    return 0;
}

int musicChange(void* _) {
    lastChange = utils::millies();
    dirtyMusic = 1;
    return 0;
}

// BACK / ESC: queue this page's removal. The manager applies it next frame
// and removed() re-shows the main settings page on that same frame (atomic
// swap — the old synchronous settingsGuiShow() here left a both-visible
// frame). The old engine did this with futureTask(0, settingsGuiShow) +
// deferred remove.
int audioSettingsClose(void* _) {
    engine::guiManagerRemoveGuiNextFrame(&settingsAudioGui);
    return 0;
}

char settingsAudioGuiIsShowing(void) {
    return document != nullptr;
}
}  // namespace game
