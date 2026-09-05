#pragma once

#include "ecs/Ecs.h"

namespace game {
// The video settings sub-page (gui/settings/video/video.html) — the old
// engine's ported as-is: Fullscreen, Vsync, UI Scale, Cursor Scale,
// Fps Limit (on/off + value) and Show Fps, BACK returns to the main settings
// page. Opens over the settings menu (which hides its document while this
// one shows).
//
// Engine support in this build: Fullscreen (windowToggleFullscreen), UI Scale
// (guiManagerUpdateScale), Fps Limit (timerInit) and Show Fps
// (guiManagerToggleShowFps). Vsync and Cursor Scale have no engine support yet
// (the swapchain present mode is driver-selected; cursors are fixed-size SDL
// system cursors) — those persist the setting and warn.
class SettingsVideoGui : public engine::System {
public:
    SettingsVideoGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern SettingsVideoGui settingsVideoGui;

// 1 while the document is alive (between added() and removed()).
char settingsVideoGuiIsShowing(void);
}  // namespace game
