#pragma once

#include "ecs/Ecs.h"

namespace game {
// "Player Actions" panel, top-right — the old engine's rmlui document
// gui/playerActions/playerActions.html (+ playerActions.css): a section title,
// a collapsible body (collapsed by default), a "Teleport to Azgaar cell" row
// (cell-id input + Teleport button), a "Teleport to 0, 0, 0" button, and a
// status hint line. Shown on ENTER WORLD, removed when ESC returns to the
// main menu. The html's onclick handlers (playerActionTeleportToCell /
// playerActionTeleportToOrigin / playerActionsToggle) run in RmlUi's Lua VM
// against luaRegisterFunction bindings registered in added(), like the old
// engine.
class PlayerActionsGui : public engine::System {
public:
    PlayerActionsGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern PlayerActionsGui playerActionsGui;
}  // namespace game
