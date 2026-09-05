// Internal gui split: GuiManager.cpp owns fonts/input/scale + dispatch; the
// backend half (GuiDiligent.cpp) implements the ImGui render path and UI
// textures.

#include "Defines.h"
#include <imgui.h>

namespace engine::gui {

// ── diligent (GuiDiligent.cpp) ──
void guiBackendInitDiligent(void);
void guiBackendDestroyDiligent(void);
void guiBackendFrameDiligent(float dt, u32 width, u32 height, void (*drawGuis)());
ImTextureID guiTextureCreateDiligent(u32 width, u32 height, u8* rgbaPixels);
void guiTextureDestroyDiligent(ImTextureID texture);

}  // namespace engine::gui

// records the UI pass into the diligent frame (called by the renderer hook).
// NOTE: Diligent must stay at global scope — a forward declaration inside
// engine::gui would shadow the real namespace for every engine::gui TU.
namespace Diligent {
struct IDeviceContext;
}

namespace engine::gui {
void guiDiligentDraw(Diligent::IDeviceContext* ctx);

}  // namespace engine::gui
