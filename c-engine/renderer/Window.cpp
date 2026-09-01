#include "Window.h"
#include "Engine.h"
#include "Utils.h"
#include "logger/Logger.h"
#include <SDL.h>
#include <SDL_syswm.h>

namespace engine {
Window window = {};
Input input = {};

static char relativeMouse = 0;

bool windowCreate(const char* title, u32 width, u32 height) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        utils::error("window: SDL_Init failed (%s)", SDL_GetError());
        return false;
    }

    // 0x0 → default to 75% of the primary display (16:9), like the old engine
    if (width == 0 || height == 0) {
        width  = 1280;
        height = 720;
        SDL_DisplayMode mode = {};
        if (SDL_GetDesktopDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0) {
            width  = (u32)(mode.w * 0.75f);
            height = (u32)(width / 1.77f);
        }
    }

    window.handle = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)width, (int)height,
                                     SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
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

    SDL_SysWMinfo wmi;
    SDL_VERSION(&wmi.version);
    if (!SDL_GetWindowWMInfo(window.handle, &wmi)) {
        utils::error("window: SDL_GetWindowWMInfo failed (%s)", SDL_GetError());
        return nullptr;
    }

    if (wmi.subsystem == SDL_SYSWM_X11) {
        return (void*)(uintptr_t)wmi.info.x11.window;
    }
#ifdef _WIN32
    if (wmi.subsystem == SDL_SYSWM_WINDOWS) {
        return wmi.info.win.window;
    }
#endif
    utils::error("window: unsupported window subsystem");
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
            case SDL_QUIT:
                engineStop();
                break;
            case SDL_KEYDOWN:
                input.pressed = (i32)event.key.keysym.scancode;
                // Alt+E exits the game
                if (event.key.keysym.scancode == SDL_SCANCODE_E &&
                    (SDL_GetModState() & KMOD_ALT)) {
                    engineStop();
                }
                break;
            case SDL_KEYUP:
                input.released = (i32)event.key.keysym.scancode;
                break;
            case SDL_MOUSEWHEEL:
                input.scrollY += (float)event.wheel.y;
                break;
            case SDL_WINDOWEVENT:
                switch (event.window.event) {
                    case SDL_WINDOWEVENT_RESIZED:
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                        window.width = (u32)event.window.data1;
                        window.height = (u32)event.window.data2;
                        break;
                }
                break;
        }
    }

    // relative mouse delta: read it as a whole here (not from motion events —
    // the warp-to-center on entering relative mode emits one bogus event)
    if (relativeMouse) {
        int rx, ry;
        SDL_GetRelativeMouseState(&rx, &ry);
        input.mouseDx += (float)rx;
        input.mouseDy += (float)ry;
    }

    // held key state (covers keys held before the window gained focus, etc.)
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    memcpy(input.keys, keys, sizeof input.keys);
    input.ctrl  = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
    input.shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    input.alt   = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
}

void windowSetRelativeMouseMode(char on) {
    if (relativeMouse == on) return;
    relativeMouse = on;
    if (window.handle) {
        SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
        SDL_ShowCursor(on ? SDL_DISABLE : SDL_ENABLE);
    }
}

void windowHideCursor(void) {
    if (window.handle) SDL_ShowCursor(SDL_DISABLE);
}

void windowShowCursor(void) {
    if (window.handle) SDL_ShowCursor(SDL_ENABLE);
}
}  // namespace engine
