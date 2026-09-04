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

void rendererCameraLookAt(const f32 eye[3], const f32 center[3], const f32 up[3]) {
    if (activeBackend) {
        activeBackend->cameraLookAt(eye, center, up);
    }
}

void rendererCameraGet(f32 pos[3], f32 forward[3]) {
    if (activeBackend) {
        activeBackend->cameraGet(pos, forward);
    }
}

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
}  // namespace engine::renderer
