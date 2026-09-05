#include "CreditsGui.h"
#include "gui/GuiManager.h"
#include "gameState/GameState.h"
#include "ecs/system/sound/SoundSystem.h"
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
    ImGui::TextWrapped("Diligent engine port");
    ImGui::Spacing();
    ImGui::Text("UI      Dear ImGui");
    ImGui::Text("Layout  Yoga (flexbox)");
    ImGui::Text("3D      Diligent Engine");
    ImGui::Spacing();
    ImGui::TextWrapped("The old engine's RMLUI (HTML/CSS/Lua) menus are being rebuilt as ImGui, screen by screen.");
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("CLOSE", ImVec2(110.0f, 0.0f))) {
        engine::soundPlayClick();
        engine::gui::guiRemove(&creditsGui);
    }
    if (ImGui::IsItemHovered()) {
        engine::soundPlayHover();
    }
    ImGui::End();
}
}  // namespace game
