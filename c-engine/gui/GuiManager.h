#pragma once

#include "gui/Gui.h"

struct ImFont;  // imgui type; declared in the global namespace (as in imgui.h)

namespace engine::gui {
// Initialize the ImGui backend (Filament's filagui) on the renderer's UI view.
// Must run after rendererInit (needs the filament engine + UI view) and after the
// data manager is ready (loads the UI font from the pak).
void guiInit(void);
void guiDestroy(void);

// Show / hide a gui. Applied on the next frame via the deferred system queue,
// so it is safe to call from inside a gui's own draw() (a button click).
void guiAdd(Gui* gui);
void guiRemove(Gui* gui);

// True when at least one gui is active this frame. The renderer uses this to
// decide whether to draw the UI overlay view.
bool guiIsActive(void);

// Current UI scale factor (1.0 at a 720p framebuffer; scales with resolution).
// Multiply layout positions/sizes by this so the GUI is proportional to the screen.
float guiScale(void);

// Priority the gui manager runs at (highest: it drives the ImGui frame after all
// other systems' postUpdate, before the renderer submits the frame).
extern int guiPriority;

ImFont* guiGetBodyFont(void);
ImFont* guiGetTitleFont(void);
// Bold Montserrat (main menu text; the old rcss menu used font-weight 900).
ImFont* guiGetMenuFont(void);
}  // namespace engine::gui
