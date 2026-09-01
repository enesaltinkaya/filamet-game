#pragma once

#include "ecs/Ecs.h"

namespace engine {
// Free-flight camera:
//   F      toggle flying (captures the mouse)
//   ESC    exit flying mode
//   R      reset to the camera pose captured at init
//   WASD   move forward / left / backward / right (relative to view)
//   SPACE/X  up / down
//   SHIFT  10x speed, CTRL 0.25x speed
//   mouse  look, wheel dollies along the view direction
class FlyingCameraSystem final : public System {
public:
    FlyingCameraSystem();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
};

extern FlyingCameraSystem flyingCameraSystem;
}  // namespace engine
