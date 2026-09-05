#include "renderer/PropsRender.h"
#include "renderer/diligent/PropsRenderDiligent.h"

namespace engine {

/*
 * Backend-agnostic dispatch of the Azgaar props render pass (the
 * HeightmapTerrainRender pattern): every entry point forwards 1:1 to the
 * diligent backend's half (renderer/diligent/PropsRenderDiligent.cpp). The
 * filament implementation was removed with the filament backend (2026-09-05;
 * it lives on in git history as the look/parity reference). See
 * PropsRender.h for the domain contract.
 */

void propsRenderSetMesh(const float* verts, u32 vertCount, const u32* idx, u32 idxCount) {
    propsRenderDiligentSetMesh(verts, vertCount, idx, idxCount);
}

void propsRenderSetVariants(const PropsRenderMeshVariant* variants, u32 variantCount) {
    propsRenderDiligentSetVariants(variants, variantCount);
}

void propsRenderSetTile(i32 tileX, i32 tileZ, u64 readyStamp,
        const PropsRenderInstance* instances, u32 instanceCount,
        const PropsRenderRange* ranges, u32 rangeCount) {
    propsRenderDiligentSetTile(tileX, tileZ, readyStamp, instances, instanceCount,
            ranges, rangeCount);
}

void propsRenderClearAll(void) {
    propsRenderDiligentClearAll();
}

void propsRenderSetWind(float dirX, float dirZ, float speed, float strength) {
    propsRenderDiligentSetWind(dirX, dirZ, speed, strength);
}

void propsRenderSetEnabled(bool enabled) {
    propsRenderDiligentSetEnabled(enabled);
}

PropsRenderStats propsRenderStats(void) {
    return propsRenderDiligentStats();
}

void propsRenderUpdate(void) {
    propsRenderDiligentUpdate();
}

void propsRenderDestroy(void) {
    propsRenderDiligentDestroy();
}
}  // namespace engine
