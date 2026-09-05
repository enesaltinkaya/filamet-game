#include "gui/GuiManager.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "ecs/Ecs.h"
#include "gui/GuiBackend.h"
#include "logger/Logger.h"
#include "renderer/Renderer.h"
#include "renderer/Window.h"

#include <SDL.h>

namespace engine::gui {
int guiPriority = 10000;  // highest: drive the ImGui frame after all other systems

static ImGuiContext* ctx = nullptr;
static char guiActive = 0;
static float curScale = 1.0f;
static ImFont* fontBody = nullptr;
static ImFont* fontTitle = nullptr;
static ImFont* fontMenu = nullptr;
static ImFont* fontMono = nullptr;

ImFont* guiGetBodyFont(void) { return fontBody; }
ImFont* guiGetTitleFont(void) { return fontTitle; }
ImFont* guiGetMenuFont(void) { return fontMenu; }
ImFont* guiGetMonoFont(void) { return fontMono; }
float guiScale(void) { return curScale; }

// Load a font from the pak into ImGui's atlas. ImGui takes ownership of the
// buffer and frees it on atlas destruction.
static ImFont* loadPakFont(const char* path, float sizePx) {
    // Missing fonts fall back to ImGui's default instead of hard-terminating
    // the process (dataManagerGetSize terminates when the path is in no pak —
    // a font is cosmetic, not a reason to kill the game).
    if (!utils::dataManagerFileExists(path)) {
        utils::warn("gui: font not found '%s' (using ImGui default)", path);
        return nullptr;
    }
    u32 size = utils::dataManagerGetSize(path);
    if (size == 0) {
        utils::error("gui: cannot read font '%s'", path);
        return nullptr;
    }
    void* buf = malloc(size);
    utils::dataManagerReadChunk(path, buf, 0, size);
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(buf, (int)size, sizePx);
}

// Feed the engine's captured input into ImGui (event API, per ImGui >= 1.87).
static void feedInput(ImGuiIO& io) {
    io.AddMousePosEvent(input.mouseX, input.mouseY);
    if (input.mousePressed >= 0) io.AddMouseButtonEvent(input.mousePressed, true);
    if (input.mouseReleased >= 0) io.AddMouseButtonEvent(input.mouseReleased, false);
    if (input.scrollY != 0.0f) io.AddMouseWheelEvent(0.0f, input.scrollY);

    auto key = [&](SDL_Scancode sc, ImGuiKey k) { io.AddKeyEvent(k, engine::input.keys[sc] != 0); };
    key(SDL_SCANCODE_ESCAPE, ImGuiKey_Escape);
    key(SDL_SCANCODE_RETURN, ImGuiKey_Enter);
    key(SDL_SCANCODE_KP_ENTER, ImGuiKey_KeypadEnter);
    key(SDL_SCANCODE_SPACE, ImGuiKey_Space);
    key(SDL_SCANCODE_TAB, ImGuiKey_Tab);
    key(SDL_SCANCODE_BACKSPACE, ImGuiKey_Backspace);
    key(SDL_SCANCODE_DELETE, ImGuiKey_Delete);
    key(SDL_SCANCODE_UP, ImGuiKey_UpArrow);
    key(SDL_SCANCODE_DOWN, ImGuiKey_DownArrow);
    key(SDL_SCANCODE_LEFT, ImGuiKey_LeftArrow);
    key(SDL_SCANCODE_RIGHT, ImGuiKey_RightArrow);
    key(SDL_SCANCODE_LSHIFT, ImGuiKey_LeftShift);
    key(SDL_SCANCODE_RSHIFT, ImGuiKey_RightShift);
    key(SDL_SCANCODE_LCTRL, ImGuiKey_LeftCtrl);
    key(SDL_SCANCODE_RCTRL, ImGuiKey_RightCtrl);
    key(SDL_SCANCODE_LALT, ImGuiKey_LeftAlt);
    key(SDL_SCANCODE_RALT, ImGuiKey_RightAlt);
    for (int i = 0; i < 26; i++) {
        key((SDL_Scancode)(SDL_SCANCODE_A + i), (ImGuiKey)(ImGuiKey_A + i));
    }
    for (int i = 0; i < 10; i++) {
        key((SDL_Scancode)(SDL_SCANCODE_0 + i), (ImGuiKey)(ImGuiKey_0 + i));
    }

    if (input.text[0]) io.AddInputCharactersUTF8(input.text);
}

static std::vector<Gui*> activeGuis;

static void drawActiveGuis(void) {
    for (Gui* g : activeGuis) g->draw();
}

class GuiManagerSystem : public System {
public:
    GuiManagerSystem() : System("gui") {}

