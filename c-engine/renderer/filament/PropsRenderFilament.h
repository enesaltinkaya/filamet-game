#pragma once
#include "renderer/PropsRender.h"

/*
 * Filament half of the props (vegetation) pass. See PropsRender.h for the
 * contract. All entry points run on the main thread: Set* from the game's
 * world load / per-frame bridge, update from the backend's draw(),
 * destroy from the backend teardown.
 *
 * GPU layout: one shared merged-mesh VBO/IBO per world; per tile one
 * RGBA32F instance-data texture (3 texels per instance, fetched in the
 * vertex stage via getInstanceIndex()); per (tile, range, chunk) one
 * MaterialInstance + one renderable (InstancedDraw, chunk <= 32767,
 * bounding box = the range's world AABB so Filament's own culling applies).
 */

namespace engine {
void propsRenderFilamentSetMesh(const float* verts, u32 vertCount, const u32* idx, u32 idxCount);
void propsRenderFilamentSetVariants(const PropsRenderMeshVariant* variants, u32 variantCount);
void propsRenderFilamentSetTile(i32 tileX, i32 tileZ, u64 readyStamp,
                                const PropsRenderInstance* instances, u32 instanceCount,
                                const PropsRenderRange* ranges, u32 rangeCount);
void propsRenderFilamentClearAll(void);
void propsRenderFilamentSetWind(float dirX, float dirZ, float speed, float strength);
void propsRenderFilamentSetEnabled(bool enabled);
void propsRenderFilamentUpdate(void);
void propsRenderFilamentStats(PropsRenderStats* out);
void propsRenderFilamentDestroy(void);
}  // namespace engine
