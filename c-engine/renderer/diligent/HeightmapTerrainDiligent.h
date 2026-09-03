#pragma once
#include "ecs/system/heightmap/HeightmapTerrainRender.h"

/*
 * Diligent half of the heightmap terrain pass.
 *
 * Contract: see HeightmapTerrainRender.h. All entry points run on the main
 * thread: the per-frame update comes from the backend's draw() (before the
 * world draw), the terrain draw after the glTF PBR draw (the same render
 * targets stay bound), the look calls from the game's world load/release,
 * destroy from the backend teardown (before the glTF pass, which owns the
 * preintegrated GGX LUT this pass borrows).
 */

namespace engine {
void heightmapTerrainDiligentInit(void);
void heightmapTerrainDiligentUpdate(void);
void heightmapTerrainDiligentDraw(void);
void heightmapTerrainDiligentRegisterLook(const HeightmapTerrainLook* look);
void heightmapTerrainDiligentReleaseLook(void);
void heightmapTerrainDiligentSetDebugView(u32 mode);
void heightmapTerrainDiligentStats(HeightmapTerrainRenderStats* out);
void heightmapTerrainDiligentDestroy(void);
}  // namespace engine
