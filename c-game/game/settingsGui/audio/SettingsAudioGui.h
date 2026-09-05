#pragma once

#include "ecs/Ecs.h"

namespace game {
// The audio settings sub-page (gui/settings/audio/audio.html) — the old
// engine's ported as-is: Effects + Music sliders bound through the RMLUI
// data model, BACK returns to the main settings page. Opens over the
// settings menu (which hides its document while this one shows).
class SettingsAudioGui : public engine::System {
public:
    SettingsAudioGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern SettingsAudioGui settingsAudioGui;

// 1 while the document is alive (between added() and removed()).
char settingsAudioGuiIsShowing(void);
}  // namespace game
