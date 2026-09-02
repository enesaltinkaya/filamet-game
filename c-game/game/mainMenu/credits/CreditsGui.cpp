#include "CreditsGui.h"
#include "gui/GuiManager.h"
#include "gameState/GameState.h"
#include "Utils.h"

#include <imgui.h>

namespace game {
CreditsGui creditsGui;

CreditsGui::CreditsGui() : engine::Gui("credits") {}

void CreditsGui::draw() {
    if (gameStateCurrent() != STATE_MAIN_MENU) return;  // menu session ended

    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x;
    float H = io.DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Always);
    ImGui::Begin("CREDITS", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

    ImGui::PushFont(engine::gui::guiGetBodyFont());
    ImGui::TextWrapped("Filament engine port");
    ImGui::Spacing();
    ImGui::Text("UI      Dear ImGui + filagui");
    ImGui::Text("Layout  Yoga (flexbox)");
    ImGui::Text("3D      Google Filament");
    ImGui::Spacing();
    ImGui::TextWrapped("The old engine's RMLUI (HTML/CSS/Lua) menus are being rebuilt as ImGui, screen by screen.");
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("CLOSE", ImVec2(110.0f, 0.0f))) {
        engine::gui::guiRemove(&creditsGui);
    }
    ImGui::End();
}
}  // namespace game
