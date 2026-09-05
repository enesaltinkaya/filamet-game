#pragma once

#include <vector>

#include "azgaar/AzgaarProps.h"

/*
 * AzgaarPropMesh
 * --------------
 * The merged species mesh of the props pass (phase 7, task: procedural
 * species meshes). All vegetation species are procedural placeholders,
 * ported verbatim from the old engine (game-001-cpp
 * c-game/game/azgaar/AzgaarProps.cpp, MeshBuilder + per-species builders):
 * unit height (base y=0, top y=1, so an instance's `scale` IS its target
 * height in metres), the 12 tree/shrub/rock builders plus one crossed
 * alpha-card per grass PNG variant (exact azgaarPropsGrassVariant order ==
 * kGrassTexPaths order).
 *
 * Output: ONE merged vertex array + ONE merged u32 index array + a range
 * per (species, variant). The render pass (PropsRender) uploads both
 * arrays once (shared VBO/IBO) and draws one InstancedDraw per (tile,
 * species, variant) range using the range's indexOffset/indexCount.
 *
 * Ownership: built once per world in azgaarPropsInit (grass variant
 * aspects/bottomV come from the loaded PNGs there), released by
 * azgaarPropsDestroy. Read-only afterwards (the render pass reads it from
 * the main thread; nothing mutates it).
 */
namespace game {

// Interleaved prop vertex, 13 floats / 52 bytes, laid out so the merged
// array uploads to the render pass's vertex buffer verbatim (no repack).
// Attribute mapping (stride 52):
//   POSITION  FLOAT3 @ 0
//   NORMAL    FLOAT4 @ 12  xyz normal (w = 0); rotated in the vertex stage
//                          with the yaw basis recovered from instanceTransform
//   UV        FLOAT2 @ 28  (unit UV; grass cards carry the trimmed card UVs)
//   COLOR     FLOAT4 @ 36  baked part colour (w = 1): brown trunks, white
//                          elsewhere = tintable by the per-instance tint
// (normal[4]/color[4] keep the old engine's layout so buffers stay
// drop-in compatible across render passes.)
// No per-instance attributes: species/variant are the draw-call identity.
struct AzgaarPropVertex {
    float position[3];
    float normal[4];  // xyz normal, w = 0
    float uv[2];
    float color[4];   // rgb part colour, w = 1
};

// One contiguous sub-range of the merged mesh: all indices in
// [indexOffset, indexOffset+indexCount) point into
// [vertexOffset, vertexOffset+vertexCount). Unit-space AABB (metres at
// scale 1) for per-instance bounding-sphere fallbacks and debug.
struct AzgaarPropMeshRange {
    u32 species;
    u32 variant;
    u32 vertexOffset;
    u32 vertexCount;
    u32 indexOffset;
    u32 indexCount;
    float aabbMin[3];
    float aabbMax[3];
};

struct AzgaarPropMesh {
    std::vector<AzgaarPropVertex> vertices;
    std::vector<u32> indices;
    std::vector<AzgaarPropMeshRange> ranges;  // species-major, variant-minor

    const AzgaarPropMeshRange* rangeFor(u32 species, u32 variant) const {
        for (const AzgaarPropMeshRange& r : ranges) {
            if (r.species == species && r.variant == variant) return &r;
        }
        return nullptr;
    }

    u32 variantCount(u32 species) const {
        u32 n = 0;
        for (const AzgaarPropMeshRange& r : ranges) {
            if (r.species == species) n++;
        }
        return n;
    }
};

// The merged mesh. Null before azgaarPropsInit / after azgaarPropsDestroy.
const AzgaarPropMesh* azgaarPropMeshGet(void);

// Build/release (called by azgaarPropsInit / azgaarPropsDestroy; not part of
// the render-pass API).
void azgaarPropMeshBuild(void);
void azgaarPropMeshRelease(void);

}  // namespace game
