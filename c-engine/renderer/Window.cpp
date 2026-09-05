#include "Window.h"
#include "Engine.h"
#include "Utils.h"
#include "logger/Logger.h"
#include <SDL.h>

#include <cstdlib>

namespace engine {
Window window = {};
Input input = {};

static char relativeMouse = 0;

// Cursor support (SDL system cursors). arrow/hand are handed to the crmlui
// wrapper; text is a system cursor. Tracked so windowIsCursorVisible() can
// gate GUI input forwarding.
static SDL_Cursor* cursorArrow = nullptr;
static SDL_Cursor* cursorHand = nullptr;
static SDL_Cursor* cursorText = nullptr;
static bool cursorVisible = true;

// Frame-to-frame state for synthesizing the InputEvent stream.
static float shimPrevMouseX = 0.0f;
static float shimPrevMouseY = 0.0f;
static u32 shimPrevWidth = 0;
static u32 shimPrevHeight = 0;

static void windowSynthesizeInputEvents(void);

// automated runs (screenshot / renderdoc capture): create the window hidden so
// nothing ever appears on screen — rendering still works, the swapchain just
// presents to an unmapped window. Same gating as rendererInit in Renderer.cpp
// (renderdoc is debug-only there).
static bool automatedHiddenRun(void) {
    const char* screenshot = getenv("ENGINE_SCREENSHOT");
    if (screenshot && screenshot[0] != '\0') {
        return true;
    }
#ifndef NDEBUG
    if (getenv("ENGINE_RENDERDOC_CAPTURE")) {
        return true;
    }
#endif
    return false;
}

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
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    bool hidden = automatedHiddenRun();
    if (hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    window.handle = SDL_CreateWindow(title, (int)width, (int)height, flags);
    if (!window.handle) {
        utils::error("window: SDL_CreateWindow failed (%s)", SDL_GetError());
        SDL_Quit();
        return false;
    }

    window.width = width;
    window.height = height;

    // Old-engine parity: uiScale 0 means "auto" — seed the persisted setting
    // with the display scale once (settings are already loaded in utilsInit);
    // guiManagerScale() then reads it every frame. cursorScale (world-cursor
    // scaling) is skipped on wayland like the old engine.
    if (utils::settingsGetDouble("uiScale") <= 0.0) {
        double scale = (double)SDL_GetWindowDisplayScale(window.handle);
        utils::settingsSetDouble("uiScale", scale);
        if (getenv("WAYLAND_DISPLAY") == nullptr) {
            utils::settingsSetDouble("cursorScale", scale);
        }
        utils::settingsWrite();
        utils::info("window: uiScale was 0, set to display scale %g", scale);
    }

    windowLoadCursors();
    utils::info("window: created %u x %u%s", width, height, hidden ? " (hidden)" : "");
    return true;
}

void windowDestroy(void) {
    windowDestroyCursors();
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
    input.mousePressed = -1;
    input.mouseReleased = -1;
    input.text[0] = 0;
    int textLen = 0;

    // TEMP VERIFY (removed after): synthetic one-shot ESC presses on the
    // rendered frames in ENGINE_FAKE_ESC_FRAMES="300,400,500" — simulates
    // physical presses (one event each; holds are now just a single
    // press, SDL repeat filtered below).
    static unsigned long fakeEsc[16];
    static int fakeEscN = -1;
    if (fakeEscN < 0) {
        fakeEscN = 0;
        if (const char* v = getenv("ENGINE_FAKE_ESC_FRAMES")) {
            char buf[128];
            snprintf(buf, sizeof buf, "%s", v);
            for (char* c = strtok(buf, ","); c && fakeEscN < 16; c = strtok(nullptr, ","))
                fakeEsc[fakeEscN++] = strtoull(c, nullptr, 10);
        }
    }
    for (int i = 0; i < fakeEscN; i++)
        if (fakeEsc[i] == utils::timer.frameCounter) {
            input.pressed = (i32)SDL_SCANCODE_ESCAPE;
            utils::debug("ESC-DEBUG fake esc injected frame=%llu", (unsigned long long)utils::timer.frameCounter);
        }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                engineStop();
                break;
            case SDL_EVENT_KEY_DOWN:
                // SDL delivers repeated KEY_DOWNs while a key is held. Those
                // are not presses: every input.pressed / input.events
                // KEY_DOWN consumer is edge-triggered (the pause menu used to
                // reopen and re-close on every repeat of a held ESC), so only
                // the initial press counts.
                if (event.key.repeat) break;
                if (event.key.scancode == SDL_SCANCODE_ESCAPE)
                    utils::debug("ESC-DEBUG SDL KEY_DOWN esc (non-repeat) frame=%llu", (unsigned long long)utils::timer.frameCounter);
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
            case SDL_EVENT_MOUSE_MOTION:
                input.mouseX = event.motion.x;
                input.mouseY = event.motion.y;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                int b = (event.button.button == SDL_BUTTON_LEFT) ? 0 :
                        (event.button.button == SDL_BUTTON_RIGHT) ? 1 :
                        (event.button.button == SDL_BUTTON_MIDDLE) ? 2 : -1;
                if (b >= 0) {
                    if (event.button.down) input.mousePressed = b;
                    else input.mouseReleased = b;
                    input.mouseX = event.button.x;
                    input.mouseY = event.button.y;
                }
                break;
            }
            case SDL_EVENT_TEXT_INPUT: {
                int n = (int)SDL_strlen(event.text.text);
                if (textLen + n < (int)sizeof input.text - 1) {
                    memcpy(input.text + textLen, event.text.text, n);
                    textLen += n;
                    input.text[textLen] = 0;
                }
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                input.scrollY += event.wheel.y;  // float in SDL3
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (window.width != (u32)event.window.data1 || window.height != (u32)event.window.data2) {
                    window.width  = (u32)event.window.data1;
                    window.height = (u32)event.window.data2;
                    utils::info("window: resized %ux%u", window.width, window.height);
                }
                break;
        }
    }

    // absolute cursor position + held buttons (covers state from before focus,
    // and keeps it consistent even without a motion event this frame)
    if (window.handle) {
        float mx = 0.0f, my = 0.0f;
        SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
        input.mouseX = mx;
        input.mouseY = my;
        input.mouseLeft   = (buttons & SDL_BUTTON_LMASK) ? 1 : 0;
        input.mouseRight  = (buttons & SDL_BUTTON_RMASK) ? 1 : 0;
        input.mouseMiddle = (buttons & SDL_BUTTON_MMASK) ? 1 : 0;
    }

    // relative mouse delta: read it as a whole here (not from motion events —
    // the warp-to-center on entering relative mode emits one bogus event)
    if (relativeMouse) {
        float rx, ry;
        SDL_GetRelativeMouseState(&rx, &ry);
        input.mouseDx += rx;
        input.mouseDy += ry;
    }

    // Test hook: synthetic orbit drag (ENGINE_FAKE_DRAG=1) — holds RMB and
    // sweeps yaw so automated runs exercise the interactive drag path (the
    // player system picks the button up and enters its relative-mouse mode).
    if (getenv("ENGINE_FAKE_DRAG")) {
        input.mouseRight = 1;
        input.mouseDx += 1.0f;
    }

    // held key state (covers keys held before the window gained focus, etc.)
    int numkeys = 0;
    const bool* keys = SDL_GetKeyboardState(&numkeys);  // bool in SDL3
    memcpy(input.keys, keys, sizeof input.keys);
    input.ctrl  = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
    input.shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    input.alt   = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];

    // Build the per-frame InputEvent stream from the accumulated state.
    windowSynthesizeInputEvents();
}

