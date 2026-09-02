#pragma once

namespace engine::renderer {
// RenderDoc in-app API (LD_PRELOAD librenderdoc.so + implicit layer, debug
// builds only). Captures land at ENGINE_RENDERDOC_DIR_<...>_frameN.rdc
// (default /tmp/RenderDoc/c-game_<...>_frameN.rdc).
void* renderDocInit(void);
void renderDocCaptureNow(void);
}
