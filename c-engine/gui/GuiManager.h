#pragma once

#include "gui/Gui.h"

#include <imgui.h>

struct ImFont;  // imgui type; declared in the global namespace (as in imgui.h)

namespace engine::gui {
// Initialize the ImGui backend (filagui on filament, imgui_impl_vulkan on
// diligent) on the active renderer. Must run after rendererInit and after the
// data manager is ready (loads the UI font from the pak).
void guiInit(void);
void guiDestroy(void);

// Show / hide a gui. Applied on the next frame via the deferred system queue,
// so it is safe to call from inside a gui's own draw() (a button click).
void guiAdd(Gui* gui);
void guiRemove(Gui* gui);

// True when at least one gui is active this frame. The renderer uses this to
// decide whether to draw the UI overlay pass.
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
// Sometype Mono (debug readouts; the old engine's rcss debug documents —
// camera, stats — were all set in "Sometype Mono").
ImFont* guiGetMonoFont(void);

// Upload an RGBA8 UI image (top-down rows); the backend takes ownership of the
// pixel buffer. Returns an ImTextureID for ImGui draw commands, or
// ImTextureID_Invalid on failure.
ImTextureID guiTextureCreate(u32 width, u32 height, u8* rgbaPixelsTakeOwnership);
void guiTextureDestroy(ImTextureID texture);
}  // namespace engine::gui
