#pragma once
#include "renderer/PropsRender.h"

/*
 * Diligent half of the Azgaar props (vegetation) render pass.
 *
 * Contract: see PropsRender.h. All entry points run on the main thread:
 * the Set* calls arrive from the game's world load / per-frame bridge
 * (Game.cpp propsBridgeUpdate), propsRenderUpdate runs from the backend's
 * frame (before the world draw) applying queued tiles to the GPU with a
 * per-frame budget (nearest to the camera first), propsRenderDiligentDraw
 * draws after the terrain pass (the same render targets stay bound), and
 * destroy comes from the backend teardown.
 *
 * Design notes (docs/lessons.md, the diligent entries):
 *   - per-instance data rides a per-tile RGBA32F texture fetched with
 *     SV_InstanceID in the vertex stage — per-instance vertex attributes
 *     silently fed nothing on the runtime-HLSL/Vulkan input-layout path;
 *   - per-(species,variant) draw constants (sway bounds, flags) are baked
 *     into the instance texels at pack time, so the only per-draw
 *     resources are the instance texture + the variant's base texture
 *     (no second cbuffer — the ambiguous-binding trap);
 *   - instance positions stay absolute f32; the shaders subtract the
 *     split anchor (f4ExtraData[3]/[4]) exactly like the terrain pass;
 *   - wind + phase time ride the props' own PBRFrameAttribs copy in
 *     f4ExtraData[0]/[1] (the terrain's look fields are not needed here).
 *
 * Debug: ENGINE_PROPS_DEBUG=1 logs a one-shot summary of the first draw's
 * tile/range state (tiles, per-range start/count/indexOffset/aabb) plus a
 * line when a frame culled everything or made its first draw.
 */

namespace engine {
// PropsRender.h API — stored CPU-side, consumed on the frames below.
void propsRenderDiligentSetMesh(const float* verts, u32 vertCount, const u32* idx, u32 idxCount);
void propsRenderDiligentSetVariants(const PropsRenderMeshVariant* variants, u32 variantCount);
void propsRenderDiligentSetTile(i32 tileX, i32 tileZ, u64 readyStamp,
        const PropsRenderInstance* instances, u32 instanceCount,
        const PropsRenderRange* ranges, u32 rangeCount);
void propsRenderDiligentClearAll(void);
void propsRenderDiligentSetWind(float dirX, float dirZ, float speed, float strength);
void propsRenderDiligentSetEnabled(bool enabled);

// Per-frame hooks (called by the backend's frame loop).
void propsRenderDiligentUpdate(void);
void propsRenderDiligentDraw(void);
PropsRenderStats propsRenderDiligentStats(void);
void propsRenderDiligentDestroy(void);
}  // namespace engine
