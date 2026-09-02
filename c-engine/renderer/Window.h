#pragma once

#include "Defines.h"

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

void windowSetRelativeMouseMode(char on);  // true: relative mode + cursor hidden
void windowHideCursor(void);
void windowShowCursor(void);
}  // namespace engine