void windowSetRelativeMouseMode(char on) {
    if (relativeMouse == on) return;
    relativeMouse = on;
    if (window.handle) {
        SDL_SetWindowRelativeMouseMode(window.handle, on);
        if (on) {
            SDL_HideCursor();
            cursorVisible = false;
        } else {
            SDL_ShowCursor();
            cursorVisible = true;
        }
    }
}

void windowHideCursor(void) {
    cursorVisible = false;
    if (window.handle) SDL_HideCursor();
}

void windowShowCursor(void) {
    cursorVisible = true;
    if (window.handle) SDL_ShowCursor();
}

void windowToggleFullscreen(char on) {
    if (!window.handle) return;
    // SDL3: SDL_bool is plain bool (the SDL_TRUE/SDL_FALSE old names are not enabled)
    SDL_SetWindowFullscreen(window.handle, on);
    utils::info("window: fullscreen %s", on ? "on" : "off");
}

// ── Cursor support ──────────────────────────────────────────────────────────

void windowLoadCursors(void) {
    windowDestroyCursors();
    cursorArrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    cursorHand  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    cursorText  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    // default to the arrow (the wrapper re-selects per element on hover)
    if (cursorArrow) {
        SDL_SetCursor(cursorArrow);
    }
}

void windowDestroyCursors(void) {
    if (cursorArrow) {
        SDL_DestroyCursor(cursorArrow);
        cursorArrow = nullptr;
    }
    if (cursorHand) {
        SDL_DestroyCursor(cursorHand);
        cursorHand = nullptr;
    }
    if (cursorText) {
        SDL_DestroyCursor(cursorText);
        cursorText = nullptr;
    }
}

