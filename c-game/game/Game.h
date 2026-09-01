#pragma once

#include "ecs/Ecs.h"

namespace game {
class GameSystem final : public engine::System {
public:
    GameSystem();
    void added() override;
    void removed() override;
    void update() override;
};

extern GameSystem gameSystem;
}  // namespace game
