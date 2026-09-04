#pragma once

#include "Defines.h"

// Shared constants of both render paths (the look of the game must not depend
// on the backend) + the internal interface every backend implements. Nothing
// in here leaks into engine/game code: they only see renderer/Renderer.h.

namespace engine::renderer {

inline constexpr f32 kClearColor[4] = {0.02f, 0.04f, 0.09f, 1.0f};
inline constexpr f32 kCameraFovYDeg = 60.0f;
inline constexpr f32 kCameraNear = 0.1f;
inline constexpr f32 kCameraFar = 20000.0f;

class RenderBackend {
public:
    virtual ~RenderBackend() = default;

    virtual bool init() = 0;
    virtual void resize(u32 width, u32 height) = 0;  // also re-applies projection
    virtual void draw() = 0;                        // one full frame (incl. UI)
    virtual void destroy() = 0;

    virtual void cameraLookAt(const f32 eye[3], const f32 center[3], const f32 up[3]) = 0;
    virtual void cameraGet(f32 pos[3], f32 forward[3]) = 0;

    virtual void setSun(const f32 direction[3], const f32 color[3], f32 intensity) = 0;
    virtual void setAmbient(const f32 color[3], f32 intensity) = 0;

    // Atmospheric distance fog on the world view (exponential extinction,
    // constant density, fog color should match the sky clear color so the
    // fogged far terrain meets the horizon seamlessly). density is [1/m].
    virtual void setFog(const f32 color[3], f32 density) = 0;
};

// Implemented by the backends (renderer/filament, renderer/diligent).
RenderBackend* filamentBackendCreate(void);
RenderBackend* diligentBackendCreate(void);

// ── screenshot plumbing (implemented in Renderer.cpp, used by the backends) ──
// True exactly once, a few frames after startup (shaders/textures warm).
bool rendererScreenshotShouldCapture(void);
// Backend hands over a w*h*4 RGBA buffer (top-down rows, no padding); it is
// saved as JPEG (ENGINE_SCREENSHOT) and freed, then the app shuts down.
void rendererScreenshotDeliver(u8* rgbaBuffer);

}  // namespace engine::renderer
