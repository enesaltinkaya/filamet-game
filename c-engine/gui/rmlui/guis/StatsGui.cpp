#include "StatsGui.h"

#include "Utils.h"
#include "ecs/Ecs.h"
#include "gui/rmlui/GuiManagerRmlUi.h"
#include "renderer/Window.h"
#include "renderer/diligent/DiligentRenderer.h"

#include "crmlui.h"

#include <cstdio>
#include <cstring>

namespace engine {

// Port of the old engine's StatsGui. The old document
// (pak_0_engine/gui/stats/stats.html) is unchanged. Bindings that had no
// equivalent in this engine are stubs:
//   - totalGpuTime / overallRendererGpu / per-pass gpu times: no GPU
//     profiling in the Diligent backend -> 0;
//   - drawCalls/instanceCount/triangleCount: the new renderer doesn't count
//     them -> 0;
//   - heap: utils::memoryUsage() doesn't exist here -> 0;
//   - windowScale: the new window has no xscale -> 1.0;
//   - rendererCpu / swapchainElapsed: no per-phase timing -> 0;
//   - framesInFlight: 1 (single-flight Diligent backend);
//   - gpuName: the Diligent device adapter description.
StatsGui statsGui;

StatsGui::StatsGui() : System("statsGui") {}

static void* document = nullptr;
static void* model    = nullptr;
static double lastUpdate;

static u64 heap = 0;
static float totalGpuTime = 0.0f;  // stub: no per-pass GPU timing
static size_t systemSize;
static size_t passSize = 0;  // stub: no renderer.passes in this engine

static char vsync;
static float fpsLimit;
static float uiScale;
static float windowScale = 1.0f;  // stub: no window.xscale
static char isVulkan = 1;

static u32 drawCalls     = 0;  // stubs: not tracked by the new renderer
static u32 instanceCount = 0;
static u32 triangleCount = 0;

static char debugReleaseText[200] = {0};
static char* debugReleaseTextPtr  = debugReleaseText;
static char gpuName[256]          = {0};
static char* gpuNamePtr          = gpuName;

static int framesInFlight = 1;  // stub: single-flight backend
static int swapchainImageCount;
static double rendererCpu         = 0.0;  // stub: no per-phase timing
static double overallRendererGpu  = 0.0;  // stub: no GPU timing
static double swapchainCpuElapsed = 0.0;  // stub: no per-phase timing
static double ecsCpu              = 0.0;  // sum of system cpu times

static void systemInfo(int index, int type, char* out);
static void passInfo(int index, int type, char* out);

void StatsGui::added() {
    snprintf(debugReleaseText, sizeof(debugReleaseText), "%s",
             utils::isDebug() ? "debug mode" : "release mode");

    snprintf(gpuName, sizeof(gpuName), "%s",
             renderer::diligent::device ? renderer::diligent::device->GetAdapterInfo().Description : "unknown");

    document = rmlNewDocument("gui/stats/stats.html");
    model    = rmlCreateModel("stats");

    vsync    = utils::settingsGetBool("vsync");
    fpsLimit = (float)utils::settingsGetDouble("fpsLimit");
    uiScale  = (float)utils::settingsGetDouble("uiScale");
    swapchainImageCount = renderer::diligent::swapChain
        ? (int)renderer::diligent::swapChain->GetDesc().BufferCount
        : 0;

    rmlBind(model, "fps", &utils::timer.fps);
    rmlBind(model, "elapsedFull", &utils::timer.elapsedFull);
    rmlBind(model, "elapsed", &utils::timer.elapsed);
    rmlBind(model, "ups", &utils::timer.ups);
    rmlBind(model, "dt", &utils::timer.dt);
    rmlBind(model, "time", &utils::timer.timeSinceStart);
    rmlBind(model, "frame", &utils::timer.frameCounter);
    rmlBind(model, "isVulkan", &isVulkan);
    rmlBind(model, "drawCalls", &drawCalls);
    rmlBind(model, "instanceCount", &instanceCount);
    rmlBind(model, "triangleCount", &triangleCount);
    rmlBind(model, "vsync", &vsync);
    rmlBind(model, "fpsLimit", &fpsLimit);
    rmlBind(model, "uiScale", &uiScale);
    rmlBind(model, "windowScale", &windowScale);
    rmlBind(model, "width", &window.width);
    rmlBind(model, "height", &window.height);
    rmlBind(model, "heap", &heap);
    rmlBind(model, "gpuName", &gpuNamePtr);
    rmlBind(model, "totalGpuTime", &totalGpuTime);
    rmlBind(model, "framesInFlight", &framesInFlight);
    rmlBind(model, "swapchainImages", &swapchainImageCount);
    rmlBind(model, "rendererCpu", &rendererCpu);
    rmlBind(model, "overallRendererGpu", &overallRendererGpu);
    rmlBind(model, "ecsCpu", &ecsCpu);
    rmlBind(model, "swapchainElapsed", &swapchainCpuElapsed);
    // Bound as char** (not the old engine's &debugReleaseText[0], which
    // bound the first byte as a bool) so the document shows the actual text.
    rmlBind(model, "debugReleaseText", &debugReleaseTextPtr);

    rmlRegisterTransformFunc(model, "systemInfo", systemInfo);
    rmlRegisterTransformFunc(model, "passInfo", passInfo);
    rmlBindArray(model, "passes", &passSize);
    rmlBindArray(model, "systems", &systemSize);

    rmlLoadDocument(document);
    rmlShowDocumentWithoutFocus(document);
}

void StatsGui::removed() {
    if (document) {
        rmlUnloadDocument(document);
        document = nullptr;
    }
    if (model) {
        rmlUnloadModel(model);
        model = nullptr;
    }
}

void StatsGui::update() {
    double now = utils::nanos();
    if (now > lastUpdate + BILLION / 2.) {  // twice per second
        // No per-pass GPU timing in this engine — the total stays 0.
        totalGpuTime = 0.0f;

        systemSize = static_cast<i32>(ecs.systems.size());
        passSize   = 0;

        double totalCpu = 0.0;
        for (size_t i = 0; i < ecs.systems.size(); i++) {
            totalCpu += ecs.systems[i]->cpuElapsedLastFrame;
        }
        ecsCpu = totalCpu;

        vsync   = utils::settingsGetBool("vsync");
        fpsLimit = (float)utils::settingsGetDouble("fpsLimit");
        uiScale  = (float)utils::settingsGetDouble("uiScale");

        lastUpdate = now;
        rmlUpdateDirtyAll(model);
    }
}

void systemInfo(int index, int type, char* out) {
    if ((size_t)index >= ecs.systems.size()) {
        out[0] = '\0';
        return;
    }
    if (type == 0) {
        sprintf(out, "%s", ecs.systems[index]->name);
    } else if (type == 1) {
        sprintf(out, "%.2f", ecs.systems[index]->cpuElapsedLastFrame / MILLION);
    } else {
        out[0] = '\0';
    }
}

void passInfo(int index, int type, char* out) {
    (void)index;
    (void)type;
    out[0] = '\0';  // no renderer passes in this engine
}

void statsGuiToggle(void) {
    if (document) {
        guiManagerRemoveGuiNextFrame(&statsGui);
    } else {
        guiManagerAddGuiNextFrame(&statsGui);
    }
}

}  // namespace engine
