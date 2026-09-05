#pragma once

#include "ecs/Ecs.h"

namespace game {
// The settings menu as an RMLUI document (gui/settings/settings.html) —
// the old engine's menu ported as-is (same html/css/lua, same document
// lifecycle): a right-side slide-over panel that opens OVER the main menu
// (the menu stays underneath), with the AUDIO / VIDEO / GRAPHICS buttons
// (audio and video ported; graphics comes later), a disabled KEYBINDINGS row and BACK.
// Added/removed through engine::guiManagerAddGuiNextFrame /
// engine::guiManagerRemoveGuiNextFrame (deferred, like the other rmlui guis).
class SettingsGui : public engine::System {
public:
    SettingsGui();
    void added() override;
    void removed() override;
};

extern SettingsGui settingsGui;

// The document is alive between added() and removed() (1 = showing).
char settingsGuiIsShowing(void);

// Sub-pages (audio first) hide the main document while they show, then re-show
// it on BACK (the old engine's settingsGuiHide/Show dance — it is safe to call
// these from within an RML input event, like rmlHideDocument in playGame).
void settingsGuiHide(void);
void settingsGuiShow(void);
}  // namespace game
