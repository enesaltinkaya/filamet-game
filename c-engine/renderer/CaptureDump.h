#pragma once

#include "Defines.h"

namespace engine::renderer {

enum CaptureSource {
    CAPTURE_SCREENSHOT = 0,
    CAPTURE_STAGE_DUMP,
    CAPTURE_SHADOW_ATLAS_DUMP,
    CAPTURE_MV_DUMP,
    CAPTURE_SOURCE_COUNT
};

struct CaptureWindow {
    u32 startFrame;
    u32 count;
    u32 stride;
};

void captureWindowsInit(void);

const CaptureWindow* captureWindow(CaptureSource src);

u32 captureWindowLastFrame(const CaptureWindow* w);

bool captureWindowCoversFrame(const CaptureWindow* w, u32 frame);

// Highest last-frame over every configured capture source; 0 = no automated
// capture configured, so the run keeps going until ENGINE_LOG_TIMEOUT.
u32 captureRunLastFrame(void);

}
