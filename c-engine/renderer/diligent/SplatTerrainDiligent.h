#pragma once

// Only diligent-path files include this.

#include "Defines.h"

#include <cstddef>
#include <string>
#include <vector>

namespace Diligent {
struct IBuffer;
struct IDeviceContext;
struct ITexture;
struct ITextureView;
}

namespace engine::renderer::diligent {

// UDIM splat resources for the Oghuzlands chunked terrain (the same packed
// GLB the untextured PBR scene slot draws, plans/blender-terrain.md phase 2).
//
// UV -> UDIM convention: the chunker remapped the original UDIM UVs
// (splatUvRange {min:[0,-9], max:[10,1]} on the first chunk node) linearly
// into [0,1]^2 as TEXCOORD_0, so a GLB uv (u,v) inverts to UDIM space as
// udimU = 10*u, udimV = 10*v - 9. Tile pick: col = floor(udimU),
// row = floor(udimV + 9), both clamped to 0..9 (chunk-boundary UVs overshoot
// by ~0.002 — clamping is mandatory, not cosmetic), local uv = the fractional
// parts. The file holding that tile is <group>/<group>.<1001 + row*10 + col>.ktx2
// (standard UDIM numbering: 1001 is the grid's bottom-left, row 0 = the
// smallest-v band, row 9*10+9 = 1100 is the top-right), and the tile's layer
// inside the weight TEXTURE2DARRAY is row*10 + col = file - 1001. The shader
// (splat pass, tasks 2/3) samples with exactly this; the loader only lays
// the resources out in this order.
//
// Weight arrays carry all 100 UDIM layers; the shipped sets cover only 11
// (grass1) + 6 (roads1) tiles. Missing layers are the Noop weight (0,0,0,255)
// — NOT black: the SplatGroup blend chain mixes the alpha detail in with
// (1 - wA), so an all-255 alpha tile leaves the base unchanged while black
// would smear the alpha detail across the whole map.

// Interleaved splat vertex (48 B): world-space position, normal, tangent
// (glTF TANGENT vec4: xyz + handedness) and the chunker-remapped [0,1]^2 uv.
struct SplatVertex {
    f32 pos[3];
    f32 nrm[3];
    f32 tan[4];
    f32 uv[2];
};
static_assert(sizeof(SplatVertex) == 48, "splat vertex layout");

// GPU resources for one chunk (chunks[i] = the i-th chunker node,
// terrain_chunk_<x>_<y>; vertices are NOT anchor-subtracted — the splat
// VS does that per frame against the cbuffer anchor).
struct SplatChunk {
    Diligent::IBuffer* vbo = nullptr;  // SplatVertex, 48 B stride
    Diligent::IBuffer* ibo = nullptr;  // u16
    size_t vertexCount = 0;
    size_t indexCount = 0;
    f32 aabbMin[3] = {0, 0, 0};
    f32 aabbMax[3] = {0, 0, 0};
};

// One detail texture set (images/terrain/<name>/{albedo,normal}.ktx2):
// albedo sRGB (decode on sample), normal linear. The baked 11-mip chains are
// uploaded verbatim (no GPU mipgen).
struct SplatDetail {
    std::string name;
    Diligent::ITexture* albedo = nullptr;
    Diligent::ITexture* normal = nullptr;
    Diligent::ITextureView* albedoView = nullptr;
    Diligent::ITextureView* normalView = nullptr;
};

// One splat group (grass1 / roads1): the UDIM weight array plus its
// red/green/blue/alpha detail sets. Groups are stored in the splatInfo JSON
// order (grass1, roads1); the Splat_1 blend chain runs last-to-first over a
// black base (roads1 is the base group, grass1 the top).
struct SplatGroup {
    std::string name;
    Diligent::ITexture* weights = nullptr;  // 1024^2 x 100 layers RGBA8_UNORM
    Diligent::ITextureView* weightsView = nullptr;
    SplatDetail details[4];  // [0]=red [1]=green [2]=blue [3]=alpha
};

struct SplatTerrain {
    std::vector<SplatChunk> chunks;
    std::vector<SplatGroup> groups;
    // splatUvRange the chunker recorded ({min:[0,-9], max:[10,1]}) — the
    // original UDIM-space uv extent, kept for the shader's inverse mapping.
    f32 uvMin[2] = {0, 0};
    f32 uvMax[2] = {0, 0};
};

// CPU-parse the packed chunker GLB (zstd GLB — readModelBytes/jansson walk,
// the gltfSceneSurfaceHeightDiligent pattern) and upload the GPU resources:
// per-chunk vertex/index buffers + AABBs, the per-group weight
// TEXTURE2DARRAYs (100 UDIM layers, missing tiles Noop) and the group detail
// sets (8 unique albedo/normal pairs, shared by reference). Every allocated
// resource is released again on failure.
bool splatTerrainLoadDiligent(const char* pakPath);
void splatTerrainDestroyDiligent(void);
// Non-null after a successful splatTerrainLoadDiligent.
const SplatTerrain* splatTerrainDiligent(void);

// The frame draw (task 3 — full PBR lighting + the TAA 3-RT outputs):
// compiles materials/splat_terrain_{vs,ps}.hlsl on first use, uploads the
// per-frame cbuffer (a flat HLSL::PBRFrameAttribs mirror + the f64 anchor —
// filled with the same getters as the PBR pass' fillFrameAttribs, matrices
// transposed for the runtime-HLSL convention), culls the chunks (frustum +
// 1-chunk camera window, FrustumCull.h) and draws them under the world pass'
// terrain debug group. The PS shades with the PBR lighting (Cook-Torrance
// GGX sun + CSM shadow receive + irradiance/prefiltered IBL + tangent-space
// normal maps) and writes all three world RTs. Behind ENGINE_SPLAT_TERRAIN
// (default on; "0" disables — the old PBR scene draw is the A/B fallback).
// Returns true when the splat pass drew (the caller skips the PBR scene
// render), false when it should not (gate off / not loaded / pass init
// failed / the TAA offscreen chain is unavailable).
bool splatTerrainDrawDiligent(Diligent::IDeviceContext* ctx);

}
