
#include "Defines.h"
#include <imgui.h>

namespace engine::gui {

void guiBackendInitDiligent(void);
void guiBackendDestroyDiligent(void);
void guiBackendFrameDiligent(float dt, u32 width, u32 height, void (*drawGuis)());
ImTextureID guiTextureCreateDiligent(u32 width, u32 height, u8* rgbaPixels);
void guiTextureDestroyDiligent(ImTextureID texture);

}

// NOTE: Diligent must stay at global scope — a forward declaration inside
// engine::gui would shadow the real namespace for every engine::gui TU.
namespace Diligent {
struct IDeviceContext;
}

namespace engine::gui {
void guiDiligentDraw(Diligent::IDeviceContext* ctx);

}
