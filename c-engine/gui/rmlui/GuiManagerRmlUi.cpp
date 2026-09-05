#include "gui/rmlui/GuiManagerRmlUi.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "ecs/system/lua/LuaSystem.h"
#include "gui/rmlui/RmluiDiligent.h"
#include "gui/rmlui/guis/DebugGui.h"
#include "gui/rmlui/guis/PassStatsGui.h"
#include "gui/rmlui/guis/ShowFpsGui.h"
#include "gui/rmlui/guis/StatsGui.h"
#include "renderer/Window.h"

#include "crmlui.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace engine {

GuiManagerRmlUi guiManagerRmlUi;

static std::vector<System*> rmluiGuis;
static std::vector<System*> pendingAdds;
static std::vector<System*> pendingRemoves;

// uiScale 0 (the settings default) means "auto": windowCreate() seeds the
// persisted setting with SDL_GetWindowDisplayScale once (old-engine parity),
// so from the first frame on this reads the display scale. The 1.0 fallback
// only covers the window-less / settings-unwritten edge cases.
static float guiManagerScale(void) {
    double s = utils::settingsGetDouble("uiScale");
    return s > 0.0 ? (float)s : 1.0f;
}

GuiManagerRmlUi::GuiManagerRmlUi() : System("gui") {}

void GuiManagerRmlUi::added() {
    using namespace engine::renderer::diligent;

    char version[20] = {};
    rmlGetVersion(version);
    utils::debug("rmlui: RmlUi %s", version);

    params.window.sdlWindowHandle = window.handle;
    params.window.windowWidth    = (int)window.width;
    params.window.windowHeight   = (int)window.height;
    params.window.arrowCursor    = windowGetArrowCursor();
    params.window.handCursor     = windowGetPointerCursor();
    params.window.textCursor      = windowGetTextCursor();
    params.window.setCursorFn    = windowSetCursor;
    params.luaState               = luaGetState();
    params.enableDebugger         = utils::isDebug() ? 1 : 0;

    params.log.debugFn  = utils::debugRml;
    params.log.infoFn   = utils::infoRml;
    params.log.warnFn   = utils::warnRml;
    params.log.errorFn  = utils::errorRml;

    params.file.fileOpenFn  = utils::dmRmlopen;
    params.file.fileCloseFn = utils::dmRmlclose;
    params.file.fileReadFn  = utils::dmRmlread;
    params.file.fileSeekFn  = utils::dmRmlseek;
    params.file.fileTellFn  = utils::dmRmltell;

    params.vulkan.beginFrame            = rmlPassBeginFrame;
    params.vulkan.endFrame              = rmlPassEndFrame;
    params.vulkan.renderGeometry        = rmlPassRenderGeometry;
    params.vulkan.compileGeometry       = rmlPassCompileGeometry;
    params.vulkan.releaseGeometry       = rmlPassReleaseGeometry;
    params.vulkan.renderCompiledGeometry = nullptr;
    params.vulkan.loadTexture           = rmlPassLoadTexture;
    params.vulkan.generateTexture       = rmlPassGenerateTexture;
    params.vulkan.releaseTexture        = rmlPassReleaseTexture;
    params.vulkan.enableScissorRegion   = rmlPassEnableScissorRegion;
    params.vulkan.setScissorRegion      = rmlPassSetScissorRegion;
    params.vulkan.setTransform          = rmlPassSetTransform;
    params.vulkan.setViewport           = rmlPassSetViewport;

    rmlInitVulkan(&params);
    rmlSetDimensions(window.width, window.height, guiManagerScale());
    rmluiPassInit();

    if (utils::settingsGetBool("showFps")) {
        guiManagerAddGuiNextFrame(&rmluiShowFpsGui);
    }

    // Headless testing: ENGINE_DEBUG_GUI=1 shows the debug GUI without the
    // interactive Ctrl+B toggle (e.g. screenshot runs).
    if (getenv("ENGINE_DEBUG_GUI") != nullptr) {
        guiManagerAddGuiNextFrame(&debugGui);
    }

    // Headless testing: ENGINE_STATS_GUI=1 shows the stats + pass-stats guis
    // without the interactive Ctrl+D/P toggles.
    if (getenv("ENGINE_STATS_GUI") != nullptr) {
        guiManagerAddGuiNextFrame(&statsGui);
        guiManagerAddGuiNextFrame(&passStatsGui);
    }
}

