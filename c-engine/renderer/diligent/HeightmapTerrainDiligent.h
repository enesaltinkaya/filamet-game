#pragma once
#include "ecs/system/heightmap/HeightmapTerrainRender.h"
#include "renderer/diligent/FrustumCull.h"

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
// The CSM shadow pass' per-cascade depth draw (ShadowDiligent.cpp calls it
// once per cascade with the cascade DSV bound + the shared cbShadowPass
// filled). No-op while the pass is not initialized.
void heightmapTerrainDiligentShadowDraw(const renderer::diligent::FrustumCullPlanes* planes,
        const f32 camPos[3]);
// Debug: run the same cascade draw with an externally supplied PSO (the
// pass' own SRB/cbuffers stay). nullptr = the pass' own shadow PSO.
void heightmapTerrainDiligentShadowDrawPSO(void* psoOverride,
        const renderer::diligent::FrustumCullPlanes* planes, const f32 camPos[3]);
// Debug: the tile draws only (caller-bound PSO/SRB; lattice IBO bound here).
void heightmapTerrainDiligentShadowDrawTilesOnly(void);
void* heightmapTerrainDiligentShadowPSO(void);
void* heightmapTerrainDiligentShadowSrb(void);
void heightmapTerrainDiligentRegisterLook(const HeightmapTerrainLook* look);
void heightmapTerrainDiligentReleaseLook(void);
void heightmapTerrainDiligentSetDebugView(u32 mode);
void heightmapTerrainDiligentStats(HeightmapTerrainRenderStats* out);
void heightmapTerrainDiligentDestroy(void);
void* heightmapTerrainDiligentPRS(void);
void* heightmapTerrainDiligentGbufferPRS(void);
void* heightmapTerrainDiligentLitVS(void);
void heightmapTerrainDiligentGbufferDrawTiles(void* psoOverride);
}
