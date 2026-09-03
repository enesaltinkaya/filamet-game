#include "ecs/system/heightmap/HeightmapTerrainRender.h"

#include "logger/Logger.h"
#include "renderer/Renderer.h"
#include "renderer/filament/HeightmapTerrainFilament.h"

namespace engine {

void heightmapTerrainRenderRegisterLook(const HeightmapTerrainLook* look) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        heightmapTerrainFilamentRegisterLook(look);
    }
    // phase 6 (plans/azgaar-terrain.md): diligent half
}

void heightmapTerrainRenderReleaseLook(void) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        heightmapTerrainFilamentReleaseLook();
    }
}

void heightmapTerrainRenderSetDebugView(u32 mode) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        heightmapTerrainFilamentSetDebugView(mode);
    }
}

void heightmapTerrainRenderUpdate(void) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        heightmapTerrainFilamentUpdate();
    }
}

void heightmapTerrainRenderDestroy(void) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        heightmapTerrainFilamentDestroy();
    }
}
}  // namespace engine
