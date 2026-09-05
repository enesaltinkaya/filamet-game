#pragma once

#include "ecs/Ecs.h"

namespace game {
// The settings menu as an RMLUI document (gui/settings/settings.html) —
// the old engine's menu ported (same html/css/lua, same document
// lifecycle): a right-side slide-over panel that opens OVER the main menu
// or the pause menu — the opener hides its own document underneath (both
// use a ~63% black background, #0000009f, so the menu ghosted through the
// panel when both stayed visible) and re-shows it when this panel closes.
// The AUDIO / VIDEO / GRAPHICS buttons (audio and video ported; graphics
// comes later), a disabled KEYBINDINGS row and BACK.
// Added/removed through engine::guiManagerAddGuiNextFrame /
// engine::guiManagerRemoveGuiNextFrame (deferred, like the other rmlui guis).
class SettingsGui : public engine::System {
public:
    SettingsGui();
    void added() override;
    void removed() override;
    void update() override;
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
