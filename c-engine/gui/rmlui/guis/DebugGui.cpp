#include "DebugGui.h"

#include "Utils.h"
#include "ecs/system/lua/LuaSystem.h"
#include "gui/rmlui/GuiManagerRmlUi.h"
#include "renderer/Renderer.h"

#include "crmlui.h"

#include <cstdio>

namespace engine {

// Port of the old engine's DebugGui. The old document (pak_0_engine/gui/debug/
// debug.html) is unchanged; the toggle callbacks that had old-engine Vulkan
// passes (shadow/bloom/AO/GI/volumetric/LPM/IBL/...) are adapted to what the
// Diligent backend actually exposes:
//   - shadows / bloom / ssao / ssr(GI) / fog / RCAS(CAS) drive
//     renderer::GraphicsSettings (rendererGraphicsApply);
//   - everything without an equivalent here (IBL, POM, contact shadow,
//     skybox, grid, reflection, LPM) is an inert stub: the state stays at
//     its neutral value, the callback just resyncs + refreshes the doc.
DebugGui debugGui;

DebugGui::DebugGui() : System("debugGui") {}

static void* document = nullptr;
static void* model    = nullptr;

// Real (GraphicsSettings-backed) state
static char shadowsEnabled;
static char bloomEnabled;
static char aoEnabled;
static char giEnabled;
static char volumetricFogEnabled;
static float casStrengthPercent;

// Stub state — the feature doesn't exist in this engine; kept bound so the
// document renders with its neutral (disabled/zero) values.
static char reflectionEnabled = 0;
static char skyboxEnabled     = 0;
static char gridEnabled       = 0;
static char pomEnabled        = 0;
static char contactShadowEnabled = 0;
static char iblEnabled        = 0;
static char* tonemapLabel;
static char iblFileLabelText[128] = {0};
static char* iblFileLabel;
static char iblSunLabelText[64]   = {0};
static char* iblSunLabel;
static float iblIntensityValue = 0.0f;

/* LPM tone/gamut sliders: inert in this engine (the Diligent backend uses
 * its built-in tonemapping, no per-pass LPM params), bound to statics so
 * the document section renders and stays interactive. */
static float lpmContrast         = 1.0f;
static float lpmHdrMax           = 1.0f;
static float lpmShoulderContrast = 0.25f;
static float lpmSaturation       = 1.0f;
static float lpmExposure         = 1.0f;

static char* aaPolicyLabel;
static char aaPolicyLabelText[192] = {0};

static void syncFromPasses(void) {
    const auto& g = renderer::rendererGraphicsSettings();
    shadowsEnabled     = g.shadowQuality != 0;
    bloomEnabled       = g.bloom;
    aoEnabled          = g.ssao;
    giEnabled          = g.ssr;
    volumetricFogEnabled = g.fog;
    casStrengthPercent = g.sharpening * 100.0f;

    tonemapLabel = (char*)"built-in";
    snprintf(iblFileLabelText, sizeof(iblFileLabelText), "none");
    iblFileLabel = iblFileLabelText;
    snprintf(iblSunLabelText, sizeof(iblSunLabelText), "-");
    iblSunLabel = iblSunLabelText;

    snprintf(aaPolicyLabelText, sizeof(aaPolicyLabelText),
             "TAA and the upscaler are mutually exclusive.");
    aaPolicyLabel = aaPolicyLabelText;
}

static int refresh(void*) {
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int toggleShadows(void* _) {
    (void)_;
    auto g = renderer::rendererGraphicsSettings();
    g.shadowQuality = (g.shadowQuality == 0) ? 2 : 0;  // off <-> medium
    renderer::rendererGraphicsApply(g);
    return refresh(_);
}

static int toggleBloom(void* _) {
    (void)_;
    auto g = renderer::rendererGraphicsSettings();
    g.bloom = !g.bloom;
    renderer::rendererGraphicsApply(g);
    return refresh(_);
}

static int toggleAo(void* _) {
    (void)_;
    auto g = renderer::rendererGraphicsSettings();
    g.ssao = !g.ssao;
    renderer::rendererGraphicsApply(g);
    return refresh(_);
}

static int toggleGi(void* _) {
    (void)_;
    auto g = renderer::rendererGraphicsSettings();
    g.ssr = !g.ssr;  // screen-space reflections: this engine's closest "GI"
    renderer::rendererGraphicsApply(g);
    return refresh(_);
}

static int toggleVolumetricFog(void* _) {
    (void)_;
    auto g = renderer::rendererGraphicsSettings();
    g.fog = !g.fog;
    renderer::rendererGraphicsApply(g);
    return refresh(_);
}

static int aaCasPrev(void* _) {
    (void)_;
    auto g = renderer::rendererGraphicsSettings();
    g.sharpening = (float)(g.sharpening - 0.05f < 0.0f ? 0.0f : g.sharpening - 0.05f);
    renderer::rendererGraphicsApply(g);
    return refresh(_);
}

static int aaCasNext(void* _) {
    (void)_;
    auto g = renderer::rendererGraphicsSettings();
    g.sharpening = (float)(g.sharpening + 0.05f > 1.0f ? 1.0f : g.sharpening + 0.05f);
    renderer::rendererGraphicsApply(g);
    return refresh(_);
}

// ── Stubs (feature has no equivalent in this engine) ─────────────────────────
static int toggleIBL(void* _)          { return refresh(_); }
static int iblFilePrev(void* _)        { return refresh(_); }
static int iblFileNext(void* _)        { return refresh(_); }
static int iblSunLeft(void* _)         { return refresh(_); }
static int iblSunRight(void* _)        { return refresh(_); }
static int iblSunUp(void* _)           { return refresh(_); }
static int iblSunDown(void* _)         { return refresh(_); }
static int iblIntensityDown(void* _)  { return refresh(_); }
static int iblIntensityUp(void* _)     { return refresh(_); }
static int toggleReflection(void* _)   { return refresh(_); }
static int toggleSkybox(void* _)       { return refresh(_); }
static int toggleGrid(void* _)         { return refresh(_); }
static int togglePOM(void* _)          { return refresh(_); }
static int toggleContactShadow(void* _) { return refresh(_); }
static int debugResetLpm(void* _) {
    (void)_;
    lpmContrast = 1.0f;
    lpmHdrMax   = 1.0f;
    lpmShoulderContrast = 0.25f;
    lpmSaturation = 1.0f;
    lpmExposure   = 1.0f;
    return refresh(_);
}

void DebugGui::added() {
    syncFromPasses();

    luaRegisterFunction("debugToggleShadows", toggleShadows);
    luaRegisterFunction("debugToggleReflection", toggleReflection);
    luaRegisterFunction("debugToggleBloom", toggleBloom);
    luaRegisterFunction("debugToggleIBL", toggleIBL);
    luaRegisterFunction("debugIblFilePrev", iblFilePrev);
    luaRegisterFunction("debugIblFileNext", iblFileNext);
    luaRegisterFunction("debugIblSunLeft", iblSunLeft);
    luaRegisterFunction("debugIblSunRight", iblSunRight);
    luaRegisterFunction("debugIblSunUp", iblSunUp);
    luaRegisterFunction("debugIblSunDown", iblSunDown);
    luaRegisterFunction("debugIblIntensityDown", iblIntensityDown);
    luaRegisterFunction("debugIblIntensityUp", iblIntensityUp);
    luaRegisterFunction("debugToggleSkybox", toggleSkybox);
    luaRegisterFunction("debugToggleGrid", toggleGrid);
    luaRegisterFunction("debugTogglePOM", togglePOM);
    luaRegisterFunction("debugToggleContactShadow", toggleContactShadow);
    luaRegisterFunction("debugToggleAo", toggleAo);
    luaRegisterFunction("debugToggleGi", toggleGi);
    luaRegisterFunction("debugToggleVolumetricFog", toggleVolumetricFog);
    luaRegisterFunction("debugAaCasPrev", aaCasPrev);
    luaRegisterFunction("debugAaCasNext", aaCasNext);
    luaRegisterFunction("debugResetLpm", debugResetLpm);

    document = rmlNewDocument("gui/debug/debug.html");
    model    = rmlCreateModel("debug");

    rmlBind(model, "shadowsEnabled", &shadowsEnabled);
    rmlBind(model, "reflectionEnabled", &reflectionEnabled);
    rmlBind(model, "bloomEnabled", &bloomEnabled);
    rmlBind(model, "iblEnabled", &iblEnabled);
    rmlBind(model, "iblFileLabel", &iblFileLabel);
    rmlBind(model, "iblSunLabel", &iblSunLabel);
    rmlBind(model, "iblIntensityValue", &iblIntensityValue);
    rmlBind(model, "skyboxEnabled", &skyboxEnabled);
    rmlBind(model, "gridEnabled", &gridEnabled);
    rmlBind(model, "pomEnabled", &pomEnabled);
    rmlBind(model, "contactShadowEnabled", &contactShadowEnabled);
    rmlBind(model, "aoEnabled", &aoEnabled);
    rmlBind(model, "giEnabled", &giEnabled);
    rmlBind(model, "volumetricFogEnabled", &volumetricFogEnabled);
    rmlBind(model, "tonemapLabel", &tonemapLabel);
    rmlBind(model, "lpmContrast", &lpmContrast);
    rmlBind(model, "lpmHdrMax", &lpmHdrMax);
    rmlBind(model, "lpmShoulderContrast", &lpmShoulderContrast);
    rmlBind(model, "lpmSaturation", &lpmSaturation);
    rmlBind(model, "lpmExposure", &lpmExposure);
    rmlBind(model, "aaPolicyLabel", &aaPolicyLabel);
    rmlBind(model, "casStrengthPercent", &casStrengthPercent);

    rmlLoadDocument(document);
    rmlShowDocumentWithoutFocus(document);
}

void DebugGui::removed() {
    if (document) {
        rmlUnloadDocument(document);
        document = nullptr;
    }
    if (model) {
        rmlUnloadModel(model);
        model = nullptr;
    }
}

void DebugGui::update() {
    syncFromPasses();
    rmlUpdateDirtyAll(model);
}

void debugGuiToggle(void) {
    if (document) {
        guiManagerRemoveGuiNextFrame(&debugGui);
    } else {
        guiManagerAddGuiNextFrame(&debugGui);
    }
}

}  // namespace engine
