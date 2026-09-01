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
