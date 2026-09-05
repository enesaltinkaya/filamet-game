#include "SettingsGui.h"
#include "gameState/GameState.h"
#include "gui/GuiManager.h"
#include "gui/rmlui/GuiManagerRmlUi.h"
#include "mainMenu/MainMenuGui.h"
#include "renderer/Renderer.h"
#include "ecs/system/sound/SoundSystem.h"
#include "Utils.h"

#include <imgui.h>

#include <cstdio>

namespace game {
SettingsGui settingsGui;

SettingsGui::SettingsGui() : engine::Gui("settingsGui") {}

static const char* upscalerNames[] = {
        "Off",
        "Native AA",
        "Quality",
        "Balanced",
        "Performance",
        "Ultra Performance",
};
static const char* shadowQualityNames[4] = {"Off", "Low", "Medium", "High"};
static const char* fogNames[2]           = {"Off", "On"};

// Local mirror of the applied settings (kept in sync via the renderer's
// normalized read-back so checkboxes always match what the renderer does).
static engine::renderer::GraphicsSettings gfx;
static bool gfxLoaded = false;

// settings.json is written at most every 500 ms while sliders move, and once
// more when the gui closes (the new engine has no futuretask scheduler wired
// into the loop, so the debounce is a plain timestamp).
static double lastChange  = 0.0;
static char dirtySettings = 0;

static void persistKeys(void) {
    utils::settingsSetDouble("upscalerMode", (double)gfx.upscaler);
    utils::settingsSetDouble("renderScale", (double)gfx.renderScale);
    utils::settingsSetDouble("aaCasStrength", (double)(gfx.sharpening * 100.0f));
    utils::settingsSetBool("taaEnabled", gfx.taa);
    utils::settingsSetDouble("taaWeight", (double)gfx.taaWeight);
    utils::settingsSetBool("msaaEnabled", gfx.msaa);
    utils::settingsSetInt("shadowQuality", gfx.shadowQuality);
    // legacy on/off key, kept in sync for old settings files
    utils::settingsSetBool("shadowsDisabled", gfx.shadowQuality == 0);
    utils::settingsSetBool("aoDisabled", !gfx.ssao);
    utils::settingsSetBool("ssrDisabled", !gfx.ssr);
    utils::settingsSetBool("bloomDisabled", !gfx.bloom);
    utils::settingsSetDouble("lensVignette", (double)(gfx.vignette * 100.0f));
    utils::settingsSetBool("dofEnabled", gfx.dof);
    utils::settingsSetDouble("dofQuality", (double)gfx.dofQuality);
    utils::settingsSetDouble("fogMode", gfx.fog ? 1.0 : 0.0);
}

// Apply to the renderer + mark for write. The renderer normalizes (upscaler
// on forces TAA on, scales clamped), so read the applied state back.
static void applied(void) {
    engine::renderer::rendererGraphicsApply(gfx);
    gfx          = engine::renderer::rendererGraphicsSettings();
    lastChange   = utils::millies();
    dirtySettings = 1;
}

static void flushWrite(void) {
    if (!dirtySettings) return;
    persistKeys();
    utils::settingsWrite();
    dirtySettings = 0;
}

static void closeSettings(void) {
    flushWrite();
    engine::soundPlayClick();
    engine::gui::guiRemove(&settingsGui);
    engine::guiManagerAddGuiNextFrame(&mainMenuGui);
}

void SettingsGui::draw() {
    if (gameStateCurrent() != STATE_MAIN_MENU) return;  // menu session ended

    if (!gfxLoaded) {
        gfx       = engine::renderer::rendererGraphicsSettings();
        gfxLoaded = true;
    }

    // Automated-run hook (mirrors MainMenuGui's ENGINE_AUTOTEST): apply one
    // settings change programmatically, persist it and close, so headless
    // runs can exercise the apply/persist path.
    static char autotestRan = 0;
    if (!autotestRan) {
        autotestRan = 1;
        const char* at = getenv("ENGINE_SETTINGS_AUTOTEST");
        if (at && at[0]) {
            utils::info("settingsGui: autotest '%s'", at);
            bool matched = true;
            if (utils::strequals(at, "upscaler")) gfx.upscaler = engine::renderer::UPSCALER_QUALITY;
            else if (utils::strequals(at, "shadowhigh")) gfx.shadowQuality = 3;
            else if (utils::strequals(at, "shadowoff")) gfx.shadowQuality = 0;
            else if (utils::strequals(at, "msaa")) gfx.msaa = true;
            else if (utils::strequals(at, "taaoff")) {
                gfx.taa      = false;
                gfx.upscaler = engine::renderer::UPSCALER_OFF;  // upscaling rides on TAA
            }
            else if (utils::strequals(at, "scale")) gfx.renderScale = 0.65f;
            else matched = false;
            if (matched) {
                applied();
                closeSettings();  // flushes settings.json + returns to the menu
                return;
            }
        }
    }

    // debounced settings.json write while sliders settle
    if (dirtySettings && utils::millies() > lastChange + 500.0) {
        flushWrite();
    }

    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x;
    const float H = io.DisplaySize.y;
    const float s = engine::gui::guiScale();
    const bool upscalerOn = gfx.upscaler != engine::renderer::UPSCALER_OFF;

    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500.0f * s, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 20, 22, 240));
    ImGui::Begin("SETTINGS - GRAPHICS", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(engine::gui::guiGetBodyFont());

    // section header (gold, like the old rcss section-title)
    auto section = [](const char* title) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(232, 196, 74, 255));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();
    };

    // helper: whole-row apply for a combo (##id so the label renders once)
    auto comboRow = [&](const char* label, int* value, const char* const names[], int count) {
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-1.0f);
        char id[64];
        snprintf(id, sizeof(id), "##%s", label);
        if (ImGui::Combo(id, value, names, count)) {
            applied();
            return true;
        }
        return false;
    };

    section("RESOLUTION & SCALING");
    ImGui::TextUnformatted("Upscaler");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##Upscaler", &gfx.upscaler, upscalerNames,
                     engine::renderer::UPSCALER_COUNT)) {
        // TAA upscaling IS the upscaler — picking a preset (re)enables TAA
        if (gfx.upscaler != engine::renderer::UPSCALER_OFF) gfx.taa = true;
        applied();
    }
    // full-width sliders: the label renders inside the bar (matches the combos)
    ImGui::PushItemWidth(-1.0f);
    ImGui::BeginDisabled(upscalerOn);
    int scalePercent = (int)(gfx.renderScale * 100.0f + 0.5f);
    if (ImGui::SliderInt("##Resolution scale", &scalePercent, 50, 100, "Resolution scale: %d%%")) {
        gfx.renderScale = (float)scalePercent / 100.0f;
        applied();
    }
    ImGui::EndDisabled();
    int sharpPercent = (int)(gfx.sharpening * 100.0f + 0.5f);
    if (ImGui::SliderInt("##Sharpening", &sharpPercent, 0, 100, "Sharpening: %d%%")) {
        gfx.sharpening = (float)sharpPercent / 100.0f;
        applied();
    }

    section("ANTI-ALIASING");
    if (ImGui::Checkbox("Temporal anti-aliasing (TAA)", &gfx.taa)) {
        // upscaling rides on TAA: turning TAA off drops the upscaler too
        if (!gfx.taa) gfx.upscaler = engine::renderer::UPSCALER_OFF;
        applied();
    }
    if (ImGui::Checkbox("MSAA 4x", &gfx.msaa)) applied();

    section("WORLD");
    comboRow("Shadows", &gfx.shadowQuality, shadowQualityNames, 4);
    if (ImGui::Checkbox("Ambient occlusion", &gfx.ssao)) applied();
    if (ImGui::Checkbox("Screen space reflections", &gfx.ssr)) applied();
    {
        int fogMode = gfx.fog ? 1 : 0;
        if (comboRow("Fog", &fogMode, fogNames, 2)) {
            gfx.fog = fogMode != 0;
            applied();
        }
    }

    section("EFFECTS");
    if (ImGui::Checkbox("Bloom", &gfx.bloom)) applied();
    int vignettePercent = (int)(gfx.vignette * 100.0f + 0.5f);
    if (ImGui::SliderInt("##Vignette", &vignettePercent, 0, 100, "Vignette: %d%%")) {
        gfx.vignette = (float)vignettePercent / 100.0f;
        applied();
    }
    if (ImGui::Checkbox("Depth of field", &gfx.dof)) applied();
    ImGui::PopItemWidth();

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(136, 136, 136, 255));
    ImGui::TextWrapped("Not available in this renderer: GI, contact shadows, film grain, lens chromatic aberration.");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("BACK", ImVec2(110.0f * s, 0.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        closeSettings();
    }
    if (ImGui::IsItemHovered()) engine::soundPlayHover();

    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleColor(1);
}
}  // namespace game