void GuiManagerRmlUi::postUpdate() {
    // Apply the deferred add/remove queued last frame (the old engine ran
    // these through futureTaskAdd(0, ...); this engine never runs
    // futureTaskRun, so the queue is local and applied here, before the
    // guis' own update() below sees the new set).
    for (System* gui : pendingAdds) {
        if (std::find(pendingRemoves.begin(), pendingRemoves.end(), gui) != pendingRemoves.end()) {
            continue;  // cancelled by a remove queued after it
        }
        utils::debug("rmlui: showing %s", gui->name);
        rmluiGuis.push_back(gui);
        gui->added();
    }
    pendingAdds.clear();
    for (System* gui : pendingRemoves) {
        for (size_t i = 0; i < rmluiGuis.size(); i++) {
            if (gui == rmluiGuis[i]) {
                utils::debug("rmlui: removing gui %s", gui->name);
                rmluiGuis.erase(rmluiGuis.begin() + i);
                gui->removed();
                break;
            }
        }
    }
    pendingRemoves.clear();

    // Toggles (old engine: Ctrl + letter while the cursor is visible, so a
    // camera drag never triggers a gui toggle).
    if (input.ctrl && windowIsCursorVisible()) {
        KeyCode key = windowMapScancode(input.pressed);
        if (key == KEY_D) {
            statsGuiToggle();
        } else if (key == KEY_B) {
            debugGuiToggle();
        } else if (key == KEY_P) {
            passStatsGuiToggle();
        } else if (key == KEY_F8) {
            static char show;
            show = !show;
            rmlToggleDebugger(show);
        }
    }

    bool cursorWasVisible = windowIsCursorVisible();
    // Ignore the first few motion events until pointer state stabilizes.
    static char skipMotionEvent = 10;
    for (size_t i = 0; i < input.events.size(); i++) {
        InputEvent* ev = &input.events[i];

        if (skipMotionEvent && ev->type == INPUT_EVENT_MOUSE_MOVE) {
            skipMotionEvent--;
            continue;
        }

        // Always forward mouse button events so RmlUi sees the full
        // press/release cycle even when the cursor was hidden mid-frame.
        bool isMouseButton = (ev->type == INPUT_EVENT_MOUSE_BUTTON_DOWN ||
                              ev->type == INPUT_EVENT_MOUSE_BUTTON_UP);
        if (!cursorWasVisible && !isMouseButton) {
            if (ev->type == INPUT_EVENT_MOUSE_MOVE || ev->type == INPUT_EVENT_MOUSE_WHEEL) {
                continue;
            }
        }

        rmlSendInputEvent(ev);
        if (ev->type == INPUT_EVENT_WINDOW_RESIZED) {
            // The new engine has no "swapchainCreated" signal — resize comes
            // through the synthesized input stream.
            rmlSetDimensions(window.width, window.height, guiManagerScale());
        }
    }

    // Update the gui data models first (dirty variables), then layout, so
    // Rml::Context::Update() sees the current frame's values (the old engine
    // hit a one-frame lag in element positions with the reverse order).
    for (System* gui : rmluiGuis) {
        gui->update();
    }

    rmlUpdate();
    rmlRenderVulkan();
}

void GuiManagerRmlUi::removed() {
    pendingAdds.clear();
    pendingRemoves.clear();
    for (System* gui : rmluiGuis) {
        utils::warn("rmlui: remove gui (manager removed): %s", gui->name);
        gui->removed();
    }
    // Drop the list so pending removals can't remove a gui a second time
    // after RML is destroyed below.
    rmluiGuis.clear();
    for (size_t i = 0; i < input.events.size(); i++) {
        rmlSendInputEvent(&input.events[i]);
    }
    rmlUpdate();

    utils::warn("rmlui: RML SHUTDOWN");
    // Must run while the Lua state is alive: ecsDestroy runs systems in
    // priority order, so luaSystem's removed() (deliberately) keeps the state
    // open and Engine.cpp closes it after ecsDestroy.
    rmlDestroy();
}

void guiManagerAddGuiNextFrame(System* gui) {
    // cancel a pending remove for the same gui, then queue the add
    for (size_t i = 0; i < pendingRemoves.size(); i++) {
        if (pendingRemoves[i] == gui) {
            pendingRemoves.erase(pendingRemoves.begin() + i);
            break;
        }
    }
    if (std::find(pendingAdds.begin(), pendingAdds.end(), gui) == pendingAdds.end()) {
        pendingAdds.push_back(gui);
    }
}

char rmluiDisabled(void) {
    static char disabled = getenv("ENGINE_NO_RMLUI") != nullptr;
    return disabled;
}

char guiManagerIsMouseInteracting(void) {
    if (rmluiDisabled()) return 0;
    /* RmlUi's own answer (Rml::Context::IsMouseInteracting): the cursor
     * hovers over, or has activated, an element in any loaded document.
     * The context state is refreshed by rmlUpdate() after the input events
     * are forwarded, so this lags the live cursor by one frame — fine for
     * gating camera input. While a camera drag is in flight (cursor hidden,
     * moves not forwarded) the state is frozen at "not interacting", so an
     * in-flight drag is never blocked by its own gate. */
    return rmlIsMouseInteracting();
}

void guiManagerRemoveGuiNextFrame(System* gui) {
    // cancel a pending add for the same gui, then queue the remove
    for (size_t i = 0; i < pendingAdds.size(); i++) {
        if (pendingAdds[i] == gui) {
            pendingAdds.erase(pendingAdds.begin() + i);
            break;
        }
    }
    if (std::find(pendingRemoves.begin(), pendingRemoves.end(), gui) == pendingRemoves.end()) {
        pendingRemoves.push_back(gui);
    }
}

void guiManagerUpdateScale(void) {
    if (rmluiDisabled()) return;
    rmlSetDimensions(window.width, window.height, guiManagerScale());
}

void GuiManagerRmlUi::updateCursors(void) {
    params.window.arrowCursor = windowGetArrowCursor();
    params.window.handCursor  = windowGetPointerCursor();
    params.window.textCursor  = windowGetTextCursor();
    rmlUpdateCursors(&params);
}

void guiManagerUpdateCursors(void) {
    if (rmluiDisabled()) return;
    guiManagerRmlUi.updateCursors();
}

void guiManagerToggleShowFps(void) {
    if (utils::settingsGetBool("showFps")) {
        guiManagerAddGuiNextFrame(&rmluiShowFpsGui);
    } else {
        guiManagerRemoveGuiNextFrame(&rmluiShowFpsGui);
    }
}

void guiManagerReleaseTexture(const char* name) {
    if (rmluiDisabled()) return;
    rmlReleaseTextureByName(name);
}

void guiManagerReleaseAllTextures(void) {
    if (rmluiDisabled()) return;
    rmlReleaseAllTextures();
}

}  // namespace engine
