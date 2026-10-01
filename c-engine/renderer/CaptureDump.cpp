#include "renderer/CaptureDump.h"

#include "Utils.h"
#include "logger/Logger.h"
#include "platform/Platform.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace engine::renderer {

static CaptureWindow captureWindows[CAPTURE_SOURCE_COUNT];
static bool captureEnabled[CAPTURE_SOURCE_COUNT];
static u32 captureLastFrame = 0;

static u32 envU32(const char* name, u32 fallback) {
    const char* e = getenv(name);
    if (!e) {
        return fallback;
    }
    const unsigned long v = strtoul(e, nullptr, 10);
    return (u32)v;
}

static u32 envStride(void) {
    const u32 v = envU32("ENGINE_SCREENSHOT_BURST_STRIDE", 0);
    return v ? v : 1;
}

u32 captureWindowLastFrame(const CaptureWindow* w) {
    if (!w || !w->count || !w->startFrame) {
        return 0;
    }
    return w->startFrame + (w->count - 1) * (w->stride ? w->stride : 1);
}

bool captureWindowCoversFrame(const CaptureWindow* w, u32 frame) {
    if (!w || !w->count || !w->startFrame || frame < w->startFrame) {
        return false;
    }
    const u32 stride = w->stride ? w->stride : 1;
    if ((frame - w->startFrame) % stride != 0) {
        return false;
    }
    return (frame - w->startFrame) / stride < w->count;
}

const CaptureWindow* captureWindow(CaptureSource src) {
    if (src < 0 || src >= CAPTURE_SOURCE_COUNT || !captureEnabled[src]) {
        return nullptr;
    }
    return &captureWindows[src];
}

u32 captureRunLastFrame(void) {
    return captureLastFrame;
}

static void ensureDumpDir(const char* envName) {
    const char* dir = getenv(envName);
    if (!dir || dir[0] == '\0') {
        return;
    }
    utils::createDirectory((std::string(dir) + utils::platform.seperator).c_str());
}

void captureWindowsInit(void) {
    const u32 frame = envU32("ENGINE_SCREENSHOT_FRAME", 0);
    const u32 burst = envU32("ENGINE_SCREENSHOT_BURST", 0);
    const u32 stride = envStride();

    const char* screenshotEnv = getenv("ENGINE_SCREENSHOT");
    if (screenshotEnv && screenshotEnv[0] != '\0') {
        utils::createDirectory(screenshotEnv);
        captureEnabled[CAPTURE_SCREENSHOT]      = true;
        captureWindows[CAPTURE_SCREENSHOT]      = {frame ? frame : 100u, burst ? burst : 1u, stride};
    }

    if (getenv("ENGINE_STAGE_DUMP")) {
        const u32 stageStride = envU32("ENGINE_SCREENSHOT_BURST_STRIDE", 8);
        captureEnabled[CAPTURE_STAGE_DUMP]     = true;
        captureWindows[CAPTURE_STAGE_DUMP]     = {frame ? frame : 120u, burst ? burst : 8u,
                                                 stageStride ? stageStride : 1u};
        ensureDumpDir("ENGINE_STAGE_DUMP");
    }

    if (getenv("ENGINE_SHADOW_ATLAS_DUMP")) {
        captureEnabled[CAPTURE_SHADOW_ATLAS_DUMP] = true;
        captureWindows[CAPTURE_SHADOW_ATLAS_DUMP] = {frame ? frame : 120u, burst, stride};
        ensureDumpDir("ENGINE_SHADOW_ATLAS_DUMP");
    }

    if (const char* mvPath = getenv("ENGINE_MV_DUMP")) {
        captureEnabled[CAPTURE_MV_DUMP]        = true;
        captureWindows[CAPTURE_MV_DUMP]        = {envU32("ENGINE_MV_DUMP_FRAME", 60), 1u, 1u};
        utils::createDirectory(mvPath);
    }

    captureLastFrame = 0;
    for (int i = 0; i < CAPTURE_SOURCE_COUNT; i++) {
        const u32 last = captureWindowLastFrame(&captureWindows[i]);
        if (last > captureLastFrame) {
            captureLastFrame = last;
        }
    }
    if (captureLastFrame) {
        utils::info("capture: automated run ends after frame %u", captureLastFrame);
    }
}

}
