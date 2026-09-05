#pragma once

#include "ecs/Ecs.h"

namespace game {
// The main menu as an RMLUI document (gui/mainMenu/mainMenu.html), the same
// html/css/lua approach as the old engine's menu: the game logo over the
// dimmed 3D scene and the ENTER WORLD / SETTINGS / CREDITS / EXIT button
// column, with milligram.css button styling + global.lua focus/click handling.
// Added/removed through engine::guiManagerAddGuiNextFrame /
// engine::guiManagerRemoveGuiNextFrame (deferred, like the other rmlui guis).
class MainMenuGui : public engine::System {
public:
    MainMenuGui();
    void added() override;
    void removed() override;
};

extern MainMenuGui mainMenuGui;
}  // namespace game
