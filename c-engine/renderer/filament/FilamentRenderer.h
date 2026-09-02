#pragma once

// Filament-side globals, shared by the filament halves of gltf/terrain/gui.
// Only filament-path files include this.

#include "renderer/RenderBackend.h"

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

namespace filament_globals {
extern filament::Engine* engine;
extern filament::SwapChain* swapChain;
extern filament::Renderer* renderer;
extern filament::Scene* scene;
extern filament::View* view;
extern filament::View* uiView;  // dedicated 2D overlay view the GUI (filagui) renders into
extern filament::Camera* camera;
extern utils::Entity cameraEntity;
}  // namespace filament_globals

RenderBackend* filamentBackendCreate(void);

}  // namespace engine::renderer
