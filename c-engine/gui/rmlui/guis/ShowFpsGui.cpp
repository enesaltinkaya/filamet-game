#include "ShowFpsGui.h"

#include "Utils.h"

#include "crmlui.h"

#include "renderer/diligent/DiligentRenderer.h"

namespace engine {

RmluiShowFpsGui rmluiShowFpsGui;

static void* document = nullptr;
static void* model    = nullptr;

// GPU time of the last completed frame (ns, like utils::timer.elapsed):
// a QUERY_TYPE_DURATION query in the Diligent backend — the old engine
// derived this from its own draw-command timestamps.
static double gpuTime = 0.0;

RmluiShowFpsGui::RmluiShowFpsGui() : System("showFpsGui") {}

void RmluiShowFpsGui::added() {
    document = rmlNewDocument("gui/showFps/showFps.html");
    model    = rmlCreateModel("showFps");
    rmlBindDouble(model, "fps", &utils::timer.fps);
    rmlBindDouble(model, "elapsedCpu", &utils::timer.elapsed);
    rmlBindDouble(model, "elapsedGpu", &gpuTime);
    rmlBindDouble(model, "elapsedCpuTotal", &utils::timer.elapsedFull);

    rmlLoadDocument(document);
    rmlShowDocumentWithoutFocus(document);
}

void RmluiShowFpsGui::removed() {
    if (model) {
        rmlUnloadModel(model);
        model = nullptr;
    }
    if (document) {
        rmlUnloadDocument(document);
        document = nullptr;
    }
}

void RmluiShowFpsGui::update() {
    static double lastShown;
    double now = utils::nanos();
    if (now > lastShown + BILLION / 2.) {  // twice per second
        lastShown = now;
        gpuTime = renderer::diligent::diligentGpuTimeNs();
        rmlUpdateDirtyAll(model);
    }
}

}  // namespace engine
