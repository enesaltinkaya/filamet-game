#include "Window.h"
#include "Engine.h"
#include "Utils.h"
#include "logger/Logger.h"
#include <SDL.h>
#include <SDL_syswm.h>

namespace engine {
Window window = {};

bool windowCreate(const char* title, u32 width, u32 height) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        utils::error("window: SDL_Init failed (%s)", SDL_GetError());
        return false;
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
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                engineStop();
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
}
}  // namespace engine
