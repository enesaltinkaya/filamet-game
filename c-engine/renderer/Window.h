#pragma once

#include "Defines.h"

struct SDL_Window;

namespace engine {
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
}  // namespace engine
