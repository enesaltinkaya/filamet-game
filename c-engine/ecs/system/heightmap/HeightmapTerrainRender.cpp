#include "ecs/system/heightmap/HeightmapTerrainRender.h"

#include "logger/Logger.h"
#include "renderer/Renderer.h"
#include "renderer/diligent/HeightmapTerrainDiligent.h"
#include "renderer/filament/HeightmapTerrainFilament.h"

namespace engine {

void heightmapTerrainRenderRegisterLook(const HeightmapTerrainLook* look) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        heightmapTerrainFilamentRegisterLook(look);
    } else if (renderer::rendererBackend() == renderer::Backend::Diligent) {
        heightmapTerrainDiligentRegisterLook(look);
    }
}

void heightmapTerrainRenderReleaseLook(void) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        heightmapTerrainFilamentReleaseLook();
    } else if (renderer::rendererBackend() == renderer::Backend::Diligent) {
        heightmapTerrainDiligentReleaseLook();
    }
}

void heightmapTerrainRenderSetDebugView(u32 mode) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        heightmapTerrainFilamentSetDebugView(mode);
    } else if (renderer::rendererBackend() == renderer::Backend::Diligent) {
        heightmapTerrainDiligentSetDebugView(mode);
    }
}

void heightmapTerrainRenderUpdate(void) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        heightmapTerrainFilamentUpdate();
    } else if (renderer::rendererBackend() == renderer::Backend::Diligent) {
        heightmapTerrainDiligentUpdate();
    }
}

HeightmapTerrainRenderStats heightmapTerrainRenderStats(void) {
    HeightmapTerrainRenderStats stats = {};
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        heightmapTerrainFilamentStats(&stats);
    } else if (renderer::rendererBackend() == renderer::Backend::Diligent) {
        heightmapTerrainDiligentStats(&stats);
    }
    return stats;
}

void heightmapTerrainRenderDestroy(void) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        heightmapTerrainFilamentDestroy();
    } else if (renderer::rendererBackend() == renderer::Backend::Diligent) {
        heightmapTerrainDiligentDestroy();
    }
}
}  // namespace engine
