#pragma once

#include "Defines.h"

namespace engine::renderer {

// Upscaling modes for the graphics settings menu. Quality/Balanced/Performance/
// UltraPerformance are the old engine's FSR3 resolution presets, kept as the
// render-scale of TAA upscaling (the scene renders at scale and TAA
// reconstructs to native — no FSR pass); NativeAA = plain TAA at native res.
enum UpscalerMode : u8 {
    UPSCALER_OFF = 0,
    UPSCALER_NATIVE_AA,
    UPSCALER_QUALITY,
    UPSCALER_BALANCED,
    UPSCALER_PERFORMANCE,
    UPSCALER_ULTRA_PERFORMANCE,
    UPSCALER_COUNT,
};

// Everything the graphics settings menu can toggle, backend-agnostic (the
// backends map each field to their native equivalent; a backend that has no
// equivalent ignores it). Persisted in data/settings.json (keys named in
// rendererGraphicsApply + graphicsSettingsLoad at the bottom).
struct GraphicsSettings {
    int upscaler = UPSCALER_OFF;   // UpscalerMode
    float renderScale = 1.0f;      // manual scale 0.5..1, used only when upscaler == OFF
    float sharpening = 1.0f;       // 0..1, post-TAA sharpen (RCAS)
    bool taa = true;               // temporal AA (the upscaler rides on it: presets set the TAA upscale ratio)
    float taaWeight = 0.9f;        // 0.5..0.95 history weight; higher = calmer but ghostier (old-engine taaWeight)
    bool msaa = false;             // 4x multi-sample AA
    int shadowQuality = 2;         // 0 off, 1 low, 2 medium, 3 high
    bool ssao = true;              // screen-space ambient occlusion
    bool ssr = true;               // screen-space reflections
    bool bloom = true;
    float vignette = 0.7f;         // 0..1
    bool dof = false;              // depth of field (focus distance is game-driven)
    float dofFocus = 10.0f;        // focus distance in metres
    int dofQuality = 4;            // 1..8 gather rings
    bool fog = true;               // distance fog on/off (color/density come from the world)
};

bool rendererInit(const char* title, u32 width, u32 height);
void rendererDraw(void);
void rendererDestroy(void);

// Camera: eye/center/up in ABSOLUTE world space (double precision — the
// camera position is the world anchor; the renderer re-expresses the whole
// frame relative to it for float precision, see DiligentRenderer). lookAt
// semantics, world up +Y, camera forward = normalize(center - eye).
// Projections use a 60 degree vertical fov with 0.1..20000 depth.
void rendererCameraLookAt(const double eye[3], const double center[3], const double up[3]);
// f32 overload: convenience for one-shot (static) vantages — far-from-origin
// per-frame cameras must use the double overload (the eye is the world anchor).
void rendererCameraLookAt(const f32 eye[3], const f32 center[3], const f32 up[3]);
void rendererCameraGet(f32 pos[3], f32 forward[3]);

// The world anchor (xz, metres) all renderables are currently placed relative
// to — the last cameraLookAt's eye. World-space code (model placement, tile
// transforms) subtracts it before handing floats to the backend.
double rendererWorldAnchorX(void);
double rendererWorldAnchorZ(void);

// Scene lighting (sun = directional); the backend feeds its PBR shaders.
void rendererSetSun(const f32 direction[3], const f32 color[3], f32 intensity);
void rendererSetAmbient(const f32 color[3], f32 intensity);

// Atmospheric distance fog on the world view (see RenderBackend::setFog).
void rendererSetFog(const f32 color[3], f32 density);
// Fog on/off without touching the world's fog color/density (settings menu).
void rendererSetFogEnabled(bool enabled);

// Apply a full graphics settings block (menu changes + startup load). The
// struct is normalized first (upscaler on forces TAA off, scale clamped, ...);
// read the applied state back with rendererGraphicsSettings().
void rendererGraphicsApply(const GraphicsSettings& settings);
const GraphicsSettings& rendererGraphicsSettings(void);
// Startup: build a GraphicsSettings from data/settings.json and apply it.
void rendererGraphicsLoad(void);

}  // namespace engine::renderer
