#pragma once

#include "ecs/Ecs.h"

namespace game {
// Player debug readout — the old engine's rmlui document
// gui/player/player.html: a translucent black box ("Player" header, Sometype
// Mono) showing the current Azgaar cell id + the player position, refreshed
// every 50ms, pointer-events none. Anchored bottom-right at right:270dp, so
// it sits left of the camera readout. Added on ENTER WORLD, removed when
// ESC returns to the main menu.
class PlayerGui : public engine::System {
public:
    PlayerGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern PlayerGui playerGui;
}  // namespace game
