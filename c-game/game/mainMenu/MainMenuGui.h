#pragma once

#include "gui/Gui.h"

namespace game {
// The main menu, drawn with ImGui (via filagui): the game logo over the
// dimmed 3D scene and a plain-text button column (ENTER WORLD / SETTINGS /
// CREDITS / EXIT), matching the old engine's rcss menu.
class MainMenuGui : public engine::Gui {
public:
    MainMenuGui();
    void added() override;
    void draw() override;
};

extern MainMenuGui mainMenuGui;
}  // namespace game
