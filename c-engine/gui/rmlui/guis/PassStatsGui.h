#pragma once

#include "ecs/Ecs.h"

namespace engine {
class PassStatsGui : public System {
public:
    PassStatsGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern PassStatsGui passStatsGui;

void passStatsGuiToggle(void);
}  // namespace engine
