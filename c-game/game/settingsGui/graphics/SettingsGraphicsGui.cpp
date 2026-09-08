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
static char  ssrEnabled            = 0;
static char  aoEnabled             = 1;
static char  giEnabled             = 1;
static char  bloomEnabled          = 1;
static char  lensEnabled           = 1;
static char  lensParamsDisabled    = 0;
static float lensGrainPercent      = 0.0f;
static float lensChromAbPercent    = 0.0f;
static float lensVignettePercent   = 0.0f;
static char  dofEnabled            = 0;
static char  dofParamsDisabled     = 1;
static float dofQuality            = 1.0f;
static int   fogMode               = 1;       // Fog

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
static const char* fogModeNames[] = {
    "Off",
    "Fog",
};

static char* shadowsLabel;
static char* shadowQualityLabel;
static char* ssrLabel;
static char* aoLabel;
static char* giLabel;
static char* bloomLabel;
static char* lensLabel;
static char* dofLabel;
static char* fogLabel;
static char* taaLabel;
static char shadowsLabelText[16];
static char shadowQualityLabelText[16];
static char ssrLabelText[16];
static char aoLabelText[16];
static char giLabelText[16];
static char bloomLabelText[16];
static char lensLabelText[16];
static char dofLabelText[16];
static char fogLabelText[16];
static char taaLabelText[16];

static void syncLabels(void);
static int renderScaleChange(void* _);
static int taaWeightChange(void* _);
static int casStrengthChange(void* _);
static int toggleLens(void* _);
static int lensParamChange(void* _);
static int toggleDof(void* _);
static int dofParamChange(void* _);
static int graphicsClose(void* _);
static int toggleShadows(void* _);
static int toggleShadowsPrev(void* _);
static int toggleShadowQuality(void* _);
static int toggleShadowQualityPrev(void* _);
static int toggleSsr(void* _);
static int toggleAo(void* _);
static int toggleGi(void* _);
static int toggleBloom(void* _);
static int toggleFog(void* _);
static int toggleTaa(void* _);

// ── renderer + persistence plumbing ─────────────────────────────────────────

