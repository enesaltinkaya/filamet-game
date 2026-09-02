#include "gui/GuiBackend.h"

#include "Utils.h"
#include "logger/Logger.h"
#include "renderer/filament/FilamentRenderer.h"

#include <filament/Engine.h>
#include <filament/Texture.h>
#include <filagui/ImGuiHelper.h>

namespace engine::gui {

using engine::renderer::filament_globals::engine;
using engine::renderer::filament_globals::uiView;

static filagui::ImGuiHelper* helper = nullptr;

// keeps external (pak PNG) textures alive until the backend is destroyed
static std::vector<filament::Texture*> uiTextures;

void guiBackendInitFilament(void) {
    // Empty font path: the helper builds its glyph atlas from the fonts we
    // already added above (it skips its own AddFontFromFileTTF).
    helper = new filagui::ImGuiHelper(engine, uiView, utils::Path(), ImGui::GetCurrentContext());
}

void guiBackendDestroyFilament(void) {
    if (helper) {
        delete helper;  // also destroys the ImGui context
        helper = nullptr;
    }
    uiTextures.clear();
}

void guiBackendFrameFilament(float dt, u32 width, u32 height, void (*drawGuis)()) {
    helper->setDisplaySize((int)width, (int)height, 1.0f, 1.0f, false);
    helper->render(dt, [&](filament::Engine*, filament::View*) { drawGuis(); });
}

ImTextureID guiTextureCreateFilament(u32 width, u32 height, u8* rgbaPixels) {
    if (!engine || !rgbaPixels) {
        free(rgbaPixels);
        return ImTextureID_Invalid;
    }

    // The Vulkan backend uploads the image on the engine loop thread, so the
    // descriptor owns the pixels and frees them only after the driver has
    // consumed them.
    filament::Texture::PixelBufferDescriptor pb =
            filament::Texture::PixelBufferDescriptor::make(rgbaPixels, (size_t)width * height * 4,
                    filament::Texture::Format::RGBA, filament::Texture::Type::UBYTE,
                    [](void* b, size_t) { free(b); });
    filament::Texture* tex = filament::Texture::Builder()
            .width(width)
            .height(height)
            .levels(1)
            .format(filament::Texture::InternalFormat::RGBA8)
            .sampler(filament::Texture::Sampler::SAMPLER_2D)
            .build(*engine);
    if (!tex) {
        return ImTextureID_Invalid;
    }
    tex->setImage(*engine, 0, std::move(pb));

    // filagui renders ImDrawCmd::GetTexID() as a filament::Texture*; keep it
    // alive for the lifetime of the gui backend
    uiTextures.push_back(tex);
    return ImTextureID((size_t)tex);
}

void guiTextureDestroyFilament(ImTextureID texture) {
    if (texture == ImTextureID_Invalid || !engine) {
        return;
    }
    auto* tex = (filament::Texture*)(size_t)texture;
    for (size_t i = 0; i < uiTextures.size(); i++) {
        if (uiTextures[i] == tex) {
            uiTextures.erase(uiTextures.begin() + (i64)i);
            break;
        }
    }
    engine->destroy(tex);
}

}  // namespace engine::gui
