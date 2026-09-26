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

    // Loads the world (glb + lights, frames the camera). Called on ENTER
    // WORLD, so the menu starts up fast. Idempotent. Synchronous — the
    // loading screen (LoadingGui) is up while it runs.
    void loadWorld();

    // The enter-world transition now that the world is loaded: flip to
    // STATE_PLAYING, add the gameplay systems, swap the loading screen for the
    // in-world guis. Called by LoadingGui (RMLUI path) once the load is done
    // and the minimum display time has passed, or directly by the menu under
    // ENGINE_NO_RMLUI (no loading screen).
    void finishWorldLoad();

    // Tear the world down (gameplay systems + in-world guis) and bring up the
    // main menu. Called by the in-game menu's MAIN MENU button (the old
    // engine's pauseMenu "EXIT GAME" action).
    void backToMainMenu();
};

extern GameSystem gameSystem;
}
