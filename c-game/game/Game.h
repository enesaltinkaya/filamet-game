#pragma once

#include "ecs/Ecs.h"

namespace game {
class GameSystem final : public engine::System {
public:
    GameSystem();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;

    // Loads the world (terrain + glb + lights, frames the camera). Called on
    // ENTER WORLD, so the menu starts up fast. Idempotent.
    void loadWorld();

    // Tear the world down (gameplay systems + terrain/props render state +
    // in-world guis) and bring up the main menu. Called by the in-game menu's
    // MAIN MENU button (the old engine's pauseMenu "EXIT GAME" action).
    void backToMainMenu();
};

extern GameSystem gameSystem;
}  // namespace game
