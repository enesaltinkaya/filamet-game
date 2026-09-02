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
};

extern GameSystem gameSystem;
}  // namespace game
