#pragma once

#include "ecs/Ecs.h"

namespace game {
// Camera debug readout, bottom-right — the old engine's rmlui document
// gui/camera/camera.html (translucent black box, "Camera" header, Sometype
// Mono, values refreshed every 50ms, pointer-events none). Added on ENTER
// WORLD, removed when ESC returns to the main menu.
class CameraGui : public engine::System {
public:
    CameraGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern CameraGui cameraGui;
}  // namespace game
