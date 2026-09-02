#pragma once

#include "Defines.h"
#include <utils/Entity.h>

namespace filament {
class Engine;
class Renderer;
class Scene;
class SwapChain;
class View;
class Camera;
}  // namespace filament

namespace engine::renderer {
extern filament::Engine* filamentEngine;
extern filament::SwapChain* swapChain;
extern filament::Renderer* renderer;
extern filament::Scene* scene;
extern filament::View* view;
extern filament::View* uiView;  // dedicated 2D overlay view the GUI (filagui) renders into
extern filament::Camera* camera;
extern utils::Entity cameraEntity;

bool rendererInit(const char* title, u32 width, u32 height);
void rendererDraw(void);
void rendererDestroy(void);
}  // namespace engine::renderer
