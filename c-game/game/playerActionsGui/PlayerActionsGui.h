#pragma once

#include "gui/Gui.h"

namespace game {
// "Player Actions" panel — port of the old engine's rmlui document
// gui/playerActions/playerActions.html (+ playerActions.css): a top-right
// translucent panel with a section title, a collapsible body (collapsed by
// default), a "Teleport to Azgaar cell" row (cell id input + button), a
// "Teleport to 0, 0, 0" button, and a small status hint line. Shown on
// ENTER WORLD, removed when ESC returns to the main menu. The old engine's
// lua hooks (playerActionTeleportToCell / ...ToOrigin / playerActionsToggle)
// have no counterpart here — there is no lua in this engine; the widgets
// are immediate-mode.
class PlayerActionsGui : public engine::Gui {
public:
    PlayerActionsGui();
    void draw() override;
};

extern PlayerActionsGui playerActionsGui;
}  // namespace game
