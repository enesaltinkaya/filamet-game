#include "PlayerActionsGui.h"
#include "Utils.h"
#include "azgaar/AzgaarWorld.h"
#include "ecs/system/player/Player.h"
#include "ecs/system/sound/SoundSystem.h"
#include "gameState/GameState.h"
#include "gui/GuiManager.h"
#include "loadingAzgaar/LoadingAzgaar.h"

#include <imgui.h>

#include <cstdio>

namespace game {
PlayerActionsGui playerActionsGui;

PlayerActionsGui::PlayerActionsGui() : engine::Gui("playerActionsGui") {}

// Panel state (the old rmlui document's model + collapsed class)
static int   cellId      = 0;
static char  collapsed   = 1;  // the old html shipped with class="collapsed"
static char statusBuf[64];
static char statusInit  = 0;

static void setStatus(const char* text) {
    snprintf(statusBuf, sizeof(statusBuf), "%s", text);
}

static void teleportToCell(void) {
    const AzgaarWorld* world = loadingAzgaarGetWorld();
    if (!world || world->cellCount == 0u) {
        setStatus("Azgaar world is not loaded");
        return;
    }
    u32 id = (u32)cellId;
    if (id >= world->cellCount) {
        snprintf(statusBuf, sizeof(statusBuf), "Invalid cell %u (max %u)", id, world->cellCount - 1u);
        return;
    }
    const AzgaarCell* cell = &world->cells[id];
    float wx, wz;
    azgaarMapToWorld(world, cell->x, cell->y, &wx, &wz);
    float h = azgaarHeightToMeters(world, cell->height);
    if (!engine::playerTeleportTo(wx, h + 1.0f, wz)) {
        setStatus("Player is not ready");
        return;
    }
    snprintf(statusBuf, sizeof(statusBuf), "Teleported to cell %u", id);
    utils::info("playerActionsGui: teleported player to Azgaar cell %u (%.2f %.2f %.2f)",
                id, wx, h + 1.0f, wz);
}

static void teleportToOrigin(void) {
    if (!engine::playerTeleportTo(0.0f, 0.0f, 0.0f)) {
        setStatus("Player is not ready");
        return;
    }
    setStatus("Teleported to 0, 0, 0");
    utils::info("playerActionsGui: teleported player to origin (0.00 0.00 0.00)");
}

void PlayerActionsGui::draw() {
    if (gameStateCurrent() != STATE_PLAYING) return;
    if (!statusInit) {
        statusInit = 1;
        setStatus("Enter Azgaar cell id");
    }

    ImGuiIO& io = ImGui::GetIO();
    const float s = engine::gui::guiScale();
    const float W = 300.0f * s;  // the old css body width

    // Top-right, content-sized height (the old css: position absolute,
    // top 0 / right 0, width 300dp)
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - W, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 20, 22, 235));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(60, 60, 65, 200));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * s, 10.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * s);
    ImGui::Begin("PlayerActionsGui", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);

    ImGui::PushFont(engine::gui::guiGetMonoFont());

    // section-title: gold, uppercase
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(232, 196, 74, 255));
    ImGui::TextUnformatted("PLAYER ACTIONS");
    ImGui::PopStyleColor();

    // section-divider
    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(80, 80, 85, 180));
    ImGui::Separator();
    ImGui::PopStyleColor();

    // actions-toggle-btn: gold text on translucent gold
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(232, 196, 74, 40));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(232, 196, 74, 90));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(232, 196, 74, 120));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(232, 196, 74, 255));
    if (ImGui::Button("Player Actions")) { engine::soundPlayClick(); collapsed = !collapsed; }
    if (ImGui::IsItemHovered()) engine::soundPlayHover();
    ImGui::PopStyleColor(4);

    if (!collapsed) {
        ImGui::TextUnformatted("Teleport to Azgaar cell");
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(10, 10, 12, 220));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(70, 70, 75, 200));
        ImGui::PushItemWidth(140.0f * s);
        ImGui::InputInt("##cellId", &cellId, 1, 0);
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0.0f, 4.0f * s);
        if (ImGui::Button("Teleport")) { engine::soundPlayClick(); teleportToCell(); }
        if (ImGui::IsItemHovered()) engine::soundPlayHover();
        ImGui::PopItemWidth();

        ImGui::TextUnformatted("Teleport to coordinates");
        if (ImGui::Button("Teleport to 0, 0, 0")) { engine::soundPlayClick(); teleportToOrigin(); }
        if (ImGui::IsItemHovered()) engine::soundPlayHover();

        // hint: small grey status line
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(136, 136, 136, 255));
        ImGui::TextUnformatted(statusBuf);
        ImGui::PopStyleColor();
    }

    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}
}  // namespace game