void* windowGetArrowCursor(void) {
    return cursorArrow;
}

void* windowGetPointerCursor(void) {
    return cursorHand;
}

void* windowGetTextCursor(void) {
    return cursorText;
}

void windowSetCursor(int cursorType) {
    SDL_Cursor* cursor = cursorArrow;
    switch (cursorType) {
        case 1: cursor = cursorHand; break;  // pointer/hand
        case 2: cursor = cursorText; break;  // text
        default: break;                       // 0=arrow (and any unhandled)
    }
    if (cursor) SDL_SetCursor(cursor);
}

bool windowIsCursorVisible(void) {
    return cursorVisible;
}

// ── InputEvent shim ─────────────────────────────────────────────────────────
// Synthesize the old-engine InputEvent stream (crmlui consumes it via
// rmlSendInputEvent) from the new engine's accumulated input state. The new
// engine collapses per-frame input into single fields, so at most one key-down,
// one key-up and one mouse-button per frame is emitted — enough for GUI
// interaction (hover, click, text, tab/enter/arrows, resize).
KeyCode windowMapScancode(int scancode) {
    switch (scancode) {
        case SDL_SCANCODE_A: return KEY_A;
        case SDL_SCANCODE_B: return KEY_B;
        case SDL_SCANCODE_C: return KEY_C;
        case SDL_SCANCODE_D: return KEY_D;
        case SDL_SCANCODE_E: return KEY_E;
        case SDL_SCANCODE_F: return KEY_F;
        case SDL_SCANCODE_H: return KEY_H;
        case SDL_SCANCODE_M: return KEY_M;
        case SDL_SCANCODE_N: return KEY_N;
        case SDL_SCANCODE_P: return KEY_P;
        case SDL_SCANCODE_R: return KEY_R;
        case SDL_SCANCODE_S: return KEY_S;
        case SDL_SCANCODE_T: return KEY_T;
        case SDL_SCANCODE_W: return KEY_W;
        case SDL_SCANCODE_X: return KEY_X;
        case SDL_SCANCODE_1: return KEY_1;
        case SDL_SCANCODE_2: return KEY_2;
        case SDL_SCANCODE_5: return KEY_5;
        case SDL_SCANCODE_RETURN: return KEY_RETURN;
        case SDL_SCANCODE_ESCAPE: return KEY_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return KEY_BACKSPACE;
        case SDL_SCANCODE_TAB: return KEY_TAB;
        case SDL_SCANCODE_SPACE: return KEY_SPACE;
        case SDL_SCANCODE_F8: return KEY_F8;
        case SDL_SCANCODE_LCTRL: return KEY_LCTRL;
        case SDL_SCANCODE_RCTRL: return KEY_RCTRL;
        case SDL_SCANCODE_LSHIFT: return KEY_LSHIFT;
        case SDL_SCANCODE_RSHIFT: return KEY_RSHIFT;
        case SDL_SCANCODE_LALT: return KEY_LALT;
        case SDL_SCANCODE_RALT: return KEY_RALT;
        case SDL_SCANCODE_UP: return KEY_UP;
        case SDL_SCANCODE_DOWN: return KEY_DOWN;
        case SDL_SCANCODE_LEFT: return KEY_LEFT;
        case SDL_SCANCODE_RIGHT: return KEY_RIGHT;
        case SDL_SCANCODE_KP_ENTER: return KEY_KP_ENTER;
        case SDL_SCANCODE_KP_PLUS: return KEY_KP_PLUS;
        case SDL_SCANCODE_KP_MINUS: return KEY_KP_MINUS;
        case SDL_SCANCODE_DELETE: return KEY_DELETE;
        default: return KEY_NONE;
    }
}

