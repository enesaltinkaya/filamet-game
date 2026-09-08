#pragma once

#include "Defines.h"

namespace engine::renderer {

// Everything the graphics settings menu can toggle, backend-agnostic (the
// backends map each field to their native equivalent; a backend that has no
// equivalent ignores it). Persisted in data/settings.json (keys named in
// rendererGraphicsApply + graphicsSettingsLoad at the bottom).
struct GraphicsSettings {
    float renderScale = 1.0f;      // render resolution scale 0.5..2 (5% steps, old-engine snapping)
    bool taa = true;
    float taaWeight = 0.9f;        // 0.5..0.95 history weight; higher = calmer but ghostier (old-engine taaWeight)
    float casStrength = 1.0f;      // 0..1.5 RCAS (Contrast Adaptive Sharpening) strength; 0 = off, 1.0 = AMD reference max, >1.0 amplified (old-engine casStrength)
    bool msaa = false;
    int shadowMode = 1;            // 0 off, 1 PCF, 2 VSM, 3 EVSM2, 4 EVSM4 (DiligentFX SHADOW_MODE_*)
    int shadowQuality = 1;         // 0 low, 1 medium, 2 high (ignored while mode == off)
    bool ssao = true;
    bool ssr = true;
    bool bloom = true;
    float vignette = 0.7f;         // 0..1
    bool dof = false;
    float dofFocus = 10.0f;        // focus distance in metres
    int dofQuality = 4;            // 1..8 gather rings
    bool fog = true;
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

void rendererSetSun(const f32 direction[3], const f32 color[3], f32 intensity);
void rendererSetAmbient(const f32 color[3], f32 intensity);

void rendererSetFog(const f32 color[3], f32 density);
// Fog on/off without touching the world's fog color/density (settings menu).
void rendererSetFogEnabled(bool enabled);

// Apply a full graphics settings block (menu changes + startup load). The
// struct is normalized first (scale snapped to 5% steps, ...); read the applied state back with rendererGraphicsSettings().
void rendererGraphicsApply(const GraphicsSettings& settings);
const GraphicsSettings& rendererGraphicsSettings(void);
void rendererGraphicsLoad(void);

}
