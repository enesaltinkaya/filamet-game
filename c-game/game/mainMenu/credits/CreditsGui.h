#pragma once

#include "gui/Gui.h"

namespace game {
// Small overlay window opened from the main menu; proves the gui scaffold can
// run multiple Gui's at once (the menu stays active underneath).
class CreditsGui : public engine::Gui {
public:
    CreditsGui();
    void draw() override;
};

extern CreditsGui creditsGui;
}  // namespace game
