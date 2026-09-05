#pragma once

#include "Defines.h"
#include "shared/InputEventShared.h"

#include <vector>

struct SDL_Window;

namespace engine {
struct Input {
    char keys[256] = {};  // held keyboard state (SDL scancodes)
    i32 pressed = 0;      // scancode pressed this frame
    i32 released = 0;     // scancode released this frame
    char ctrl = 0, shift = 0, alt = 0;

    float mouseDx = 0.0f;  // relative mouse delta accumulated this frame
    float mouseDy = 0.0f;
    float scrollY = 0.0f;

    // absolute mouse + buttons + text (drives the GUI / ImGui)
    float mouseX = 0.0f, mouseY = 0.0f;      // absolute cursor position (pixels)
    char mouseLeft = 0, mouseRight = 0, mouseMiddle = 0;  // held button state
    i32 mousePressed = -1, mouseReleased = -1;            // button idx (0/1/2) this frame
    char text[256] = {};                                   // UTF-8 text input this frame

    // Per-frame stream of old-engine-style InputEvents synthesized from the
    // accumulated state above (see windowSynthesizeInputEvents). crmlui's input
    // callbacks consume this (rmlSendInputEvent per event).
    std::vector<InputEvent> events;
};

struct Window {
    SDL_Window* handle;
    u32 width;
    u32 height;
};

extern Window window;

bool windowCreate(const char* title, u32 width, u32 height);
void windowDestroy(void);
void* windowNativeHandle(void);  // X11 Window (Linux) / HWND (Windows)
void windowPollEvents(void);     // pumps events; engineStop() on window close

extern struct Input input;

// SDL scancode -> old-engine KeyCode (0 = KEY_NONE for unmapped keys); the
// rmlui gui manager uses it for its Ctrl+letter toggles.
KeyCode windowMapScancode(int scancode);

void windowSetRelativeMouseMode(char on);  // true: relative mode + cursor hidden
void windowHideCursor(void);
void windowShowCursor(void);

/* Toggle desktop fullscreen (the video settings' Fullscreen row). The size
 * change flows through the normal resize pipeline: SDL posts WINDOW_RESIZED,
 * the renderer re-sizes the swapchain, and the rmlui manager gets its
 * INPUT_EVENT_WINDOW_RESIZED (rmlSetDimensions). */
void windowToggleFullscreen(char on);

// Cursor support (SDL system cursors) — the pointers are passed to the
// crmlui wrapper (RmlParams.window) and windowSetCursor is used as its
// set-cursor callback (0=arrow, 1=pointer/hand, 2=text, 3=move, 4=cross,
// 5=resize, 6=unavailable).
void windowLoadCursors(void);
void windowDestroyCursors(void);
void* windowGetArrowCursor(void);
void* windowGetPointerCursor(void);  // pointing hand
void* windowGetTextCursor(void);
void windowSetCursor(int cursorType);
bool windowIsCursorVisible(void);
}  // namespace engine
