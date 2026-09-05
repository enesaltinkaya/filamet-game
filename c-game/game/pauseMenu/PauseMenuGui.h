#pragma once

#include "ecs/Ecs.h"

namespace game {
// The in-game menu (the old engine's pauseMenu): a separate RMLUI document
// (gui/pauseMenu/pauseMenu.html) shown over the world on ESC, distinct from
// the main menu. SETTINGS opens the settings slide-over over it, MAIN MENU
// tears the world down and returns to the main menu, RETURN TO GAME (or ESC
// on the focused document) closes it. Added/removed through
// engine::guiManagerAddGuiNextFrame / RemoveGuiNextFrame (deferred, like the
// other rmlui guis).
class PauseMenuGui final : public engine::System {
public:
    PauseMenuGui();
    void added() override;
    void removed() override;
};

extern PauseMenuGui pauseMenuGui;

// 1 while the document is loaded (the gui is registered for this session).
char pauseMenuGuiIsShowing(void);
}  // namespace game
