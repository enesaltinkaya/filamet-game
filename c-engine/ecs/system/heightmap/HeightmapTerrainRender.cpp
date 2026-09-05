#include "ecs/system/heightmap/HeightmapTerrainRender.h"

#include "logger/Logger.h"
#include "renderer/diligent/HeightmapTerrainDiligent.h"

namespace engine {

void heightmapTerrainRenderRegisterLook(const HeightmapTerrainLook* look) {
    heightmapTerrainDiligentRegisterLook(look);
}

void heightmapTerrainRenderReleaseLook(void) {
    heightmapTerrainDiligentReleaseLook();
}

void heightmapTerrainRenderSetDebugView(u32 mode) {
    heightmapTerrainDiligentSetDebugView(mode);
}

void heightmapTerrainRenderUpdate(void) {
    heightmapTerrainDiligentUpdate();
}

HeightmapTerrainRenderStats heightmapTerrainRenderStats(void) {
    HeightmapTerrainRenderStats stats = {};
    heightmapTerrainDiligentStats(&stats);
    return stats;
}

void heightmapTerrainRenderDestroy(void) {
    heightmapTerrainDiligentDestroy();
}
}  // namespace engine