// Push the page state to the live renderer. rendererGraphicsApply normalizes
// (clamps/snaps) and stores the applied copy, so the page and the startup
// load always agree on the effective values. Backends ignore fields they
// have no equivalent for yet (RenderBackend::applyGraphicsSettings) — until
// a pass lands, its toggle is apply+persist only. lensGrain/lensChromAb have
// no GraphicsSettings field at all: persist-only, like the old engine's
// grain/CA (only the vignette rides the settings block).
static void applyRenderer(void) {
    auto g          = engine::renderer::rendererGraphicsSettings();
    g.taa           = taaEnabled != 0;
    g.taaWeight     = taaWeightPercent / 100.0f;
    g.casStrength   = casStrengthPercent / 100.0f;
    g.renderScale   = renderScalePercent / 100.0f;
    g.shadowMode    = shadowsMode;
    g.shadowQuality = shadowsQuality;
    g.ssr           = ssrEnabled != 0;
    g.ssao          = aoEnabled != 0;
    g.bloom         = bloomEnabled != 0;
    g.vignette      = lensVignettePercent / 100.0f;
    g.dof           = dofEnabled != 0;
    g.dofQuality    = (int)dofQuality;
    g.fog           = fogMode != 0;
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
static char   dirtyLens  = 0;
static char   dirtyDof   = 0;

static void markSliderDirty(char* dirty) {
    *dirty     = 1;
    lastChange = utils::millies();
}

static void applySliderChanges(void) {
    if (!(dirtyScale | dirtyAA | dirtyLens | dirtyDof)) {
        return;
    }
    dirtyScale = dirtyAA = dirtyLens = dirtyDof = 0;

    applyRenderer();
    persistDouble("renderScale", renderScalePercent / 100.0);
    persistDouble("taaWeight", taaWeightPercent / 100.0);
    persistDouble("casStrength", casStrengthPercent / 100.0);
    persistDouble("lensGrain", lensGrainPercent);
    persistDouble("lensChromAb", lensChromAbPercent);
    persistDouble("lensVignette", lensVignettePercent);
    persistDouble("dofQuality", dofQuality);

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
    engine::luaRegisterFunction("toggleSsr", toggleSsr);
    engine::luaRegisterFunction("toggleAo", toggleAo);
    engine::luaRegisterFunction("toggleGi", toggleGi);
    engine::luaRegisterFunction("toggleBloom", toggleBloom);
    engine::luaRegisterFunction("toggleLens", toggleLens);
    engine::luaRegisterFunction("lensParamChange", lensParamChange);
    engine::luaRegisterFunction("toggleDof", toggleDof);
    engine::luaRegisterFunction("dofParamChange", dofParamChange);
    engine::luaRegisterFunction("toggleFog", toggleFog);
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
    ssrEnabled           = (char)!utils::settingsGetBool("ssrDisabled");
    aoEnabled            = (char)!utils::settingsGetBool("aoDisabled");
    giEnabled            = (char)!utils::settingsGetBool("giDisabled");
    bloomEnabled         = (char)!utils::settingsGetBool("bloomDisabled");
    fogMode              = (int)utils::settingsGetDouble("fogMode");
    lensEnabled          = (char)utils::settingsGetBool("lensEnabled");
    lensGrainPercent     = (float)utils::settingsGetDouble("lensGrain");
    lensChromAbPercent   = (float)utils::settingsGetDouble("lensChromAb");
    lensVignettePercent  = (float)utils::settingsGetDouble("lensVignette");
    dofEnabled           = (char)utils::settingsGetBool("dofEnabled");
    dofQuality           = (float)utils::settingsGetDouble("dofQuality");
    lensParamsDisabled   = !lensEnabled;
    dofParamsDisabled    = !dofEnabled;
    // clamp hand-edited files (the renderer re-clamps its own copy on apply)
    if (shadowsMode < 0 || shadowsMode > 4) shadowsMode = 1;
    if (shadowsQuality < 0 || shadowsQuality > 2) shadowsQuality = 2;
    shadowQualityDisabled = shadowsMode == 0;
    if (fogMode < 0 || fogMode > 1) fogMode = 1;
    syncLabels();

    model = rmlCreateModel("graphics");
    rmlBindFloat(model, "renderScalePercent", &renderScalePercent);
    rmlBindFloat(model, "taaWeightPercent", &taaWeightPercent);
    rmlBindFloat(model, "casStrengthPercent", &casStrengthPercent);
    rmlBindBool(model, "lensParamsDisabled", &lensParamsDisabled);
    rmlBindFloat(model, "lensGrainPercent", &lensGrainPercent);
    rmlBindFloat(model, "lensChromAbPercent", &lensChromAbPercent);
    rmlBindFloat(model, "lensVignettePercent", &lensVignettePercent);
    rmlBindBool(model, "dofParamsDisabled", &dofParamsDisabled);
    rmlBindFloat(model, "dofQuality", &dofQuality);
    rmlBind(model, "shadowsLabel", &shadowsLabel);
    rmlBind(model, "shadowQualityLabel", &shadowQualityLabel);
    rmlBindBool(model, "shadowQualityDisabled", &shadowQualityDisabled);
    rmlBind(model, "ssrLabel", &ssrLabel);
    rmlBind(model, "aoLabel", &aoLabel);
    rmlBind(model, "giLabel", &giLabel);
    rmlBind(model, "bloomLabel", &bloomLabel);
    rmlBind(model, "lensLabel", &lensLabel);
    rmlBind(model, "dofLabel", &dofLabel);
    rmlBind(model, "fogLabel", &fogLabel);
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
            while (ssrEnabled) toggleSsr(nullptr);
            while (bloomEnabled) toggleBloom(nullptr);
            while (fogMode != 0) toggleFog(nullptr);
            renderScalePercent  = 130.0f;
            taaWeightPercent    = 85.0f;
            casStrengthPercent  = 75.0f;
            lensGrainPercent    = 33.0f;
            lensChromAbPercent  = 44.0f;
            lensVignettePercent = 55.0f;
            dofQuality          = 6.0f;
            dirtyScale = dirtyAA = dirtyLens = dirtyDof = 1;
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
    if ((dirtyScale | dirtyAA | dirtyLens | dirtyDof) && utils::millies() > lastChange + 50.0) {
        applySliderChanges();
    }
    if (autotestWireClose && !(dirtyScale | dirtyAA | dirtyLens | dirtyDof)) {
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
    snprintf(ssrLabelText, sizeof(ssrLabelText), "%s", ssrEnabled ? "On" : "Off");
    ssrLabel = ssrLabelText;
    snprintf(aoLabelText, sizeof(aoLabelText), "%s", aoEnabled ? "On" : "Off");
    aoLabel = aoLabelText;
    snprintf(giLabelText, sizeof(giLabelText), "%s", giEnabled ? "On" : "Off");
    giLabel = giLabelText;
    snprintf(bloomLabelText, sizeof(bloomLabelText), "%s", bloomEnabled ? "On" : "Off");
    bloomLabel = bloomLabelText;
    snprintf(lensLabelText, sizeof(lensLabelText), "%s", lensEnabled ? "On" : "Off");
    lensLabel = lensLabelText;
    snprintf(dofLabelText, sizeof(dofLabelText), "%s", dofEnabled ? "On" : "Off");
    dofLabel = dofLabelText;
    snprintf(fogLabelText, sizeof(fogLabelText), "%s", fogModeNames[fogMode]);
    fogLabel = fogLabelText;
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

static int toggleSsr(void* _) {
    ssrEnabled = !ssrEnabled;
    applyRenderer();
    persistBool("ssrDisabled", ssrEnabled == 0);
    syncLabels();
    rmlUpdateDirtyAll(model);
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

static int toggleLens(void* _) {
    lensEnabled        = !lensEnabled;
    lensParamsDisabled = !lensEnabled;
    applyRenderer();
    persistBool("lensEnabled", lensEnabled != 0);
    syncLabels();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int toggleDof(void* _) {
    dofEnabled        = !dofEnabled;
    dofParamsDisabled = !dofEnabled;
    applyRenderer();
    persistBool("dofEnabled", dofEnabled != 0);
    syncLabels();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int toggleFog(void* _) {
    fogMode = (fogMode + 1) % 2;
    applyRenderer();
    persistDouble("fogMode", fogMode);
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

int lensParamChange(void* _) {
    markSliderDirty(&dirtyLens);
    return 0;
}

int dofParamChange(void* _) {
    markSliderDirty(&dirtyDof);
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
