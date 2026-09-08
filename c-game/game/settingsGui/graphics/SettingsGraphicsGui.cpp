#include "SettingsGraphicsGui.h"
#include "../SettingsGui.h"
#include "ecs/system/lua/LuaSystem.h"
#include "gui/rmlui/GuiManagerRmlUi.h"
#include "renderer/Renderer.h"
#include "Utils.h"

#include "crmlui.h"

#include <cstdio>
#include <cstdlib>

namespace game {
SettingsGraphicsGui settingsGraphicsGui;

SettingsGraphicsGui::SettingsGraphicsGui() : engine::System("settingsGraphicsGui") {}

static void* document = nullptr;
static void* model    = nullptr;

// Page state, seeded in added() from data/settings.json — the same keys
// rendererGraphicsLoad (c-engine/renderer/Renderer.cpp) maps to
// renderer::GraphicsSettings at startup; keep the two mappings in sync.
static char  taaEnabled            = 0;
static float taaWeightPercent      = 90.0f;  // TAA temporal blend weight, % (50..95)
static float casStrengthPercent    = 100.0f; // RCAS sharpening, % (0..150; 100 = AMD max)
static float renderScalePercent    = 100.0f;  // render resolution scale, % (50..200)
static int   shadowsMode           = 1;       // PCF
static int   shadowsQuality        = 2;       // Medium
static char  shadowQualityDisabled = 0;
static char  aoEnabled             = 1;
static float ssaoRadius            = 1.0f;   // AO radius 0.1..10 (world-space)
static int   ssaoAlgorithm         = 0;      // 0=GTAO 1=HBAO 2=VBAO
static float ssaoIntensity         = 1.0f;   // AO composite strength 0..2
static char  giEnabled             = 1;
static char  bloomEnabled          = 1;

static const char* shadowModeNames[] = {
    "Off",
    "PCF",
    "VSM",
    "EVSM2",
    "EVSM4",
};
static const char* shadowQualityNames[] = {
    "Low",
    "Medium",
    "High",
};
static const char* ssaoAlgorithmNames[] = {
    "GTAO",
    "HBAO",
    "VBAO",
};

static char* shadowsLabel;
static char* shadowQualityLabel;
static char* aoLabel;
static char* ssaoAlgorithmLabel;
static char* giLabel;
static char* bloomLabel;
static char* taaLabel;
static char shadowsLabelText[16];
static char shadowQualityLabelText[16];
static char aoLabelText[16];
static char ssaoAlgorithmLabelText[16];
static char giLabelText[16];
static char bloomLabelText[16];
static char taaLabelText[16];

static void syncLabels(void);
static int renderScaleChange(void* _);
static int taaWeightChange(void* _);
static int casStrengthChange(void* _);
static int ssaoRadiusChange(void* _);
static int ssaoIntensityChange(void* _);
static int graphicsClose(void* _);
static int toggleShadows(void* _);
static int toggleShadowsPrev(void* _);
static int toggleShadowQuality(void* _);
static int toggleShadowQualityPrev(void* _);
static int toggleAo(void* _);
static int toggleSsaoAlgorithm(void* _);
static int toggleSsaoAlgorithmPrev(void* _);
static int toggleGi(void* _);
static int toggleBloom(void* _);
static int toggleTaa(void* _);

// ── renderer + persistence plumbing ─────────────────────────────────────────

// Push the page state to the live renderer. rendererGraphicsApply normalizes
// (clamps/snaps) and stores the applied copy, so the page and the startup
// load always agree on the effective values. Fields the page no longer has
// controls for (fog, vignette) keep the renderer's current values.
static void applyRenderer(void) {
    auto g          = engine::renderer::rendererGraphicsSettings();
    g.taa           = taaEnabled != 0;
    g.taaWeight     = taaWeightPercent / 100.0f;
    g.casStrength   = casStrengthPercent / 100.0f;
    g.renderScale   = renderScalePercent / 100.0f;
    g.shadowMode    = shadowsMode;
    g.shadowQuality = shadowsQuality;
    g.ssao          = aoEnabled != 0;
    g.ssaoRadius    = ssaoRadius;
    g.ssaoAlgorithm = ssaoAlgorithm;
    g.ssaoIntensity = ssaoIntensity;
    g.bloom         = bloomEnabled != 0;
    engine::renderer::rendererGraphicsApply(g);
}

// Persist one key + write the file. Every control owns its settings.json key
// (the *Disabled booleans are the persisted polarity of their toggle). Three
// distinct names instead of overloads: bool arguments promote to int, so an
// overloaded set silently routed toggles into settingsSetInt's assert (and a
// release build would have written mistyped keys that trip the settings
// file validation into rewriting all defaults).
static void persistBool(const char* key, bool value) {
    utils::settingsSetBool(key, value);
    utils::settingsWrite();
}
static void persistInt(const char* key, int value) {
    utils::settingsSetInt(key, value);
    utils::settingsWrite();
}
static void persistDouble(const char* key, double value) {
    utils::settingsSetDouble(key, value);
    utils::settingsWrite();
}

// ── sliders: defer apply+persist until the value is readable and settled ────
//
// RMLUI writes the fresh slider value back into the bound float only WHILE
// processing the 'change' event — the handler runs before that write (the
// old engine's persistAASettings comment; the video page has the same
// deferral). A change handler therefore only marks its control dirty;
// update() applies + persists once the slider has settled (50 ms), and
// removed() flushes a pending change on BACK.
static double lastChange = 0.0;
static char   dirtyScale = 0;
static char   dirtyAA    = 0;
static char   dirtySsao  = 0;

static void markSliderDirty(char* dirty) {
    *dirty     = 1;
    lastChange = utils::millies();
}

static void applySliderChanges(void) {
    if (!(dirtyScale | dirtyAA | dirtySsao)) {
        return;
    }
    dirtyScale = dirtyAA = dirtySsao = 0;

    applyRenderer();
    persistDouble("renderScale", renderScalePercent / 100.0);
    persistDouble("taaWeight", taaWeightPercent / 100.0);
    persistDouble("casStrength", casStrengthPercent / 100.0);
    persistDouble("ssaoRadius", ssaoRadius);
    persistDouble("ssaoIntensity", ssaoIntensity);

    if (model) {
        rmlUpdateDirtyAll(model);
    }
}

// Headless testing hook (=wire): close via the real BACK path once the
// deferred slider changes have been applied (see added()).
static char autotestWireClose = 0;

void SettingsGraphicsGui::added() {
    engine::luaRegisterFunction("renderScaleChange", renderScaleChange);
    engine::luaRegisterFunction("taaWeightChange", taaWeightChange);
    engine::luaRegisterFunction("casStrengthChange", casStrengthChange);
    engine::luaRegisterFunction("graphicsClose", graphicsClose);
    engine::luaRegisterFunction("toggleShadows", toggleShadows);
    engine::luaRegisterFunction("toggleShadowsPrev", toggleShadowsPrev);
    engine::luaRegisterFunction("toggleShadowQuality", toggleShadowQuality);
    engine::luaRegisterFunction("toggleShadowQualityPrev", toggleShadowQualityPrev);
    engine::luaRegisterFunction("toggleAo", toggleAo);
    engine::luaRegisterFunction("toggleSsaoAlgorithm", toggleSsaoAlgorithm);
    engine::luaRegisterFunction("toggleSsaoAlgorithmPrev", toggleSsaoAlgorithmPrev);
    engine::luaRegisterFunction("ssaoRadiusChange", ssaoRadiusChange);
    engine::luaRegisterFunction("ssaoIntensityChange", ssaoIntensityChange);
    engine::luaRegisterFunction("toggleGi", toggleGi);
    engine::luaRegisterFunction("toggleBloom", toggleBloom);
    engine::luaRegisterFunction("toggleTaa", toggleTaa);

    // Seed from the persisted settings (not the renderer's normalized block)
    // so the page reflects the file both agree on; changes made via the debug
    // GUI write the same keys.
    taaEnabled           = (char)utils::settingsGetBool("taaEnabled");
    taaWeightPercent     = (float)(utils::settingsGetDouble("taaWeight") * 100.0);
    casStrengthPercent   = (float)(utils::settingsGetDouble("casStrength") * 100.0);
    renderScalePercent   = (float)(utils::settingsGetDouble("renderScale") * 100.0);
    shadowsMode          = utils::settingsGetInt("shadowMode");
    shadowsQuality       = utils::settingsGetInt("shadowQuality");
    aoEnabled            = (char)!utils::settingsGetBool("aoDisabled");
    ssaoRadius           = (float)utils::settingsGetDouble("ssaoRadius");
    ssaoAlgorithm        = utils::settingsGetInt("ssaoAlgorithm");
    ssaoIntensity        = (float)utils::settingsGetDouble("ssaoIntensity");
    giEnabled            = (char)!utils::settingsGetBool("giDisabled");
    bloomEnabled         = (char)!utils::settingsGetBool("bloomDisabled");
    // clamp hand-edited files (the renderer re-clamps its own copy on apply)
    if (shadowsMode < 0 || shadowsMode > 4) shadowsMode = 1;
    if (shadowsQuality < 0 || shadowsQuality > 2) shadowsQuality = 2;
    if (ssaoAlgorithm < 0 || ssaoAlgorithm > 2) ssaoAlgorithm = 0;
    shadowQualityDisabled = shadowsMode == 0;
    syncLabels();

    model = rmlCreateModel("graphics");
    rmlBindFloat(model, "renderScalePercent", &renderScalePercent);
    rmlBindFloat(model, "taaWeightPercent", &taaWeightPercent);
    rmlBindFloat(model, "casStrengthPercent", &casStrengthPercent);
    rmlBindFloat(model, "ssaoRadius", &ssaoRadius);
    rmlBindFloat(model, "ssaoIntensity", &ssaoIntensity);
    rmlBind(model, "shadowsLabel", &shadowsLabel);
    rmlBind(model, "shadowQualityLabel", &shadowQualityLabel);
    rmlBindBool(model, "shadowQualityDisabled", &shadowQualityDisabled);
    rmlBind(model, "aoLabel", &aoLabel);
    rmlBind(model, "ssaoAlgorithmLabel", &ssaoAlgorithmLabel);
    rmlBind(model, "giLabel", &giLabel);
    rmlBind(model, "bloomLabel", &bloomLabel);
    rmlBind(model, "taaLabel", &taaLabel);

    document = rmlNewDocument("gui/settings/graphics/graphics.html");
    rmlLoadDocument(document);
    rmlShowDocument(document);

    // Atomic swap point: the manager applies this add (its adds loop) before
    // the frame's render, so hiding the main settings page here lands both
    // changes on the same rendered frame — click frame shows the main page,
    // this frame shows the graphics page; never both, never blank.
    if (settingsGuiIsShowing()) settingsGuiHide();

    // Headless testing: ENGINE_GRAPHICS_SETTINGS_AUTOTEST=close exercises the
    // real BACK path (settingsGuiShow + deferred remove); =wire flips a set
    // of toggles (immediate path) and sets the sliders (deferred path), lets
    // update() apply them, then closes — check data/settings.json afterwards.
    const char* at = getenv("ENGINE_GRAPHICS_SETTINGS_AUTOTEST");
    if (at && at[0]) {
        if (utils::strequals(at, "close")) {
            graphicsClose(nullptr);
        } else if (utils::strequals(at, "wire")) {
            while (taaEnabled) toggleTaa(nullptr);
            while (shadowsMode != 1) toggleShadows(nullptr);
            while (shadowsQuality != 1) toggleShadowQuality(nullptr);
            while (bloomEnabled) toggleBloom(nullptr);
            renderScalePercent  = 130.0f;
            taaWeightPercent    = 85.0f;
            casStrengthPercent  = 75.0f;
            dirtyScale = dirtyAA = 1;
            lastChange        = 0.0;  // settle on the first update()
            autotestWireClose = 1;
        }
    }
}

void SettingsGraphicsGui::update() {
    if (model) {
        rmlUpdateDirtyAll(model);
    }
    // Slider settle: apply + persist 50 ms after the last change — always at
    // least one frame after the 'change' event, i.e. after RMLUI wrote the
    // fresh values into the bound floats.
    if ((dirtyScale | dirtyAA | dirtySsao) && utils::millies() > lastChange + 50.0) {
        applySliderChanges();
    }
    if (autotestWireClose && !(dirtyScale | dirtyAA | dirtySsao)) {
        autotestWireClose = 0;
        graphicsClose(nullptr);
    }
}

void SettingsGraphicsGui::removed() {
    // A BACK within the settle window still persists + applies the last
    // slider change (unlike the video page, no toEngine=0 split is needed:
    // rendererGraphicsApply guards on the live backend and settings are
    // plain c-utils — both safe after partial teardown, docs/lessons.md
    // removed()-ordering entry).
    applySliderChanges();
    rmlUnloadDocument(document);
    document = nullptr;
    // Atomic swap point (BACK / ESC): re-show the main settings page on this
    // same frame our document is unloaded — never both pages, never blank.
    // Guarded: a state transition may have torn settings down already (no-op).
    if (settingsGuiIsShowing()) settingsGuiShow();
    rmlUnloadModel(model);
    model    = nullptr;
}

// Recompute every bound label from the page state.
static void syncLabels(void) {
    snprintf(taaLabelText, sizeof(taaLabelText), "%s", taaEnabled ? "On" : "Off");
    taaLabel = taaLabelText;

    snprintf(shadowsLabelText, sizeof(shadowsLabelText), "%s",
             shadowModeNames[shadowsMode]);
    shadowsLabel = shadowsLabelText;
    snprintf(shadowQualityLabelText, sizeof(shadowQualityLabelText), "%s",
             shadowQualityNames[shadowsQuality]);
    shadowQualityLabel = shadowQualityLabelText;
    snprintf(aoLabelText, sizeof(aoLabelText), "%s", aoEnabled ? "On" : "Off");
    aoLabel = aoLabelText;
    snprintf(ssaoAlgorithmLabelText, sizeof(ssaoAlgorithmLabelText), "%s",
             ssaoAlgorithmNames[ssaoAlgorithm]);
    ssaoAlgorithmLabel = ssaoAlgorithmLabelText;
    snprintf(giLabelText, sizeof(giLabelText), "%s", giEnabled ? "On" : "Off");
    giLabel = giLabelText;
    snprintf(bloomLabelText, sizeof(bloomLabelText), "%s", bloomEnabled ? "On" : "Off");
    bloomLabel = bloomLabelText;
}

// ── toggles: the handler computes the new value — apply + persist right away ─

static int toggleTaa(void* _) {
    taaEnabled = !taaEnabled;
    applyRenderer();
    persistBool("taaEnabled", taaEnabled != 0);
    syncLabels();
    rmlUpdateDirtyAll(model);
    return 0;
}

static void shadowsModeApply(int mode) {
    shadowsMode = mode;
    shadowQualityDisabled = shadowsMode == 0;
    applyRenderer();
    persistInt("shadowMode", shadowsMode);
    // Legacy on/off key, kept in sync (read once at startup for migration,
    // see Settings.cpp).
    utils::settingsSetBool("shadowsDisabled", shadowsMode == 0);
    utils::settingsWrite();
    syncLabels();
    rmlUpdateDirtyAll(model);
}

// Cycle the filtering mode: off -> PCF -> VSM -> EVSM2 -> EVSM4 -> off.
static int toggleShadows(void* _) {
    int count = (int)(sizeof(shadowModeNames) / sizeof(shadowModeNames[0]));
    shadowsModeApply((shadowsMode + 1) % count);
    return 0;
}

static int toggleShadowsPrev(void* _) {
    int count = (int)(sizeof(shadowModeNames) / sizeof(shadowModeNames[0]));
    shadowsModeApply((shadowsMode + count - 1) % count);
    return 0;
}

static void shadowsQualityApply(int quality) {
    shadowsQuality = quality;
    applyRenderer();
    persistInt("shadowQuality", shadowsQuality);
    utils::settingsWrite();
    syncLabels();
    rmlUpdateDirtyAll(model);
}

// Cycle the quality tier: low -> medium -> high -> low.
static int toggleShadowQuality(void* _) {
    int count = (int)(sizeof(shadowQualityNames) / sizeof(shadowQualityNames[0]));
    shadowsQualityApply((shadowsQuality + 1) % count);
    return 0;
}

static int toggleShadowQualityPrev(void* _) {
    int count = (int)(sizeof(shadowQualityNames) / sizeof(shadowQualityNames[0]));
    shadowsQualityApply((shadowsQuality + count - 1) % count);
    return 0;
}

static int toggleAo(void* _) {
    aoEnabled = !aoEnabled;
    applyRenderer();
    persistBool("aoDisabled", aoEnabled == 0);
    syncLabels();
    rmlUpdateDirtyAll(model);
    return 0;
}

static void ssaoAlgorithmApply(int algorithm) {
    ssaoAlgorithm = algorithm;
    applyRenderer();
    persistInt("ssaoAlgorithm", ssaoAlgorithm);
    syncLabels();
    rmlUpdateDirtyAll(model);
}

// Cycle the AO method: GTAO -> HBAO -> VBAO -> GTAO (DiligentFX Algorithm enum).
static int toggleSsaoAlgorithm(void* _) {
    int count = (int)(sizeof(ssaoAlgorithmNames) / sizeof(ssaoAlgorithmNames[0]));
    ssaoAlgorithmApply((ssaoAlgorithm + 1) % count);
    return 0;
}

static int toggleSsaoAlgorithmPrev(void* _) {
    int count = (int)(sizeof(ssaoAlgorithmNames) / sizeof(ssaoAlgorithmNames[0]));
    ssaoAlgorithmApply((ssaoAlgorithm + count - 1) % count);
    return 0;
}

static int toggleGi(void* _) {
    giEnabled = !giEnabled;
    applyRenderer();
    persistBool("giDisabled", giEnabled == 0);
    syncLabels();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int toggleBloom(void* _) {
    bloomEnabled = !bloomEnabled;
    applyRenderer();
    persistBool("bloomDisabled", bloomEnabled == 0);
    syncLabels();
    rmlUpdateDirtyAll(model);
    return 0;
}

int renderScaleChange(void* _) {
    markSliderDirty(&dirtyScale);
    return 0;
}

int taaWeightChange(void* _) {
    markSliderDirty(&dirtyAA);
    return 0;
}

int casStrengthChange(void* _) {
    markSliderDirty(&dirtyAA);
    return 0;
}

int ssaoRadiusChange(void* _) {
    markSliderDirty(&dirtySsao);
    return 0;
}

int ssaoIntensityChange(void* _) {
    markSliderDirty(&dirtySsao);
    return 0;
}

// BACK / ESC: queue this page's removal. The manager applies it next frame
// and removed() re-shows the main settings page on that same frame (atomic
// swap — the old synchronous settingsGuiShow() here left a both-visible
// frame). The old engine did this with futureTask(0, settingsGuiShow) +
// deferred remove.
int graphicsClose(void* _) {
    engine::guiManagerRemoveGuiNextFrame(&settingsGraphicsGui);
    return 0;
}

char settingsGraphicsGuiIsShowing(void) {
    return document != nullptr;
}
}
