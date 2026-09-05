#include "Renderer.h"

#include "RenderBackend.h"
#include "Engine.h"
#include "RenderDoc.h"
#include "Utils.h"
#include "logger/Logger.h"
#include "renderer/Window.h"
#include "settings/Settings.h"

#include <cstdio>
#include <cstdlib>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/git/stb_image_write.h"

namespace engine::renderer {

static Backend backend = Backend::Filament;
static RenderBackend* activeBackend = nullptr;
static u32 viewportWidth = 0;
static u32 viewportHeight = 0;
static u32 screenshotStartFrame = 3;

static void selectScreenshotStartFrame(void) {
    if (const char* env = getenv("ENGINE_SCREENSHOT_FRAME")) {
        const unsigned long v = strtoul(env, nullptr, 10);
        screenshotStartFrame = v ? (u32)v : 1;
    }
}

// ── screenshot (ENGINE_SCREENSHOT=path: capture one frame, quit) ────────────
static const char* screenshotPath = nullptr;
static bool screenshotDone = false;
static u32 screenshotFrame = 0;

// ENGINE_RENDERDOC_CAPTURE=1 + LD_PRELOAD librenderdoc.so — capture one frame
// for inspection (ENGINE_RENDERDOC_CAPTURE_FRAMES, default 30)
static u32 renderDocCaptureFrame = 0;

Backend rendererBackend(void) {
    return backend;
}

const char* rendererBackendName(void) {
    return backend == Backend::Diligent ? "diligent" : "filament";
}

bool rendererScreenshotShouldCapture(void) {
    if (!screenshotPath || screenshotDone) {
        return false;
    }
    if (screenshotFrame++ < screenshotStartFrame) {
        return false;  // let shaders/textures warm up first
    }
    screenshotDone = true;
    return true;
}

void rendererScreenshotDeliver(u8* buffer) {
    if (!stbi_write_jpg(screenshotPath, (int)window.width, (int)window.height, 4, buffer, 90)) {
        utils::warn("renderer: cannot save screenshot to %s", screenshotPath);
    } else {
        utils::info("renderer: screenshot saved to %s", screenshotPath);
    }
    free(buffer);

    // automated run is done → shut the app down
    engineStop();
}

static Backend selectBackend(void) {
    const char* env = getenv("ENGINE_RENDERER");
    if (env && env[0] != '\0') {
        if (utils::strequals(env, "diligent")) return Backend::Diligent;
        if (utils::strequals(env, "filament")) return Backend::Filament;
        utils::warn("renderer: unknown ENGINE_RENDERER '%s' (filament|diligent)", env);
    }
    // persisted setting (0 = filament, 1 = diligent); settings templates carry
    // the default, so a missing key reads as filament
    return utils::settingsGetInt("rendererBackend") == 1 ? Backend::Diligent : Backend::Filament;
}

bool rendererInit(const char* title, u32 width, u32 height) {
    if (!windowCreate(title, width, height)) {
        return false;
    }

    backend = selectBackend();
    activeBackend = backend == Backend::Diligent ? diligentBackendCreate() : filamentBackendCreate();
    if (!activeBackend->init()) {
        delete activeBackend;
        activeBackend = nullptr;
        windowDestroy();
        return false;
    }
    utils::info("renderer: initialized (%s backend)", rendererBackendName());

    // apply the persisted graphics settings (upscaler/TAA/shadows/effects)
    rendererGraphicsLoad();

    selectScreenshotStartFrame();

    const char* screenshotEnv = getenv("ENGINE_SCREENSHOT");
    if (screenshotEnv && screenshotEnv[0] != '\0') {
        screenshotPath = screenshotEnv;
    }

#ifndef NDEBUG
    if (getenv("ENGINE_RENDERDOC_CAPTURE")) {
        const char* framesEnv = getenv("ENGINE_RENDERDOC_CAPTURE_FRAMES");
        renderDocCaptureFrame = framesEnv && framesEnv[0] != '\0' ? (u32)atoi(framesEnv) : 30;
    }
#endif
    return true;
}

void rendererDraw(void) {
    if (!activeBackend) {
        return;
    }

#ifndef NDEBUG
    if (renderDocCaptureFrame && --renderDocCaptureFrame == 0) {
        renderDocCaptureNow();
    }
#endif

    if (window.width != viewportWidth || window.height != viewportHeight) {
        viewportWidth = window.width;
        viewportHeight = window.height;
        activeBackend->resize(window.width, window.height);
    }

    activeBackend->draw();
}

void rendererDestroy(void) {
    utils::info("renderer: destroying");
    if (activeBackend) {
        activeBackend->destroy();
        delete activeBackend;
        activeBackend = nullptr;
    }
    windowDestroy();
}

void rendererCameraLookAt(const double eye[3], const double center[3], const double up[3]) {
    if (activeBackend) {
        activeBackend->cameraLookAt(eye, center, up);
    }
}

void rendererCameraLookAt(const f32 eye[3], const f32 center[3], const f32 up[3]) {
    const double deye[3] = {(double)eye[0], (double)eye[1], (double)eye[2]};
    const double dcenter[3] = {(double)center[0], (double)center[1], (double)center[2]};
    const double dup[3] = {(double)up[0], (double)up[1], (double)up[2]};
    rendererCameraLookAt(deye, dcenter, dup);
}

void rendererCameraGet(f32 pos[3], f32 forward[3]) {
    if (activeBackend) {
        activeBackend->cameraGet(pos, forward);
    }
}

double rendererWorldAnchorX(void) { return activeBackend ? activeBackend->worldAnchorX() : 0.0; }
double rendererWorldAnchorZ(void) { return activeBackend ? activeBackend->worldAnchorZ() : 0.0; }

void rendererSetSun(const f32 direction[3], const f32 color[3], f32 intensity) {
    if (activeBackend) {
        activeBackend->setSun(direction, color, intensity);
    }
}

void rendererSetAmbient(const f32 color[3], f32 intensity) {
    if (activeBackend) {
        activeBackend->setAmbient(color, intensity);
    }
}

void rendererSetFog(const f32 color[3], f32 density) {
    if (activeBackend) {
        activeBackend->setFog(color, density);
    }
}

void rendererSetFogEnabled(bool enabled) {
    if (activeBackend) {
        activeBackend->setFogEnabled(enabled);
    }
}

// ── graphics settings (menu ↔ renderer) ────────────────────────────────────
static GraphicsSettings graphicsApplied;

static int sanitizeUpscaler(int mode) {
    if (mode < UPSCALER_OFF || mode >= UPSCALER_COUNT) {
        return UPSCALER_OFF;
    }
    return mode;
}

static GraphicsSettings graphicsNormalize(GraphicsSettings s) {
    s.upscaler     = sanitizeUpscaler(s.upscaler);
    // manual scale: 0.5..1 in 5% steps (the old engine's snapping), upscaler
    // presets carry their own TAA-upscale render scale
    if (s.renderScale < 0.5f) s.renderScale = 0.5f;
    if (s.renderScale > 1.0f) s.renderScale = 1.0f;
    s.renderScale  = (float)((int)(s.renderScale * 20.0f + 0.5f)) / 20.0f;
    if (s.sharpening < 0.0f) s.sharpening = 0.0f;
    if (s.sharpening > 1.0f) s.sharpening = 1.0f;
    // TAA upscaling IS the upscaler now (filament reconstructs to native from
    // a jittered low-res TAA pass), so the upscaler requires TAA on
    if (s.upscaler != UPSCALER_OFF) s.taa = true;
    if (s.taaWeight < 0.5f) s.taaWeight = 0.5f;
    if (s.taaWeight > 0.95f) s.taaWeight = 0.95f;
    if (s.shadowQuality < 0) s.shadowQuality = 0;
    if (s.shadowQuality > 3) s.shadowQuality = 3;
    if (s.vignette < 0.0f) s.vignette = 0.0f;
    if (s.vignette > 1.0f) s.vignette = 1.0f;
    if (s.dofQuality < 1) s.dofQuality = 1;
    if (s.dofQuality > 8) s.dofQuality = 8;
    if (s.dofFocus < 0.1f) s.dofFocus = 0.1f;
    return s;
}

void rendererGraphicsApply(const GraphicsSettings& settings) {
    graphicsApplied = graphicsNormalize(settings);
    if (activeBackend) {
        activeBackend->applyGraphicsSettings(graphicsApplied);
    }
}

const GraphicsSettings& rendererGraphicsSettings(void) {
    return graphicsApplied;
}

void rendererGraphicsLoad(void) {
    GraphicsSettings s;
    s.upscaler      = sanitizeUpscaler((int)utils::settingsGetDouble("upscalerMode"));
    s.renderScale   = (float)utils::settingsGetDouble("renderScale");
    s.sharpening    = (float)(utils::settingsGetDouble("aaCasStrength") / 100.0);
    s.taa           = utils::settingsGetBool("taaEnabled");
    s.taaWeight     = (float)utils::settingsGetDouble("taaWeight");
    s.msaa          = utils::settingsGetBool("msaaEnabled");
    s.shadowQuality = utils::settingsGetInt("shadowQuality");
    s.ssao          = !utils::settingsGetBool("aoDisabled");
    s.ssr           = !utils::settingsGetBool("ssrDisabled");
    s.bloom         = !utils::settingsGetBool("bloomDisabled");
    s.vignette      = (float)(utils::settingsGetDouble("lensVignette") / 100.0);
    s.dof           = utils::settingsGetBool("dofEnabled");
    s.dofFocus      = (float)utils::settingsGetDouble("dofFocus");
    s.dofQuality    = (int)utils::settingsGetDouble("dofQuality");
    s.fog           = utils::settingsGetDouble("fogMode") > 0.5;
    rendererGraphicsApply(s);
}
}  // namespace engine::renderer
