#pragma once

#include "gui/Gui.h"

namespace game {
// Graphics settings (the old engine's RMLUI settings/graphics document,
// rebuilt as ImGui). Opens from the main menu's SETTINGS row; every change
// applies to the renderer immediately and persists to data/settings.json.
class SettingsGui : public engine::Gui {
public:
    SettingsGui();
    void draw() override;
};

extern SettingsGui settingsGui;
}  // namespace game
