#include "gui/GuiManager.h"
#include "renderer/Renderer.h"
#include "renderer/Window.h"
#include "ecs/Ecs.h"
#include "Utils.h"
#include "datamanager/DataManager.h"
#include "logger/Logger.h"

#include <imgui.h>
#include <filagui/ImGuiHelper.h>

#include <SDL.h>

namespace engine::gui {
int guiPriority = 10000;  // highest: drive the ImGui frame after all other systems

static filagui::ImGuiHelper* helper = nullptr;
static ImGuiContext* ctx = nullptr;
static char guiActive = 0;
static float curScale = 1.0f;
static ImFont* fontBody = nullptr;
static ImFont* fontTitle = nullptr;
static ImFont* fontMenu = nullptr;

ImFont* guiGetBodyFont(void) { return fontBody; }
ImFont* guiGetTitleFont(void) { return fontTitle; }
ImFont* guiGetMenuFont(void) { return fontMenu; }
float guiScale(void) { return curScale; }

// Load a font from the pak into ImGui's atlas. ImGui takes ownership of the
// buffer and frees it on atlas destruction.
static ImFont* loadPakFont(const char* path, float sizePx) {
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

class GuiManagerSystem : public System {
public:
    GuiManagerSystem() : System("gui") {}

    void removed() override {
        if (helper) {
            delete helper;  // also destroys the ImGui context
            helper = nullptr;
        }
        ctx = nullptr;
        fontBody = nullptr;
        fontTitle = nullptr;
        fontMenu = nullptr;
        guiActive = 0;
    }

    void postUpdate() override {
        if (!helper) {
            guiActive = 0;
            return;
        }

        // Active guis = the Gui systems currently in the ecs (fresh each frame,
        // so it always reflects the deferred add/remove applied this frame).
        std::vector<Gui*> guis;
        for (System* s : ecs.systems) {
            if (Gui* g = dynamic_cast<Gui*>(s)) guis.push_back(g);
        }
        guiActive = !guis.empty() ? 1 : 0;
        if (!guiActive) return;

        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        helper->setDisplaySize((int)window.width, (int)window.height, 1.0f, 1.0f, false);

        // Scale UI with the framebuffer resolution (1x at 720p). GUI layout code
        // uses guiScale() for positions/sizes; this scales the glyph rendering.
        curScale = (float)window.height / 720.0f;
        if (curScale < 1.0f) curScale = 1.0f;
        if (curScale > 3.0f) curScale = 3.0f;
        ImGui::GetStyle().FontScaleMain = curScale;

        feedInput(io);

        // NewFrame + emit every gui's widgets + Render + build the Filament
        // renderables. The renderer submits the UI view afterward.
        helper->render((float)utils::timer.dt, [&](filament::Engine*, filament::View*) {
            for (Gui* g : guis) g->draw();
        });
    }
};

static GuiManagerSystem guiManager;

void guiInit(void) {
    utils::info("gui: initializing ImGui/filagui backend");

    ctx = ImGui::CreateContext();
    fontBody = loadPakFont("fonts/montserratLight.ttf", 18.0f);
    fontTitle = loadPakFont("fonts/montserrat.ttf", 42.0f);
    // The old rcss menu requested font-weight 900. RMLUI's freetype font
    // engine enumerated the variable font's named instances and registered
    // one face per weight, so the old menu rendered Montserrat Black (900);
    // the body text used weight 300 (Light). Dear ImGui renders variable
    // fonts at their default instance (Thin), so we load static Black/Light
    // instances instead of the raw VF.
    fontMenu = loadPakFont("fonts/montserratBlack.ttf", 18.0f);

    // Empty font path: the helper builds its glyph atlas from the fonts we
    // already added above (it skips its own AddFontFromFileTTF).
    helper = new filagui::ImGuiHelper(renderer::filamentEngine, renderer::uiView, utils::Path(), ctx);

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
}  // namespace engine::gui
