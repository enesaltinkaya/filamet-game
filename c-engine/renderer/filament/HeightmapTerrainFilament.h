#pragma once
#include "ecs/system/heightmap/HeightmapTerrainRender.h"

/*
 * Filament half of the heightmap terrain pass.
 *
 * Contract: see HeightmapTerrainRender.h. All entry points run on the main
 * thread: the per-frame update from the backend's draw(), the look calls
 * from the game's world load/release, destroy from the backend teardown.
 */

namespace engine {
void heightmapTerrainFilamentInit(void);
void heightmapTerrainFilamentUpdate(void);
void heightmapTerrainFilamentRegisterLook(const HeightmapTerrainLook* look);
void heightmapTerrainFilamentReleaseLook(void);
void heightmapTerrainFilamentSetDebugView(u32 mode);
void heightmapTerrainFilamentDestroy(void);
}  // namespace engine
