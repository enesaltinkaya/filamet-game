#include "SettingsGraphicsGui.h"
#include "../SettingsGui.h"
#include "ecs/system/lua/LuaSystem.h"
#include "gui/rmlui/GuiManagerRmlUi.h"
#include "Utils.h"

#include "crmlui.h"

#include <cstdio>
#include <cstdlib>

namespace game {
SettingsGraphicsGui settingsGraphicsGui;

SettingsGraphicsGui::SettingsGraphicsGui() : engine::System("settingsGraphicsGui") {}

static void* document = nullptr;
static void* model    = nullptr;

// Placeholder UI state — GUI-only port (see the class doc). The old engine
// seeded this from the live renderer (rendererGetAASettings /
// rendererGetUpscalerMode / rendererGetRenderScale, the vulkan*Pass getters
// and utils::settingsGet*) and every handler wrote the change back (the
// matching setters + utils::settings* + settingsWrite). Until those features
// exist here, the controls only move local state; the TODO(graphics-wire)
// spots are where engine calls + persistence go in.
static int   upscalerMode          = 0;  // TODO(graphics-wire)
static char  taaEnabled            = 0;
static float casStrengthPercent    = 0.0f;
static float renderScalePercent    = 100.0f;
static char  renderScaleDisabled   = 0;
static int   shadowsQuality        = 3;  // High
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
static char  contactShadowEnabled  = 1;
static int   fogMode               = 1;  // Fog

static const char* upscalerNames[] = {
    "Off",
    "Native AA",
    "Quality",
    "Balanced",
    "Performance",
    "Ultra Performance",
};
static const char* shadowQualityNames[] = {
    "Off",
    "Low",
    "Medium",
    "High",
};
static const char* fogModeNames[] = {
    "Off",
    "Fog",
};

static char* upscalerLabel;
static char* aaPolicyLabel;
static char* upscalePolicyLabel;
static char* shadowsLabel;
static char* ssrLabel;
static char* aoLabel;
static char* giLabel;
static char* bloomLabel;
static char* lensLabel;
static char* dofLabel;
static char* contactShadowLabel;
static char* fogLabel;
static char* taaLabel;
static char upscalerLabelText[64];
static char aaPolicyLabelText[192];
static char upscalePolicyLabelText[192];
static char shadowsLabelText[16];
static char ssrLabelText[16];
static char aoLabelText[16];
static char giLabelText[16];
static char bloomLabelText[16];
static char lensLabelText[16];
static char dofLabelText[16];
static char contactShadowLabelText[16];
static char fogLabelText[16];
static char taaLabelText[16];

static void syncLabels(void);
static int upscalerPrev(void* _);
static int upscalerNext(void* _);
static int aaCasStrengthChange(void* _);
static int renderScaleChange(void* _);
static int toggleLens(void* _);
static int lensParamChange(void* _);
static int toggleDof(void* _);
static int dofParamChange(void* _);
static int graphicsClose(void* _);
static int toggleShadows(void* _);
static int toggleSsr(void* _);
static int toggleAo(void* _);
static int toggleGi(void* _);
static int toggleBloom(void* _);
static int toggleContactShadow(void* _);
static int toggleFog(void* _);
static int toggleTaa(void* _);

void SettingsGraphicsGui::added() {
    engine::luaRegisterFunction("upscalerPrev", upscalerPrev);
    engine::luaRegisterFunction("upscalerNext", upscalerNext);
    engine::luaRegisterFunction("aaCasStrengthChange", aaCasStrengthChange);
    engine::luaRegisterFunction("renderScaleChange", renderScaleChange);
    engine::luaRegisterFunction("graphicsClose", graphicsClose);
    engine::luaRegisterFunction("toggleShadows", toggleShadows);
    engine::luaRegisterFunction("toggleSsr", toggleSsr);
    engine::luaRegisterFunction("toggleAo", toggleAo);
    engine::luaRegisterFunction("toggleGi", toggleGi);
    engine::luaRegisterFunction("toggleBloom", toggleBloom);
    engine::luaRegisterFunction("toggleLens", toggleLens);
    engine::luaRegisterFunction("lensParamChange", lensParamChange);
    engine::luaRegisterFunction("toggleDof", toggleDof);
    engine::luaRegisterFunction("dofParamChange", dofParamChange);
    engine::luaRegisterFunction("toggleContactShadow", toggleContactShadow);
    engine::luaRegisterFunction("toggleFog", toggleFog);
    engine::luaRegisterFunction("toggleTaa", toggleTaa);

    // TODO(graphics-wire): seed from the live renderer + utils::settings
    // (old engine: rendererGetAASettings / rendererGetUpscalerMode /
    // rendererGetRenderScale, the vulkan*Pass getters, settings "fogMode").
    syncLabels();

    model = rmlCreateModel("graphics");
    rmlBindBool(model, "renderScaleDisabled", &renderScaleDisabled);
    rmlBind(model, "upscalerLabel", &upscalerLabel);
    rmlBind(model, "aaPolicyLabel", &aaPolicyLabel);
    rmlBind(model, "upscalePolicyLabel", &upscalePolicyLabel);
    rmlBindFloat(model, "casStrengthPercent", &casStrengthPercent);
    rmlBindFloat(model, "renderScalePercent", &renderScalePercent);
    rmlBindBool(model, "lensParamsDisabled", &lensParamsDisabled);
    rmlBindFloat(model, "lensGrainPercent", &lensGrainPercent);
    rmlBindFloat(model, "lensChromAbPercent", &lensChromAbPercent);
    rmlBindFloat(model, "lensVignettePercent", &lensVignettePercent);
    rmlBindBool(model, "dofParamsDisabled", &dofParamsDisabled);
    rmlBindFloat(model, "dofQuality", &dofQuality);
    rmlBind(model, "shadowsLabel", &shadowsLabel);
    rmlBind(model, "ssrLabel", &ssrLabel);
    rmlBind(model, "aoLabel", &aoLabel);
    rmlBind(model, "giLabel", &giLabel);
    rmlBind(model, "bloomLabel", &bloomLabel);
    rmlBind(model, "lensLabel", &lensLabel);
    rmlBind(model, "dofLabel", &dofLabel);
    rmlBind(model, "contactShadowLabel", &contactShadowLabel);
    rmlBind(model, "fogLabel", &fogLabel);
    rmlBind(model, "taaLabel", &taaLabel);

    document = rmlNewDocument("gui/settings/graphics/graphics.html");
    rmlLoadDocument(document);
    rmlShowDocument(document);

    // Headless testing: ENGINE_GRAPHICS_SETTINGS_AUTOTEST=close exercises the
    // real BACK path (settingsGuiShow + deferred remove).
    const char* at = getenv("ENGINE_GRAPHICS_SETTINGS_AUTOTEST");
    if (at && at[0]) {
        if (utils::strequals(at, "close")) {
            graphicsClose(nullptr);
        }
    }
}

void SettingsGraphicsGui::update() {
    if (model) {
        rmlUpdateDirtyAll(model);
    }
}

void SettingsGraphicsGui::removed() {
    rmlUnloadDocument(document);
    document = nullptr;
    rmlUnloadModel(model);
    model    = nullptr;
}

// Recompute every bound label from the local state (old engine's syncAAUi +
// syncEffectLabels, but reading locals instead of the live passes).
static void syncLabels(void) {
    renderScaleDisabled = (upscalerMode != 0);

    snprintf(taaLabelText, sizeof(taaLabelText), "%s", taaEnabled ? "On" : "Off");
    taaLabel = taaLabelText;

    snprintf(upscalerLabelText, sizeof(upscalerLabelText), "%s", upscalerNames[upscalerMode]);
    upscalerLabel = upscalerLabelText;

    snprintf(
        aaPolicyLabelText,
        sizeof(aaPolicyLabelText),
        "Post-process AA is disabled while FSR is active.");
    aaPolicyLabel = aaPolicyLabelText;

    snprintf(upscalePolicyLabelText,
             sizeof(upscalePolicyLabelText),
             upscalerMode != 0
                 ? "FSR quality mode controls the internal render resolution. Manual render "
                   "scale is disabled."
                 : "Manual resolution scale is used only when the upscaler is Off.");
    upscalePolicyLabel = upscalePolicyLabelText;

    snprintf(shadowsLabelText, sizeof(shadowsLabelText), "%s",
             shadowQualityNames[shadowsQuality]);
    shadowsLabel = shadowsLabelText;
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
    snprintf(contactShadowLabelText, sizeof(contactShadowLabelText), "%s",
             contactShadowEnabled ? "On" : "Off");
    contactShadowLabel = contactShadowLabelText;
    snprintf(fogLabelText, sizeof(fogLabelText), "%s", fogModeNames[fogMode]);
    fogLabel = fogLabelText;
}

int upscalerPrev(void* _) {
    int cur = (upscalerMode + (int)(sizeof(upscalerNames) / sizeof(upscalerNames[0]) - 1)) %
              (int)(sizeof(upscalerNames) / sizeof(upscalerNames[0]));
    upscalerMode = cur;
    if (upscalerMode != 0) taaEnabled = 0;  // UI-level parity with the old engine's mutual exclusion
    syncLabels();
    rmlUpdateDirtyAll(model);
    // TODO(graphics-wire): debounced rendererSetUpscalerMode + AA settings +
    // rendererApplyRenderScale + persistence (old engine: applyUpscalerModeLater).
    return 0;
}

int upscalerNext(void* _) {
    int count = (int)(sizeof(upscalerNames) / sizeof(upscalerNames[0]));
    upscalerMode = (upscalerMode + 1) % count;
    if (upscalerMode != 0) taaEnabled = 0;  // UI-level parity with the old engine's mutual exclusion
    syncLabels();
    rmlUpdateDirtyAll(model);
    // TODO(graphics-wire): debounced rendererSetUpscalerMode + AA settings +
    // rendererApplyRenderScale + persistence (old engine: applyUpscalerModeLater).
    return 0;
}

int aaCasStrengthChange(void* _) {
    // The bound casStrengthPercent already holds the fresh slider value
    // (RMLUI writes it back during the 'change' event).
    // TODO(graphics-wire): debounced rendererSetAASettings +
    // settings "aaMode"/"aaCasStrength" persistence (old engine: persistAASettings).
    return 0;
}

int renderScaleChange(void* _) {
    if (renderScaleDisabled) {
        return 0;
    }
    // TODO(graphics-wire): debounced rendererSetRenderScale +
    // rendererApplyRenderScale + settings "renderScale" persistence
    // (old engine: renderScaleApply).
    return 0;
}

int toggleShadows(void* _) {
    // Cycle the quality level: off -> low -> medium -> high -> off.
    int count = (int)(sizeof(shadowQualityNames) / sizeof(shadowQualityNames[0]));
    shadowsQuality = (shadowsQuality + 1) % count;
    syncLabels();
    rmlUpdateDirtyAll(model);
    // TODO(graphics-wire): engine::vulkanShadowPassSetQuality +
    // settings "shadowQuality"/"shadowsDisabled" persistence.
    return 0;
}

int toggleSsr(void* _) {
    ssrEnabled = !ssrEnabled;
    syncLabels();
    rmlUpdateDirtyAll(model);
    // TODO(graphics-wire): engine::vulkanSsrPassSetDisabled +
    // settings "ssrDisabled" persistence.
    return 0;
}

int toggleAo(void* _) {
    aoEnabled = !aoEnabled;
    syncLabels();
    rmlUpdateDirtyAll(model);
    // TODO(graphics-wire): engine::vulkanAOPassSetDisabled +
    // settings "aoDisabled" persistence.
    return 0;
}

int toggleGi(void* _) {
    giEnabled = !giEnabled;
    syncLabels();
    rmlUpdateDirtyAll(model);
    // TODO(graphics-wire): engine::vulkanGiPassSetDisabled +
    // settings "giDisabled" persistence.
    return 0;
}

int toggleBloom(void* _) {
    bloomEnabled = !bloomEnabled;
    syncLabels();
    rmlUpdateDirtyAll(model);
    // TODO(graphics-wire): engine::vulkanBloomPassSetDisabled +
    // settings "bloomDisabled" persistence.
    return 0;
}

int toggleLens(void* _) {
    lensEnabled      = !lensEnabled;
    lensParamsDisabled = !lensEnabled;
    syncLabels();
    rmlUpdateDirtyAll(model);
    // TODO(graphics-wire): engine::vulkanLensPassSetDisabled +
    // settings "lensEnabled" persistence.
    return 0;
}

int lensParamChange(void* _) {
    // The bound lens*Percent values already hold the fresh slider values.
    // TODO(graphics-wire): debounced vulkanLensPassSetGrain/ChromAb/Vignette +
    // settings "lensGrain"/"lensChromAb"/"lensVignette" persistence
    // (old engine: persistLensSettings).
    return 0;
}

int toggleDof(void* _) {
    dofEnabled           = !dofEnabled;
    dofParamsDisabled = !dofEnabled;
    syncLabels();
    rmlUpdateDirtyAll(model);
    // TODO(graphics-wire): engine::vulkanDofPassSetDisabled +
    // settings "dofEnabled" persistence.
    return 0;
}

int dofParamChange(void* _) {
    // The bound dofQuality already holds the fresh slider value.
    // TODO(graphics-wire): debounced engine::vulkanDofPassSetQuality +
    // settings "dofQuality" persistence (old engine: persistDofSettings).
    return 0;
}

int toggleContactShadow(void* _) {
    contactShadowEnabled = !contactShadowEnabled;
    syncLabels();
    rmlUpdateDirtyAll(model);
    // TODO(graphics-wire): engine::vulkanContactShadowPassSetDisabled +
    // settings "contactShadowDisabled" persistence.
    return 0;
}

int toggleFog(void* _) {
    fogMode = (fogMode + 1) % 2;
    syncLabels();
    rmlUpdateDirtyAll(model);
    // TODO(graphics-wire): apply the fog mode to the renderer (old engine:
    // vulkanResourceSetFogData + vulkanVolumetricPassSetDisabled) + settings
    // "fogMode" persistence.
    return 0;
}

int toggleTaa(void* _) {
    taaEnabled = !taaEnabled;
    if (taaEnabled) upscalerMode = 0;  // UI-level parity with the old engine's mutual exclusion
    syncLabels();
    rmlUpdateDirtyAll(model);
    // TODO(graphics-wire): rendererSetAAMode + settings "taaEnabled"/
    // "upscalerMode" persistence.
    return 0;
}

// BACK / ESC: re-show the main settings page (synchronously — the click
// handler runs inside the manager's input phase, and rmlShowDocument mid-event
// is safe, see MainMenuGui::luaPlayGame) and remove this page next frame
// (the old engine's futureTask(0, settingsGuiShow) + deferred remove).
int graphicsClose(void* _) {
    settingsGuiShow();
    engine::guiManagerRemoveGuiNextFrame(&settingsGraphicsGui);
    return 0;
}

char settingsGraphicsGuiIsShowing(void) {
    return document != nullptr;
}
}  // namespace game
