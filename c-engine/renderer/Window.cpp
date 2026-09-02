#include "Window.h"
#include "Engine.h"
#include "Utils.h"
#include "logger/Logger.h"
#include <SDL.h>

namespace engine {
Window window = {};
Input input = {};

static char relativeMouse = 0;

bool windowCreate(const char* title, u32 width, u32 height) {
    // SDL3: SDL_Init returns bool (true = success), the SDL2 '!= 0' check is inverted
    if (!SDL_Init(SDL_INIT_VIDEO)) {  // SDL3: events are implicit in SDL_INIT_VIDEO
        utils::error("window: SDL_Init failed (%s)", SDL_GetError());
        return false;
    }

    // 0x0 → default to 75% of the primary display (16:9), like the old engine
    if (width == 0 || height == 0) {
        width  = 1280;
        height = 720;
        SDL_Rect bounds = {};
        if (SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &bounds) && bounds.w > 0 && bounds.h > 0) {
            width  = (u32)(bounds.w * 0.75f);
            height = (u32)(width / 1.77f);
        }
    }

    // SDL3: no position params (window is centered), shown by default, no ALLOW_HIGHDPI (always on)
    window.handle = SDL_CreateWindow(title, (int)width, (int)height,
                                     SDL_WINDOW_RESIZABLE);
    if (!window.handle) {
        utils::error("window: SDL_CreateWindow failed (%s)", SDL_GetError());
        SDL_Quit();
        return false;
    }

    window.width = width;
    window.height = height;
    utils::info("window: created %u x %u", width, height);
    return true;
}

void windowDestroy(void) {
    if (window.handle) {
        SDL_DestroyWindow(window.handle);
        window.handle = nullptr;
    }
    SDL_Quit();
    utils::info("window: destroyed");
}

void* windowNativeHandle(void) {
    if (!window.handle) {
        return nullptr;
    }

    // SDL3: the old SDL_SysWMinfo is gone — the window's platform handle is
    // exposed as a window property instead
    SDL_PropertiesID props = SDL_GetWindowProperties(window.handle);
#ifdef _WIN32
    void* hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (hwnd) {
        return hwnd;
    }
#else
    Sint64 xwindow = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (xwindow != 0) {
        return (void*)(uintptr_t)xwindow;
    }
#endif
    utils::error("window: no native window handle available");
    return nullptr;
}

void windowPollEvents(void) {
    // one-shot input fields: fresh per frame
    input.pressed = 0;
    input.released = 0;
    input.mouseDx = 0.0f;
    input.mouseDy = 0.0f;
    input.scrollY = 0.0f;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                engineStop();
                break;
            case SDL_EVENT_KEY_DOWN:
                input.pressed = (i32)event.key.scancode;
                // Alt+E exits the game
                if (event.key.scancode == SDL_SCANCODE_E &&
                    (SDL_GetModState() & SDL_KMOD_ALT)) {
                    engineStop();
                }
                break;
            case SDL_EVENT_KEY_UP:
                input.released = (i32)event.key.scancode;
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                input.scrollY += event.wheel.y;  // float in SDL3
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                window.width = (u32)event.window.data1;
                window.height = (u32)event.window.data2;
                break;
        }
    }

    // relative mouse delta: read it as a whole here (not from motion events —
    // the warp-to-center on entering relative mode emits one bogus event)
    if (relativeMouse) {
        float rx, ry;
        SDL_GetRelativeMouseState(&rx, &ry);
        input.mouseDx += rx;
        input.mouseDy += ry;
    }

    // held key state (covers keys held before the window gained focus, etc.)
    int numkeys = 0;
    const bool* keys = SDL_GetKeyboardState(&numkeys);  // bool in SDL3
    memcpy(input.keys, keys, sizeof input.keys);
    input.ctrl  = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
    input.shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    input.alt   = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
}

void windowSetRelativeMouseMode(char on) {
    if (relativeMouse == on) return;
    relativeMouse = on;
    if (window.handle) {
        SDL_SetWindowRelativeMouseMode(window.handle, on);
        if (on) {
            SDL_HideCursor();
        } else {
            SDL_ShowCursor();
        }
    }
}

void windowHideCursor(void) {
    if (window.handle) SDL_HideCursor();
}

void windowShowCursor(void) {
    if (window.handle) SDL_ShowCursor();
}
}  // namespace engine
