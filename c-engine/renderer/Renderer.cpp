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
#include <cstring>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/git/stb_image_write.h"

namespace engine::renderer {

static RenderBackend* activeBackend = nullptr;
static u32 viewportWidth = 0;
static u32 viewportHeight = 0;
static u32 screenshotStartFrame = 3;

// ── screenshot (ENGINE_SCREENSHOT=path: capture one frame, quit) ────────────
static const char* screenshotPath = nullptr;
static bool screenshotDone = false;
static u32 screenshotFrame = 0;

static void selectScreenshotStartFrame(void) {
    if (const char* env = getenv("ENGINE_SCREENSHOT_FRAME")) {
        const unsigned long v = strtoul(env, nullptr, 10);
        screenshotStartFrame = v ? (u32)v : 1;
    } else if (screenshotPath) {
        screenshotStartFrame = 100;
    }
}

// ENGINE_RENDERDOC_CAPTURE=1 + LD_PRELOAD librenderdoc.so — capture one frame
// for inspection (ENGINE_RENDERDOC_CAPTURE_FRAMES, default 30)
static u32 renderDocCaptureFrame = 0;
static u32 renderDocExitFrames = 0;

bool rendererScreenshotShouldCapture(void) {
    if (!screenshotPath) {
        return false;
    }
    if (screenshotDone) {
        return false;
    }
    if (screenshotFrame++ < screenshotStartFrame) {
        return false;  // let shaders/textures warm up first
    }
    screenshotDone = true;
    return true;
}

void rendererScreenshotDeliver(u8* buffer) {
    char path[512];
    snprintf(path, sizeof(path), "%s", screenshotPath);
    // .png → lossless (debug MV-view captures need exact 8-bit values; JPEG
    // quantization flattens small deltas). Everything else stays JPEG.
    const size_t len = strlen(path);
    const bool png = len >= 4 && strcmp(path + len - 4, ".png") == 0;
    const bool ok = png ? stbi_write_png(path, (int)window.width, (int)window.height, 4, buffer, (int)window.width * 4)
                        : stbi_write_jpg(path, (int)window.width, (int)window.height, 4, buffer, 90);
    if (!ok) {
        utils::warn("renderer: cannot save screenshot to %s", path);
    } else {
        utils::info("renderer: screenshot saved to %s", path);
    }
    free(buffer);

    engineStop();
}

bool rendererInit(const char* title, u32 width, u32 height) {
    if (!windowCreate(title, width, height)) {
        return false;
    }

    activeBackend = diligentBackendCreate();
    if (!activeBackend->init()) {
        delete activeBackend;
        activeBackend = nullptr;
        windowDestroy();
        return false;
    }
    utils::info("renderer: initialized (diligent backend)");

    // apply the persisted graphics settings (scale/TAA/shadows/effects)
    rendererGraphicsLoad();

    const char* screenshotEnv = getenv("ENGINE_SCREENSHOT");
    if (screenshotEnv && screenshotEnv[0] != '\0') {
        screenshotPath = screenshotEnv;
    }
    selectScreenshotStartFrame();

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
        if (renderDocCaptureNow()) {
            renderDocExitFrames = 2;
        }
    }
    if (renderDocExitFrames && --renderDocExitFrames == 0) {
        engineStop();
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

static GraphicsSettings graphicsApplied;

static GraphicsSettings graphicsNormalize(GraphicsSettings s) {
    // manual scale: 0.5..2 in 5% steps (the old engine's sanitizeRenderScale,
    // range matching the settings menu's 50..200% slider)
    if (s.renderScale < 0.5f) s.renderScale = 0.5f;
    if (s.renderScale > 2.0f) s.renderScale = 2.0f;
    s.renderScale  = (float)((int)(s.renderScale * 20.0f + 0.5f)) / 20.0f;
    if (s.taaWeight < 0.5f) s.taaWeight = 0.5f;
    if (s.taaWeight > 0.95f) s.taaWeight = 0.95f;
    if (s.casStrength < 0.0f) s.casStrength = 0.0f;
    if (s.casStrength > 1.5f) s.casStrength = 1.5f;
    if (s.shadowMode < 0) s.shadowMode = 0;
    if (s.shadowMode > 4) s.shadowMode = 4;
    if (s.shadowQuality < 0) s.shadowQuality = 0;
    if (s.shadowQuality > 2) s.shadowQuality = 2;  // 0=low 1=medium 2=high (older 0..4 files clamp down)
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
    s.renderScale   = (float)utils::settingsGetDouble("renderScale");
    s.taa           = utils::settingsGetBool("taaEnabled");
    s.taaWeight     = (float)utils::settingsGetDouble("taaWeight");
    s.casStrength   = (float)utils::settingsGetDouble("casStrength");
    s.msaa          = utils::settingsGetBool("msaaEnabled");
    s.shadowMode    = utils::settingsGetInt("shadowMode");
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
}
