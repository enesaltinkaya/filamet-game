#pragma once

#include "ecs/Ecs.h"

#include "crmlui.h"

namespace engine {

// Drives the crmlui wrapper for the whole engine: owns the RmlParams
// (init/destroy), the per-frame input -> rmlSendInputEvent pump, document
// add/remove, resize, cursor and scale updates, and the Ctrl+B/D/P/F8
// toggles. Ported from the old engine's GuiManagerRmlUi with two adaptations:
// deferred add/remove is a queue applied at the top of postUpdate (this engine
// never runs utils::futureTaskRun), and resize arrives as an
// INPUT_EVENT_WINDOW_RESIZED in the synthesized input stream instead of a
// "swapchainCreated" signal.
class GuiManagerRmlUi : public System {
public:
    GuiManagerRmlUi();
    void added() override;
    void removed() override;
    void postUpdate() override;

public:
    // Re-reads the window cursors into the params (the wrapper keeps its own
    // copy) and refreshes its cursor table.
    void updateCursors(void);

private:
    // Must outlive added(): the wrapper stores the RmlParams pointer without
    // copying it (RenderInterface_VK ctor), so this storage is owned by the
    // manager and never a function local.
    RmlParams params{};
};

extern GuiManagerRmlUi guiManagerRmlUi;

// ENGINE_NO_RMLUI: completely disable the RMLUI stack. When set, the
// GuiManagerRmlUi system is never added (so rmlInitVulkan never runs), the
// rmlui draw hooks are skipped, and the crmlui-touching free functions below
// are no-ops. Cached on first call.
char rmluiDisabled(void);

// Deferred (next-frame) gui add/remove — applied at the top of the manager's
// postUpdate, outside any iteration (the old engine's futureTaskAdd(0, ...)).
void guiManagerAddGuiNextFrame(System* gui);
void guiManagerRemoveGuiNextFrame(System* gui);

/* 1 if the mouse cursor is hovering over, or has activated, an element
 * of any loaded RmlUi document (Rml::Context::IsMouseInteracting).
 * Game input that would conflict with the GUI (the camera's click-hold-rotate)
 * should be gated on this. */
char guiManagerIsMouseInteracting(void);

void guiManagerUpdateScale(void);
void guiManagerUpdateCursors(void);
void guiManagerToggleShowFps(void);
void guiManagerReleaseTexture(const char* name);
void guiManagerReleaseAllTextures(void);
}  // namespace engine