static MouseButton windowMapMouseButton(int button) {
    switch (button) {
        case 0: return MOUSE_BUTTON_LEFT;
        case 1: return MOUSE_BUTTON_RIGHT;
        case 2: return MOUSE_BUTTON_MIDDLE;
        default: return MOUSE_BUTTON_NONE;
    }
}

static void windowSynthesizeInputEvents(void) {
    input.events.clear();
    auto push = [&](InputEvent& ev) {
        ev.ctrl  = input.ctrl;
        ev.shift = input.shift;
        ev.alt   = input.alt;
        input.events.push_back(ev);
    };

    if (input.pressed != 0) {
        InputEvent ev = {};
        ev.type = INPUT_EVENT_KEY_DOWN;
        ev.data.key.key = windowMapScancode(input.pressed);
        if (ev.data.key.key != KEY_NONE) push(ev);
    }

    if (input.released != 0) {
        InputEvent ev = {};
        ev.type = INPUT_EVENT_KEY_UP;
        ev.data.key.key = windowMapScancode(input.released);
        if (ev.data.key.key != KEY_NONE) push(ev);
    }

    if (input.mouseX != shimPrevMouseX || input.mouseY != shimPrevMouseY ||
        input.mouseDx != 0.0f || input.mouseDy != 0.0f) {
        InputEvent ev = {};
        ev.type = INPUT_EVENT_MOUSE_MOVE;
        ev.data.motion.x = input.mouseX;
        ev.data.motion.y = input.mouseY;
        ev.data.motion.dx = input.mouseDx;
        ev.data.motion.dy = input.mouseDy;
        input.events.push_back(ev);
    }

    if (input.mousePressed >= 0) {
        InputEvent ev = {};
        ev.type = INPUT_EVENT_MOUSE_BUTTON_DOWN;
        ev.data.mouseButton.button = windowMapMouseButton(input.mousePressed);
        push(ev);
    }

    if (input.mouseReleased >= 0) {
        InputEvent ev = {};
        ev.type = INPUT_EVENT_MOUSE_BUTTON_UP;
        ev.data.mouseButton.button = windowMapMouseButton(input.mouseReleased);
        push(ev);
    }

    if (input.scrollY != 0.0f) {
        InputEvent ev = {};
        ev.type = INPUT_EVENT_MOUSE_WHEEL;
        ev.data.wheel.x = 0.0f;
        ev.data.wheel.y = input.scrollY;
        input.events.push_back(ev);
    }

    if (input.text[0] != '\0') {
        InputEvent ev = {};
        ev.type = INPUT_EVENT_TEXT_INPUT;
        strncpy(ev.data.text.text, input.text, sizeof ev.data.text.text - 1);
        ev.data.text.text[sizeof ev.data.text.text - 1] = '\0';
        input.events.push_back(ev);
    }

    if (window.width != shimPrevWidth || window.height != shimPrevHeight) {
        InputEvent ev = {};
        ev.type = INPUT_EVENT_WINDOW_RESIZED;
        ev.data.resize.width = (int)window.width;
        ev.data.resize.height = (int)window.height;
        input.events.push_back(ev);
    }

    shimPrevMouseX = input.mouseX;
    shimPrevMouseY = input.mouseY;
    shimPrevWidth = window.width;
    shimPrevHeight = window.height;
}
}  // namespace engine
