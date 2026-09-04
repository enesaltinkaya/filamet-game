#pragma once

#include "gui/Gui.h"

namespace game {
// Small debug overlay in the bottom-right corner showing the camera's
// position and rotation, ported from the old engine's rmlui document
// gui/camera/camera.html: a translucent black box, "Camera" header, mono
// type, values refreshed every 50ms, no mouse interaction. Added on
// ENTER WORLD, removed when ESC returns to the main menu.
class CameraGui : public engine::Gui {
public:
    CameraGui();
    void draw() override;
};

extern CameraGui cameraGui;
}  // namespace game
