#include "renderer/PropsRender.h"

#include "logger/Logger.h"
#include "renderer/Renderer.h"
#include "renderer/filament/PropsRenderFilament.h"

namespace engine {

void propsRenderSetMesh(const float* verts, u32 vertCount, const u32* idx, u32 idxCount) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        propsRenderFilamentSetMesh(verts, vertCount, idx, idxCount);
    }
    // The diligent mirror is deferred with phase 6 (no-op for now).
}

void propsRenderSetVariants(const PropsRenderMeshVariant* variants, u32 variantCount) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        propsRenderFilamentSetVariants(variants, variantCount);
    }
}

void propsRenderSetTile(i32 tileX, i32 tileZ, u64 readyStamp,
                        const PropsRenderInstance* instances, u32 instanceCount,
                        const PropsRenderRange* ranges, u32 rangeCount) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        propsRenderFilamentSetTile(tileX, tileZ, readyStamp, instances, instanceCount,
                ranges, rangeCount);
    }
}

void propsRenderClearAll(void) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        propsRenderFilamentClearAll();
    }
}

void propsRenderSetWind(float dirX, float dirZ, float speed, float strength) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        propsRenderFilamentSetWind(dirX, dirZ, speed, strength);
    }
}

void propsRenderSetEnabled(bool enabled) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        propsRenderFilamentSetEnabled(enabled);
    }
}

PropsRenderStats propsRenderStats(void) {
    PropsRenderStats stats = {};
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        propsRenderFilamentStats(&stats);
    }
    return stats;
}

void propsRenderUpdate(void) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        propsRenderFilamentUpdate();
    }
}

void propsRenderDestroy(void) {
    if (renderer::rendererBackend() == renderer::Backend::Filament) {
        propsRenderFilamentDestroy();
    }
}
}  // namespace engine
