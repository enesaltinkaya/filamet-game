#pragma once

#include "ecs/Ecs.h"

namespace game {
// The loading screen (the old engine's gui/loading/loading.html): a full-black
// document with a centred stage text, shown over the clear background while
// the world loads. Added on ENTER WORLD (MainMenuGui) and removed once the
// world is up (GameSystem::finishWorldLoad). The world load itself is
// synchronous, so the screen is up for the load's duration and the stage text
// is static. Added/removed through engine::guiManagerAddGuiNextFrame /
// RemoveGuiNextFrame (deferred, like the other rmlui guis).
class LoadingGui final : public engine::System {
public:
    LoadingGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern LoadingGui loadingGui;

// Creates + shows the loading document synchronously (caller frame) so the
// click frame's render already shows it — no menu-lag frame. The system
// itself is still added deferred (lifecycle bookkeeping); added() is a no-op
// when the document is already up.
void loadingGuiShowNow(void);
}
