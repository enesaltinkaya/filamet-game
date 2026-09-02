#pragma once

#include "ecs/Ecs.h"

namespace engine {
// A GUI element (main menu, HUD, settings, credits, ...). Immediate-mode: while
// a Gui is active, the gui manager calls its draw() every frame inside the
// ImGui frame, so draw() emits ImGui widgets (buttons, text, windows) and
// handles clicks. Activate/deactivate with gui::guiAdd / gui::guiRemove.
class Gui : public System {
public:
    explicit Gui(const char* name) : System(name) {}
    virtual void draw() {}
};
}  // namespace engine
