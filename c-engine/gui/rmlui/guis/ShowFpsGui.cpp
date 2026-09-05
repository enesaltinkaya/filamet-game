#include "ShowFpsGui.h"

#include "Utils.h"

#include "crmlui.h"

namespace engine {

RmluiShowFpsGui rmluiShowFpsGui;

static void* document = nullptr;
static void* model    = nullptr;

// This engine has no per-frame GPU time (no VulkanProfile /
// rendererElapsedGpu in the Diligent backend); the old engine bound
// renderer.rendererElapsedGpu here.
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
        rmlUpdateDirtyAll(model);
    }
}

}  // namespace engine