    void removed() override {
        // the backend owns the ImGui context teardown
        guiBackendDestroyDiligent();
        ctx = nullptr;
        fontBody = nullptr;
        fontTitle = nullptr;
        fontMenu = nullptr;
        fontMono = nullptr;
        guiActive = 0;
    }

    void postUpdate() override {
        if (!ctx) {
            guiActive = 0;
            return;
        }

        // Active guis = the Gui systems currently in the ecs (fresh each frame,
        // so it always reflects the deferred add/remove applied this frame).
        activeGuis.clear();
        for (System* s : ecs.systems) {
            if (Gui* g = dynamic_cast<Gui*>(s)) activeGuis.push_back(g);
        }
        guiActive = !activeGuis.empty() ? 1 : 0;
        if (!guiActive) return;

        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();

        // Scale UI with the framebuffer resolution (1x at 720p). GUI layout code
        // uses guiScale() for positions/sizes; this scales the glyph rendering.
        curScale = (float)window.height / 720.0f;
        if (curScale < 1.0f) curScale = 1.0f;
        if (curScale > 3.0f) curScale = 3.0f;
        ImGui::GetStyle().FontScaleMain = curScale;

        feedInput(io);

        // NewFrame + emit every gui's widgets + Render; the renderer submits
        // the UI pass afterward (imgui vulkan pass)
        // Real frame time, not utils::timer.dt: this runs once per rendered
        // frame in postUpdate, while timer.dt is the fixed 1/UPS simulation
        // tick (ImGui would animate 2x fast at 120fps). frameTime is already
        // clamped to 250ms against hitches.
        guiBackendFrameDiligent((float)(utils::timer.frameTime / BILLION), window.width, window.height,
                drawActiveGuis);
    }
};

static GuiManagerSystem guiManager;

void guiInit(void) {
    utils::info("gui: initializing ImGui backend (vulkan)");

    ctx = ImGui::CreateContext();
    // Fonts: montserrat.ttf is the raw variable font (default instance =
    // Thin, wght 100-900). Dear ImGui cannot pick a weight from a VF, so the
    // menu/body weights ship as static instances montserratLight.ttf (300)
    // and montserratBlack.ttf (900), instanced from that same VF with
    // fontTools (scripts and provenance in plans/azgaar-terrain.md phase-7
    // cleanup). If a static instance is ever missing, loadPakFont falls back
    // to ImGui's default font (deliberate: cosmetic, must not crash).
    // Rasterized 3x then drawn scaled back down: FontScaleMain
    // (= window.height/720, window defaults to 75% of the display) magnifies
    // the baked glyphs at draw time, and a linearly-magnified 14px glyph is
    // what made the new engine's text look fuzzy next to the old
    // RMLUI/freetype pipeline, which rasterized at the exact on-screen size.
    // 3x oversampling keeps the on-screen size identical while sampling a
    // 3x-resolution raster. ImGui bakes the atlas at the requested px and
    // applies ImFont::Scale to glyph metrics at draw time.
    static constexpr float kFontOversample = 3.0f;
    fontBody  = loadPakFont("fonts/montserratLight.ttf", 18.0f * kFontOversample);
    fontTitle = loadPakFont("fonts/montserrat.ttf", 42.0f * kFontOversample);
    // The old rcss menu requested font-weight 900; RMLUI's freetype font
    // engine registered one face per named instance, so the old menu
    // rendered Montserrat Black (900) for menu rows and Light (300) for
    // body text — the static instances above reproduce that.
    fontMenu = loadPakFont("fonts/montserratBlack.ttf", 18.0f * kFontOversample);
    // Sometype Mono: the old engine's debug-readout face (camera/stats gui)
    fontMono = loadPakFont("fonts/SometypeMono-SemiBold.ttf", 14.0f * kFontOversample);
    for (ImFont* f : {fontBody, fontTitle, fontMenu, fontMono})
        if (f) f->Scale = 1.0f / kFontOversample;

    guiBackendInitDiligent();

    systemAdd(guiPriority, &guiManager);
}

void guiDestroy(void) {
    for (System* s : ecs.systems) {
        if (s == &guiManager) {
            systemRemove(&guiManager);
            return;
        }
    }
}

void guiAdd(Gui* gui) {
    ecsSystemAddDeferred(guiPriority - 1, gui);  // below the manager, so it draws first
}

void guiRemove(Gui* gui) {
    ecsSystemRemoveDeferred(gui);
}

bool guiIsActive(void) {
    return guiActive != 0;
}

ImTextureID guiTextureCreate(u32 width, u32 height, u8* rgbaPixels) {
    return guiTextureCreateDiligent(width, height, rgbaPixels);
}

void guiTextureDestroy(ImTextureID texture) {
    guiTextureDestroyDiligent(texture);
}
}  // namespace engine::gui
