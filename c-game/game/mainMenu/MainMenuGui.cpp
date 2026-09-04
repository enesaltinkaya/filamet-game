#include "MainMenuGui.h"
#include "Game.h"
#include "cameraGui/CameraGui.h"
#include "gui/GuiManager.h"
#include "gameState/GameState.h"
#include "credits/CreditsGui.h"
#include "ecs/system/flyingCamera/FlyingCamera.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "Engine.h"
#include "Utils.h"

#include <imgui.h>

#include "renderer/Renderer.h"
#include <string.h>

namespace game {
MainMenuGui mainMenuGui;

MainMenuGui::MainMenuGui() : engine::Gui("mainMenu") {}

static void enterWorld(void) {
    utils::info("mainMenu: ENTER WORLD");
    gameSystem.loadWorld();  // blocks a moment on first entry only
    gameStateSet(STATE_PLAYING);
    engine::ecsSystemAddDeferred(100, &engine::flyingCameraSystem);
    // no-op until a world sets an active HeightmapTerrain (phase 4)
    engine::ecsSystemAddDeferred(100, &engine::heightmapTerrainSystem);
    engine::gui::guiRemove(&mainMenuGui);
    // the old engine showed the camera debug readout while in the world
    engine::gui::guiAdd(&cameraGui);
}

static void exitGame(void) {
    utils::info("mainMenu: EXIT");
    engine::engineStop();
}

// ── pak PNG → UI texture ──────────────────────────────────────────────────
// The active backend uploads the pixels (stb-decoded PNG) and hands back an
// ImTextureID: filament renders it through filagui (a filament::Texture*),
// diligent through imgui_impl_vulkan (a VkDescriptorSet). The .ktx2
// originals stay the GPU-side assets; the PNGs are the CPU-friendly twins
// for the UI (a second Basis transcoder copy can't coexist in the process:
// the engine's copy is already used by the gltf ktx2 path).
#define STB_IMAGE_IMPLEMENTATION
#include "stb/git/stb_image.h"
#undef STB_IMAGE_IMPLEMENTATION

static ImTextureID pngToImGuiTexture(const char* pakPath) {
    // dataManagerGetSize terminates when a path is in no pak — a decorative
    // UI texture is not worth killing the process (draw paths null-guard).
    if (!utils::dataManagerFileExists(pakPath)) {
        utils::error("mainMenu: texture not found '%s'", pakPath);
        return ImTextureID_Invalid;
    }
    u32 size = utils::dataManagerGetSize(pakPath);
    if (size == 0) {
        utils::error("mainMenu: cannot read '%s'", pakPath);
        return ImTextureID_Invalid;
    }
    void* buf = malloc(size);
    utils::dataManagerReadChunk(pakPath, buf, 0, size);
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load_from_memory((const stbi_uc*)buf, (int)size, &w, &h, &ch, 4);
    free(buf);
    if (!px) {
        utils::error("mainMenu: failed to decode '%s'", pakPath);
        return ImTextureID_Invalid;
    }
    return engine::gui::guiTextureCreate((u32)w, (u32)h, px);
}

static ImTextureID logoTex = ImTextureID_Invalid;
static ImTextureID barTex  = ImTextureID_Invalid;
static int focusIdx = 0;  // old menu: first button autofocus, nav-up/down wraps

void MainMenuGui::added() {
    if (logoTex == ImTextureID_Invalid) logoTex = pngToImGuiTexture("images/logo.png");
    // images/button.png is the old engine's gui/images/button.png.ktx2 soft
    // strip (1024x128 black RGB + horizontal alpha sheen), transcoded to PNG
    // with basisu -unpack; the .ktx2 original stays in pak_0_engine as
    // provenance. A missing strip just skips the focused-row highlight.
    if (barTex == ImTextureID_Invalid)  barTex  = pngToImGuiTexture("images/button.png");
    focusIdx = 0;
}

// Layout in 720p design units, scaled by guiScale() (measured from the old
// menu: a 450dp (= 31.25% of window width) right-aligned panel with a
// #0000009f background over the scene; logo at 20% height, 84% of the panel
// width; rows at 58%/62.6%/67.3%/71.9% height with a 33px pitch; 18px
// Montserrat Black font with a 2px black drop shadow; the focused row gets
// the old button.png.ktx2 soft strip stretched over the panel width).
void MainMenuGui::draw() {
    if (gameStateCurrent() != STATE_MAIN_MENU) return;  // guard against double trigger

    // Headless action testing (mirrors the ENGINE_SCREENSHOT/ENGINE_LOG_TIMEOUT
    // automated-run pattern): ENGINE_AUTOTEST=enter|credits|exit fires once.
    static char autotestRan = 0;
    if (!autotestRan) {
        const char* at = getenv("ENGINE_AUTOTEST");
        if (at && at[0]) {
            autotestRan = 1;
            if (utils::strequals(at, "enter")) enterWorld();
            else if (utils::strequals(at, "credits")) engine::gui::guiAdd(&creditsGui);
            else if (utils::strequals(at, "exit")) exitGame();
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x;
    const float H = io.DisplaySize.y;
    const float s = engine::gui::guiScale();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::Begin("MainMenu", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Right-aligned panel (old: body width 450dp, right 0, bg #0000009f)
    const float panelW = 0.3125f * W;
    const float pcx    = W - panelW * 0.5f;  // panel center x
    dl->AddRectFilled(ImVec2(W - panelW, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 159));

    // Logo (old: gui/images/logo.png.ktx2, 768x512, top-centered in the panel)
    if (logoTex != ImTextureID_Invalid) {
        float lw = 0.84f * panelW;
        if (lw > 0.465f * H * 1.5f) lw = 0.465f * H * 1.5f;
        const float lh = lw / (768.0f / 512.0f);
        const float x0 = pcx - lw * 0.5f;
        const float y0 = 0.20f * H;
        dl->AddImage(logoTex, ImVec2(x0, y0), ImVec2(x0 + lw, y0 + lh));
    }

    // Button column: plain white labels with a 2px black drop shadow
    // (old: font-weight 900 + font-effect: shadow(2dp 2dp black) — RMLUI's
    // freetype engine matched the 900 against the variable font's Black
    // named instance, so the old menu text was Montserrat Black). The
    // focused row gets the old button.png.ktx2 soft strip (stretched over
    // the panel width, as the old full-width button decorator was).
    static const char* labels[4] = { "ENTER WORLD", "SETTINGS", "CREDITS", "EXIT" };
    const float rowH  = 33.0f * s;
    const float barW  = panelW;
    const float barH  = 27.0f * s;
    float cy          = 0.58f * H;  // first row center

    ImGui::PushFont(engine::gui::guiGetMenuFont());
    const ImVec2 mouse = io.MousePos;
    for (int i = 0; i < 4; i++) {
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        const ImVec2 r0(pcx - ts.x * 0.5f, cy - ts.y * 0.5f);

        if (mouse.x >= W - panelW && mouse.x <= W &&
            mouse.y >= cy - rowH * 0.5f && mouse.y <= cy + rowH * 0.5f)
            focusIdx = i;  // mouse acts as the nav cursor

        if (focusIdx == i && barTex != ImTextureID_Invalid) {
            const ImVec2 b0(pcx - barW * 0.5f, cy - barH * 0.5f);
            dl->AddImage(barTex, b0, ImVec2(b0.x + barW, b0.y + barH));
        }

        // Single shadow pass (2px offset, as the old shadow(2dp 2dp black)),
        // then the white label on top. No synthetic embolden: the old menu
        // rendered the thin face.
        dl->AddText(ImVec2(r0.x + 2.0f * s, r0.y + 2.0f * s), IM_COL32(0, 0, 0, 255), labels[i]);
        dl->AddText(r0, IM_COL32(255, 255, 255, 255), labels[i]);

        ImGui::SetCursorScreenPos(ImVec2(W - panelW, cy - rowH * 0.5f));
        char id[16];
        snprintf(id, sizeof(id), "##menu%d", i);
        if (ImGui::InvisibleButton(id, ImVec2(panelW, rowH))) {
            if (i == 0) enterWorld();
            else if (i == 1) utils::info("mainMenu: SETTINGS (todo)");
            else if (i == 2) engine::gui::guiAdd(&creditsGui);
            else exitGame();
        }

        cy += rowH;
    }
    ImGui::PopFont();

    // Keyboard navigation: up/down wrap (old: #first nav-up #exit, #exit
    // nav-down #first), enter/space activates the focused row.
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) focusIdx = (focusIdx + 3) % 4;
    else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) focusIdx = (focusIdx + 1) % 4;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Space) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        if (focusIdx == 0) enterWorld();
        else if (focusIdx == 1) utils::info("mainMenu: SETTINGS (todo)");
        else if (focusIdx == 2) engine::gui::guiAdd(&creditsGui);
        else exitGame();
    }

    ImGui::End();
    ImGui::PopStyleColor(1);  // WindowBg
    ImGui::PopStyleVar(1);    // WindowPadding
}
}  // namespace game
