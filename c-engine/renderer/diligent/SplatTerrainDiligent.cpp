#include "renderer/diligent/SplatTerrainDiligent.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "gltf/GltfInternal.h"
#include "logger/Logger.h"
#include "renderer/RenderBackend.h"
#include "renderer/diligent/DiligentRenderer.h"
#include "renderer/diligent/FrustumCull.h"
#include "renderer/diligent/IblDiligent.h"
#include "renderer/diligent/ShadowDiligent.h"
#include "renderer/diligent/TaaDiligent.h"

#include <Common/interface/RefCntAutoPtr.hpp>
#include <DiligentFXShaderSourceStreamFactory.hpp>
#include <Graphics/GraphicsEngine/interface/Buffer.h>
#include <Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <Graphics/GraphicsEngine/interface/PipelineResourceSignature.h>
#include <Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <Graphics/GraphicsEngine/interface/Sampler.h>
#include <Graphics/GraphicsEngine/interface/ShaderResourceBinding.h>
#include <Graphics/GraphicsEngine/interface/Texture.h>
#include <Graphics/GraphicsTools/interface/GraphicsUtilities.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Diligent;
using engine::gltf::gltfGlbFindChunksDiligent;
using engine::gltf::gltfReadModelBytesDiligent;

namespace Diligent {
namespace HLSL {
// C++ mirrors of the PBR renderer's shader structs (the same include block
// as GltfDiligent.cpp): the splat frame cbuffer is a flat mirror of
// PBRFrameAttribs, filled with the same getters fillFrameAttribs uses.
#define PBR_MAX_LIGHTS 1
#define ENABLE_SHADOWS 1
#define PBR_MAX_SHADOW_MAPS 1
#include <Shaders/Common/public/BasicStructures.fxh>
#include <Shaders/PBR/public/PBR_Structures.fxh>
#include <Shaders/PBR/private/RenderPBR_Structures.fxh>
}
}

namespace engine::renderer::diligent {

namespace {

constexpr size_t WEIGHT_LAYERS = 100;  // the full UDIM grid (files 1001..1100)
constexpr u32 WEIGHT_SIZE = 1024;

// glTF componentType values.
constexpr int CT_BYTE = 5120;  // i8
constexpr int CT_UBYTE = 5121;  // u8
constexpr int CT_SHORT = 5122;  // i16
constexpr int CT_U16 = 5123;  // u16
constexpr int CT_I32 = 5125;  // i32
constexpr int CT_F32 = 5126;  // f32

SplatTerrain* terrain = nullptr;

// Everything allocated during a load; released on a mid-load failure before
// anything is committed to the global SplatTerrain.
struct LoadGuard {
    std::vector<IBuffer*> buffers;
    std::vector<ITexture*> textures;

    IBuffer* trackBuffer(IBuffer* p) {
        if (p) buffers.push_back(p);
        return p;
    }
    ITexture* trackTexture(ITexture* p) {
        if (p) textures.push_back(p);
        return p;
    }
    void releaseAll(void) {
        // Views are never released here: this Diligent build's GetDefaultView
        // does not AddRef, so the default view is owned by the texture and
        // dies with it.
        for (IBuffer* p : buffers) p->Release();
        for (ITexture* p : textures) p->Release();
        buffers.clear();
        textures.clear();
    }
};

struct RawChunk {
    std::vector<SplatVertex> verts;
    std::vector<u16> indices;
    f32 aabbMin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    f32 aabbMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
};

// A decoded weight tile, kept alive until its group's CreateTexture has
// copied the data driver-side (TextureData pContext).
struct WeightTile {
    utils::Image image;
    bool valid = false;
};

// jansson accessor -> raw pointer into the BIN chunk.
struct AccessorData {
    const u8* data = nullptr;
    size_t count = 0;
    size_t stride = 0;
    int componentType = 0;
    bool normalized = false;
};

int compBytes(int ct) {
    switch (ct) {
        case CT_BYTE:
        case CT_UBYTE:
            return 1;
        case CT_SHORT:
        case CT_U16:
            return 2;
        case CT_I32:
        case CT_F32:
            return 4;
        default:
            return 0;
    }
}

int typeComponents(const char* type) {
    if (!type) {
        return 0;
    }
    if (std::strcmp(type, "SCALAR") == 0) {
        return 1;
    }
    if (std::strcmp(type, "VEC2") == 0) {
        return 2;
    }
    if (std::strcmp(type, "VEC3") == 0) {
        return 3;
    }
    if (std::strcmp(type, "VEC4") == 0) {
        return 4;
    }
    return 0;
}

bool accessorData(json_t* accessors, json_t* bufferViews, const u8* bin, size_t binSize,
        int id, AccessorData& out) {
    out = {};
    if (!json_is_array(accessors) || id < 0 || (size_t)id >= json_array_size(accessors)) {
        return false;
    }
    json_t* acc = json_array_get(accessors, id);
    if (!json_is_object(acc)) {
        return false;
    }
    json_t* bvId = json_object_get(acc, "bufferView");
    if (!json_is_integer(bvId) || (size_t)json_integer_value(bvId) >= json_array_size(bufferViews)) {
        return false;
    }
    json_t* bv = json_array_get(bufferViews, (size_t)json_integer_value(bvId));
    size_t bvOff = 0, stride = 0;
    json_t* bo = json_object_get(bv, "byteOffset");
    if (json_is_integer(bo)) {
        bvOff = (size_t)json_integer_value(bo);
    }
    // glTF: byteStride is a bufferView field (absent = tightly packed).
    json_t* bs = json_object_get(bv, "byteStride");
    if (json_is_integer(bs) && json_integer_value(bs) > 0) {
        stride = (size_t)json_integer_value(bs);
    }
    size_t accOff = 0;
    json_t* ao = json_object_get(acc, "byteOffset");
    if (json_is_integer(ao)) {
        accOff = (size_t)json_integer_value(ao);
    }
    json_t* cnt = json_object_get(acc, "count");
    if (!json_is_integer(cnt) || json_integer_value(cnt) <= 0) {
        return false;
    }
    json_t* ct = json_object_get(acc, "componentType");
    if (!json_is_integer(ct)) {
        return false;
    }
    out.count = (size_t)json_integer_value(cnt);
    out.componentType = json_integer_value(ct);
    out.normalized = json_is_true(json_object_get(acc, "normalized"));
    const int cb = compBytes(out.componentType);
    const int cc = typeComponents(jsonGetString(acc, "type"));
    if (cb <= 0 || cc <= 0) {
        return false;
    }
    if (stride == 0) {
        stride = (size_t)cb * cc;
    }
    out.data = bin + bvOff + accOff;
    out.stride = stride;
    if (out.data + out.count * stride > bin + binSize) {
        return false;
    }
    return true;
}

f32 rdF32(const u8* p) {
    f32 v;
    std::memcpy(&v, p, 4);
    return v;
}
i16 rdI16(const u8* p) {
    i16 v;
    std::memcpy(&v, p, 2);
    return v;
}

// Tight byte size of an rgba8 mip chain of the given base size.
size_t rgba8MipBytes(u32 base, int mips) {
    size_t total = 0;
    for (int m = 0; m < mips; m++) {
        const u32 w = std::max(1u, base >> m);
        const u32 h = std::max(1u, base >> m);
        total += u64(w) * h * 4;
    }
    return total;
}

// Uploads the group's 100-layer weight array from the decoded tiles (missing
// layers filled with the Noop weight). Subresources are mip-major
// (level * ArraySize + slice), rows tight (the c-utils ktx2 decode packs
// rgba8 levels at w*4 pitch, same assumption as diligentCreateImageTexture).
bool createWeightArray(const std::string& name, const WeightTile* tiles, int mips,
        ITexture** outTex, ITextureView** outView, LoadGuard* guard) {
    *outTex = nullptr;
    *outView = nullptr;
    if (mips <= 0 || mips > 16) {
        return false;
    }
    size_t levelOff[17] = {0};
    size_t layerBytesPer[16] = {0};
    size_t total = 0;
    for (int m = 0; m < mips; m++) {
        const u32 w = std::max(1u, WEIGHT_SIZE >> m);
        const u32 h = std::max(1u, WEIGHT_SIZE >> m);
        layerBytesPer[m] = u64(w) * h * 4;
        levelOff[m] = total;
        total += layerBytesPer[m] * WEIGHT_LAYERS;
    }
    std::vector<u8> staging(total);
    for (int m = 0; m < mips; m++) {
        const size_t layerBytes = layerBytesPer[m];
        for (size_t L = 0; L < WEIGHT_LAYERS; L++) {
            u8* dst = staging.data() + levelOff[m] + L * layerBytes;
            const WeightTile& t = tiles[L];
            if (t.valid && t.image.mipSizes.size() > (size_t)m) {
                std::memcpy(dst, (const u8*)t.image.data + t.image.mipSizes[m], layerBytes);
            } else {
                // Noop weight (0,0,0,255): the alpha detail mixes in with
                // (1 - wA), so an all-255 tile leaves the base untouched.
                std::memset(dst, 0, layerBytes);
                for (size_t i = 0; i < layerBytes / 4; i++) {
                    dst[i * 4 + 3] = 255;
                }
            }
        }
    }

    TextureDesc desc;
    desc.Name      = name.c_str();
    desc.Type      = RESOURCE_DIM_TEX_2D_ARRAY;
    desc.Usage     = USAGE_IMMUTABLE;
    desc.BindFlags = BIND_SHADER_RESOURCE;
    desc.Format    = TEX_FORMAT_RGBA8_UNORM;
    desc.Width     = WEIGHT_SIZE;
    desc.Height    = WEIGHT_SIZE;
    desc.MipLevels = (Uint32)mips;
    desc.ArraySize = (Uint32)WEIGHT_LAYERS;

    // The Vulkan backend's InitializeContentOnDevice consumes
    // TextureSubResData in layer-major order (layer * MipLevels + mip) —
    // unlike the D3D-style mip-major GetSubresIndex convention. Point each
    // entry at its (layer, mip) slice with that mip's row pitch.
    std::vector<TextureSubResData> subres(u64(WEIGHT_LAYERS) * mips);
    for (size_t L = 0; L < WEIGHT_LAYERS; L++) {
        for (int m = 0; m < mips; m++) {
            const size_t layerBytes = layerBytesPer[m];
            const u64 rowPitch = std::max(1u, WEIGHT_SIZE >> m) * 4;
            subres[L * mips + m] =
                    TextureSubResData(staging.data() + levelOff[m] + L * layerBytes, rowPitch);
        }
    }
    TextureData data;
    data.pSubResources   = subres.data();
    data.NumSubresources = (Uint32)subres.size();
    data.pContext        = context;

    RefCntAutoPtr<ITexture> tex;
    device->CreateTexture(desc, &data, &tex);
    if (!tex) {
        utils::warn("splatTerrain: weight array '%s' creation failed", name.c_str());
        return false;
    }
    StateTransitionDesc barrier{tex,
                                RESOURCE_STATE_UNKNOWN,
                                RESOURCE_STATE_SHADER_RESOURCE,
                                STATE_TRANSITION_FLAG_UPDATE_STATE};
    context->TransitionResourceStates(1, &barrier);
    tex->AddRef();
    *outTex = guard->trackTexture(tex);
    // The default view is owned by the texture (GetDefaultView does not
    // AddRef); it is released implicitly when the texture is destroyed.
    *outView = tex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    utils::info("splatTerrain: weight array '%s' 1024^2 x %zu layers x %d mips (%.0f KB)",
            name.c_str(), WEIGHT_LAYERS, mips, (double)total / 1024.0);
    return true;
}

// The packed GLB's splat base dir: "models/terrain/oghuzlands.zstd" ->
// "models/terrain/oghuzlands" (the chunker's tile sets live in
// <base>/<group>/<group>.<udim>.ktx2, like the old engine's splatBaseDir).
std::string splatBaseDir(const char* pakPath) {
    std::string p(pakPath);
    size_t slash = p.rfind('/');
    size_t dot = p.rfind('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        p = p.substr(0, dot);
    }
    return p;
}

}  // namespace

bool splatTerrainLoadDiligent(const char* pakPath) {
    if (terrain) {
        utils::warn("splatTerrain: already loaded — destroying first");
        splatTerrainDestroyDiligent();
    }
    if (!device || !context) {
        utils::warn("splatTerrain: renderer device not ready");
        return false;
    }

    // ── CPU parse: readModelBytes (zstd) → glbFindChunks → jansson ──
    std::vector<unsigned char> bytes;
    std::string error;
    if (!gltfReadModelBytesDiligent(pakPath, bytes, error)) {
        utils::warn("splatTerrain: cannot read %s (%s)", pakPath, error.c_str());
        return false;
    }
    const unsigned char* jsonPtr = nullptr;
    size_t jsonSize = 0, binSize = 0;
    const unsigned char* binPtr = nullptr;
    if (!gltfGlbFindChunksDiligent(bytes, &jsonPtr, jsonSize, &binPtr, binSize)) {
        utils::warn("splatTerrain: %s is not a GLB", pakPath);
        return false;
    }
    json_t* root = jsonParseN(reinterpret_cast<const char*>(jsonPtr), (u32)jsonSize);
    if (!root) {
        utils::warn("splatTerrain: bad GLB JSON in %s", pakPath);
        return false;
    }
    json_t* accessors   = jsonGetArray(root, "accessors");
    json_t* bufferViews = jsonGetArray(root, "bufferViews");
    json_t* meshes      = jsonGetArray(root, "meshes");
    json_t* nodes       = jsonGetArray(root, "nodes");
    if (!json_is_array(accessors) || !json_is_array(bufferViews) ||
        !json_is_array(meshes) || !json_is_array(nodes)) {
        jsonFree(root);
        utils::warn("splatTerrain: GLB JSON lacks accessors/bufferViews/meshes/nodes");
        return false;
    }
    const u8* bin = binPtr;

    // splatInfo (group → 4 detail names) + splatUvRange: the chunker puts
    // both on the first chunk node's extras.
    std::vector<std::string> groupNames;
    std::vector<std::array<std::string, 4>> groupDetails;
    f32 uvMin[2] = {0, -9}, uvMax[2] = {10, 1};
    for (size_t ni = 0; ni < json_array_size(nodes); ni++) {
        json_t* node = json_array_get(nodes, ni);
        if (!json_is_object(node)) {
            continue;
        }
        json_t* extras  = jsonGetObject(node, "extras");
        json_t* splat   = json_is_object(extras) ? jsonGetObject(extras, "splatInfo") : nullptr;
        json_t* uvRange = json_is_object(extras) ? jsonGetObject(extras, "splatUvRange") : nullptr;
        if (splat && json_is_object(splat)) {
            const char* key;
            json_t* value;
            json_object_foreach(splat, key, value) {
                if (!json_is_object(value)) {
                    continue;
                }
                std::array<std::string, 4> details;
                const char* channels[4] = {"red", "green", "blue", "alpha"};
                bool have = true;
                for (int ch = 0; ch < 4; ch++) {
                    details[ch] = jsonGetString(value, channels[ch]);
                    if (details[ch].empty()) {
                        have = false;
                        break;
                    }
                }
                if (have) {
                    groupNames.push_back(key);
                    groupDetails.push_back(details);
                }
            }
        }
        if (uvRange && json_is_object(uvRange)) {
            json_t* mn = jsonGetObject(uvRange, "min");
            json_t* mx = jsonGetObject(uvRange, "max");
            if (json_is_array(mn) && json_array_size(mn) == 2 && json_is_array(mx) &&
                json_array_size(mx) == 2) {
                uvMin[0] = (f32)json_number_value(json_array_get(mn, 0));
                uvMin[1] = (f32)json_number_value(json_array_get(mn, 1));
                uvMax[0] = (f32)json_number_value(json_array_get(mx, 0));
                uvMax[1] = (f32)json_number_value(json_array_get(mx, 1));
            }
        }
    }
    if (groupNames.empty()) {
        jsonFree(root);
        utils::warn("splatTerrain: no splatInfo in %s", pakPath);
        return false;
    }

    // Per-chunk CPU geometry: POSITION/NORMAL/TANGENT/TEXCOORD_0 + indices.
    std::vector<RawChunk> raw;
    size_t totalVerts = 0, totalTris = 0;
    for (size_t ni = 0; ni < json_array_size(nodes); ni++) {
        json_t* node = json_array_get(nodes, ni);
        if (!json_is_object(node) || !json_is_integer(json_object_get(node, "mesh"))) {
            continue;
        }
        const size_t mi = (size_t)json_integer_value(json_object_get(node, "mesh"));
        if (mi >= json_array_size(meshes)) {
            continue;
        }
        json_t* mesh  = json_array_get(meshes, mi);
        json_t* prims = json_is_object(mesh) ? jsonGetObject(mesh, "primitives") : nullptr;
        if (!json_is_array(prims) || json_array_size(prims) == 0) {
            continue;
        }
        json_t* prim = json_array_get(prims, 0);
        json_t* attrs = json_is_object(prim) ? jsonGetObject(prim, "attributes") : nullptr;
        json_t* idxId = json_is_object(prim) ? json_object_get(prim, "indices") : nullptr;
        if (!json_is_object(attrs) || !json_is_integer(idxId)) {
            continue;
        }

        AccessorData pos, nrm, tan, uv, idx;
        if (!accessorData(accessors, bufferViews, bin, binSize,
                    (int)json_integer_value(json_object_get(attrs, "POSITION")), pos) ||
            !accessorData(accessors, bufferViews, bin, binSize,
                    (int)json_integer_value(json_object_get(attrs, "NORMAL")), nrm) ||
            !accessorData(accessors, bufferViews, bin, binSize,
                    (int)json_integer_value(json_object_get(attrs, "TANGENT")), tan) ||
            !accessorData(accessors, bufferViews, bin, binSize,
                    (int)json_integer_value(json_object_get(attrs, "TEXCOORD_0")), uv) ||
            !accessorData(accessors, bufferViews, bin, binSize, (int)json_integer_value(idxId), idx)) {
            utils::warn("splatTerrain: node %zu — missing attribute or index accessor", ni);
            continue;
        }
        if (nrm.componentType != CT_SHORT || tan.componentType != CT_BYTE) {
            utils::warn("splatTerrain: node %zu — unexpected NORMAL/TANGENT componentTypes %d/%d", ni,
                    nrm.componentType, tan.componentType);
            continue;
        }
        if (pos.count > 65535) {
            utils::warn("splatTerrain: node %zu — %zu verts exceed u16 indexing", ni, pos.count);
            continue;
        }
        if (pos.componentType != CT_F32 || uv.componentType != CT_F32) {
            utils::warn("splatTerrain: node %zu — unexpected POSITION/TEXCOORD_0 componentType", ni);
            continue;
        }

        RawChunk rc;
        rc.verts.resize(pos.count);
        rc.indices.resize(idx.count);
        for (size_t i = 0; i < pos.count; i++) {
            SplatVertex& v = rc.verts[i];
            const u8* p  = pos.data + i * pos.stride;
            const u8* n  = nrm.data + i * nrm.stride;
            const u8* t  = tan.data + i * tan.stride;
            const u8* uu = uv.data + i * uv.stride;
            for (int c = 0; c < 3; c++) {
                v.pos[c] = rdF32(p + c * 4);
                v.nrm[c] = (f32)rdI16(n + c * 2) / 32767.0f;
            }
            for (int c = 0; c < 4; c++) {
                v.tan[c] = (f32)((const i8*)t)[c] / 127.0f;
            }
            v.uv[0] = rdF32(uu);
            v.uv[1] = rdF32(uu + 4);
            for (int c = 0; c < 3; c++) {
                rc.aabbMin[c] = std::min(rc.aabbMin[c], v.pos[c]);
                rc.aabbMax[c] = std::max(rc.aabbMax[c], v.pos[c]);
            }
        }
        bool badIndex = false;
        for (size_t i = 0; i < idx.count; i++) {
            const u8* s = idx.data + i * idx.stride;
            if (idx.componentType == CT_U16) {
                rc.indices[i] = (u16)rdI16(s);
            } else if (idx.componentType == CT_F32) {
                const f32 f = rdF32(s);
                if (f < 0.5f || f > 65535.5f || std::truncf(f) != f) {
                    utils::warn("splatTerrain: node %zu — f32 index %g out of u16 range", ni,
                            (double)f);
                    badIndex = true;
                    break;
                }
                rc.indices[i] = (u16)f;
            } else if (idx.componentType == CT_I32) {
                i32 s32;
                std::memcpy(&s32, s, 4);
                if (s32 < 0 || s32 > 65535) {
                    utils::warn("splatTerrain: node %zu — i32 index %d out of u16 range", ni, s32);
                    badIndex = true;
                    break;
                }
                rc.indices[i] = (u16)s32;
            } else {
                utils::warn("splatTerrain: node %zu — unsupported index componentType %d", ni,
                        idx.componentType);
                badIndex = true;
                break;
            }
        }
        if (badIndex) {
            continue;
        }
        raw.push_back(std::move(rc));
        totalVerts += raw.back().verts.size();
        totalTris += raw.back().indices.size() / 3;
    }
    if (raw.empty()) {
        jsonFree(root);
        utils::warn("splatTerrain: no usable chunk meshes in %s", pakPath);
        return false;
    }
    jsonFree(root);

    // ── GPU upload ──
    LoadGuard guard;
    auto fail = [&]() {
        guard.releaseAll();
        return false;
    };

    std::vector<SplatChunk> chunks;
    for (size_t ci = 0; ci < raw.size(); ci++) {
        const RawChunk& rc = raw[ci];
        SplatChunk chunk;
        BufferDesc vdesc;
        vdesc.Usage          = USAGE_IMMUTABLE;
        vdesc.BindFlags      = BIND_VERTEX_BUFFER;
        vdesc.Size           = rc.verts.size() * sizeof(SplatVertex);
        vdesc.Name           = "splat terrain chunk vbo";
        BufferData vdata;
        vdata.pData    = rc.verts.data();
        vdata.DataSize = vdesc.Size;
        vdata.pContext = context;
        RefCntAutoPtr<IBuffer> vboRef;
        device->CreateBuffer(vdesc, &vdata, &vboRef);
        if (!vboRef) {
            utils::warn("splatTerrain: chunk %zu vertex buffer failed", ci);
            return fail();
        }
        vboRef->AddRef();
        BufferDesc idesc;
        idesc.Usage       = USAGE_IMMUTABLE;
        idesc.BindFlags    = BIND_INDEX_BUFFER;
        idesc.Size         = rc.indices.size() * sizeof(u16);
        idesc.Name         = "splat terrain chunk ibo";
        BufferData idata;
        idata.pData    = rc.indices.data();
        idata.DataSize = idesc.Size;
        idata.pContext = context;
        RefCntAutoPtr<IBuffer> iboRef;
        device->CreateBuffer(idesc, &idata, &iboRef);
        if (!iboRef) {
            utils::warn("splatTerrain: chunk %zu index buffer failed", ci);
            return fail();
        }
        iboRef->AddRef();
        chunk.vbo         = guard.trackBuffer(vboRef);
        chunk.ibo         = guard.trackBuffer(iboRef);
        chunk.vertexCount = rc.verts.size();
        chunk.indexCount  = rc.indices.size();
        std::memcpy(chunk.aabbMin, rc.aabbMin, sizeof(rc.aabbMin));
        std::memcpy(chunk.aabbMax, rc.aabbMax, sizeof(rc.aabbMax));
        chunks.push_back(chunk);
        utils::info("splatTerrain: chunk %zu %zu verts %zu tris  AABB [%.0f %.0f %.0f]-[%.0f %.0f %.0f]",
                ci, chunk.vertexCount, chunk.indexCount / 3, chunk.aabbMin[0], chunk.aabbMin[1],
                chunk.aabbMin[2], chunk.aabbMax[0], chunk.aabbMax[1], chunk.aabbMax[2]);
    }

    // Detail sets: images/terrain/<name>/{albedo,normal}.ktx2, deduped by name.
    struct DetailTex {
        ITexture* albedo = nullptr;
        ITexture* normal = nullptr;
        ITextureView* albedoView = nullptr;
        ITextureView* normalView = nullptr;
    };
    std::unordered_map<std::string, DetailTex> detailCache;
    std::vector<SplatGroup> groups;
    for (size_t g = 0; g < groupNames.size(); g++) {
        SplatGroup group;
        group.name = groupNames[g];

        // Weight tiles: scan the pak for <base>/<group>/<group>.<udim>.ktx2
        // (standard UDIM numbering: layer = file - 1001).
        std::vector<WeightTile> tiles(WEIGHT_LAYERS);
        {
            std::string tileDir = splatBaseDir(pakPath) + "/" + group.name + "/";
            std::string prefix  = tileDir + group.name + ".";
            std::vector<utils::String> files = utils::dataManagerListFiles(".ktx2");
            for (utils::String& f : files) {
                if (f.size > prefix.size() + 5 &&
                    std::strncmp(f.data, prefix.data(), prefix.size()) == 0) {
                    const char* after = f.data + prefix.size();
                    char* end = nullptr;
                    const long udim = std::strtol(after, &end, 10);
                    if (end && *end == '.' && udim >= 1001 && udim <= 1100) {
                        const size_t layer = (size_t)udim - 1001;
                        utils::Image image = utils::imageLoad(f.data);
                        if (!image.isKtx || !image.data || image.width <= 0 || image.height <= 0 ||
                            image.mips <= 0) {
                            utils::warn("splatTerrain: bad weight tile %s", f.data);
                            if (image.data) {
                                utils::imageDestory(&image);
                            }
                            continue;
                        }
                        const size_t tight = rgba8MipBytes((u32)image.width, image.mips);
                        if (image.width != (int)WEIGHT_SIZE || image.height != (int)WEIGHT_SIZE ||
                            image.size != tight) {
                            utils::warn("splatTerrain: weight tile %s unexpected payload (%dx%d %d mips %llu B, expected %dx%d tight %zu B) — using Noop",
                                    f.data, image.width, image.height, image.mips,
                                    (unsigned long long)image.size, WEIGHT_SIZE, WEIGHT_SIZE, tight);
                            utils::imageDestory(&image);
                            continue;
                        }
                        if (tiles[layer].valid) {
                            utils::warn("splatTerrain: duplicate weight tile for layer %zu — using Noop", layer);
                            utils::imageDestory(&image);
                            continue;
                        }
                        tiles[layer].image = std::move(image);
                        tiles[layer].valid = true;
                    }
                }
                utils::stringDestroy(&f);
            }
        }
        int mips = 0;
        size_t tilesLoaded = 0;
        for (size_t L = 0; L < WEIGHT_LAYERS; L++) {
            if (tiles[L].valid) {
                if (mips == 0) {
                    mips = tiles[L].image.mips;
                }
                if (tiles[L].image.mips != mips) {
                    utils::warn("splatTerrain: %s layer %zu has %d mips (first tile %d) — using Noop",
                            group.name.c_str(), L, tiles[L].image.mips, mips);
                    utils::imageDestory(&tiles[L].image);
                    tiles[L].valid = false;
                } else {
                    tilesLoaded++;
                }
            }
        }
        if (mips == 0 || !createWeightArray("splat weights " + group.name, tiles.data(), mips,
                    &group.weights, &group.weightsView, &guard)) {
            for (auto& t : tiles) {
                if (t.valid) {
                    utils::imageDestory(&t.image);
                }
            }
            utils::warn("splatTerrain: no usable weight tiles for group '%s'", group.name.c_str());
            return fail();
        }
        for (auto& t : tiles) {
            if (t.valid) {
                utils::imageDestory(&t.image);
            }
        }
        utils::info("splatTerrain: group '%s' — %zu/%zu weight tiles, %d mips", group.name.c_str(),
                tilesLoaded, WEIGHT_LAYERS, mips);

        for (int ch = 0; ch < 4; ch++) {
            SplatDetail& detail = group.details[ch];
            detail.name = groupDetails[g][ch];
            auto cacheIt = detailCache.find(detail.name);
            if (cacheIt == detailCache.end()) {
                DetailTex dt;
                const std::string albedoPath = "images/terrain/" + detail.name + "/albedo.ktx2";
                const std::string normalPath = "images/terrain/" + detail.name + "/normal.ktx2";
                utils::Image albedo = utils::imageLoad(albedoPath.c_str());
                if (!albedo.isKtx || !albedo.data) {
                    utils::warn("splatTerrain: detail albedo load failed: %s", albedoPath.c_str());
                    if (albedo.data) {
                        utils::imageDestory(&albedo);
                    }
                    return fail();
                }
                dt.albedo = diligentCreateImageTexture(albedo, albedoPath.c_str(), true);
                utils::imageDestory(&albedo);
                if (!dt.albedo) {
                    utils::warn("splatTerrain: detail albedo upload failed: %s", albedoPath.c_str());
                    return fail();
                }
                utils::Image normal = utils::imageLoad(normalPath.c_str());
                if (!normal.isKtx || !normal.data) {
                    utils::warn("splatTerrain: detail normal load failed: %s", normalPath.c_str());
                    utils::imageDestory(&normal);
                    dt.albedo->Release();
                    return fail();
                }
                dt.normal = diligentCreateImageTexture(normal, normalPath.c_str(), false);
                utils::imageDestory(&normal);
                if (!dt.normal) {
                    utils::warn("splatTerrain: detail normal upload failed: %s", normalPath.c_str());
                    dt.albedo->Release();
                    return fail();
                }
                dt.albedo   = guard.trackTexture(dt.albedo);
                dt.normal   = guard.trackTexture(dt.normal);
                dt.albedoView = dt.albedo->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
                dt.normalView = dt.normal->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
                cacheIt = detailCache.emplace(detail.name, dt).first;
            }
            detail.albedo     = cacheIt->second.albedo;
            detail.normal     = cacheIt->second.normal;
            detail.albedoView = cacheIt->second.albedoView;
            detail.normalView = cacheIt->second.normalView;
        }
        groups.push_back(std::move(group));
    }

    const char* bandDirs[3] = {"snow_default", "sand_default", "cliff_side_default"};
    std::array<SplatDetail, 3> bands = {};
    for (int b = 0; b < 3; b++) {
        SplatDetail& band = bands[b];
        band.name = bandDirs[b];
        const std::string albedoPath = std::string("images/terrain/") + bandDirs[b] + "/albedo.ktx2";
        utils::Image albedo = utils::imageLoad(albedoPath.c_str());
        if (!albedo.isKtx || !albedo.data) {
            utils::warn("splatTerrain: band albedo load failed: %s", albedoPath.c_str());
            if (albedo.data) {
                utils::imageDestory(&albedo);
            }
            return fail();
        }
        band.albedo = diligentCreateImageTexture(albedo, albedoPath.c_str(), true);
        utils::imageDestory(&albedo);
        if (!band.albedo) {
            utils::warn("splatTerrain: band albedo upload failed: %s", albedoPath.c_str());
            return fail();
        }
        const std::string normalPath = std::string("images/terrain/") + bandDirs[b] + "/normal.ktx2";
        utils::Image normal = utils::imageLoad(normalPath.c_str());
        if (!normal.isKtx || !normal.data) {
            utils::warn("splatTerrain: band normal load failed: %s", normalPath.c_str());
            utils::imageDestory(&normal);
            band.albedo->Release();
            return fail();
        }
        band.normal = diligentCreateImageTexture(normal, normalPath.c_str(), false);
        utils::imageDestory(&normal);
        if (!band.normal) {
            utils::warn("splatTerrain: band normal upload failed: %s", normalPath.c_str());
            band.albedo->Release();
            return fail();
        }
        band.albedo = guard.trackTexture(band.albedo);
        band.normal = guard.trackTexture(band.normal);
        band.albedoView = band.albedo->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        band.normalView = band.normal->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        const TextureDesc& ad = band.albedo->GetDesc();
        const TextureDesc& nd = band.normal->GetDesc();
        utils::info("splatTerrain: band '%s' albedo %ux%u %d mips / normal %ux%u %d mips",
                bandDirs[b], ad.Width, ad.Height, ad.MipLevels, nd.Width, nd.Height, nd.MipLevels);
    }

    // Commit.
    auto* t = new SplatTerrain();
    t->chunks = std::move(chunks);
    t->groups = std::move(groups);
    for (int b = 0; b < 3; b++) {
        t->band[b] = bands[b];
    }
    std::memcpy(t->uvMin, uvMin, sizeof(uvMin));
    std::memcpy(t->uvMax, uvMax, sizeof(uvMax));
    guard.buffers.clear();
    guard.textures.clear();
    terrain = t;
    utils::info("splatTerrain: loaded %s — %zu chunks, %zu verts, %zu tris, %zu groups, "
            "%zu detail sets (uv range [%.0f,%.0f]..[%.0f,%.0f])",
            pakPath, t->chunks.size(), totalVerts, totalTris, t->groups.size(),
            detailCache.size(), (double)uvMin[0], (double)uvMin[1], (double)uvMax[0],
            (double)uvMax[1]);
    return true;
}

// ── Draw pass (task 3 — full PBR lighting + TAA 3-RT) ───────────────────
// Per-frame: fill the frame cbuffer (HLSL::PBRFrameAttribs + anchor — the
// same getters GltfDiligent.cpp fillFrameAttribs uses: the directional sun
// light, the CSM cascade receive state, the IBL params; matrices TRANSPOSED
// for the runtime-glslang convention) → frustum + 1-chunk-window cull → one
// SRB commit → the visible chunks. The cbuffer is a USAGE_DYNAMIC ring (the
// rmlui/gltf frame-attribs pattern), so it is re-Uploaded through
// MapBuffer/UnmapBuffer and the SRB re-committed every frame — never
// UpdateBuffer on a dynamic buffer (docs/lessons.md). The IBL cubes' and
// the shadow atlas' SRVs are re-set on the dynamic SRB slots only when the
// bound resource changes (IBL env cycling, shadow mode changes).

void splatPassRelease(void);

// The staging for the frame cbuffer upload: a byte mirror of the
// cbSplatFrame cbuffer declared in splat_terrain_{vs,ps}.hlsl (HLSL::
// PBRFrameAttribs flat + the splat anchor). Field order must stay identical.
struct SplatFrameStaging {
    Diligent::HLSL::PBRFrameAttribs frame;
    Diligent::float4 anchor;  // xyz: f64 camera eye rounded to f32, w 0
};
static_assert(sizeof(SplatFrameStaging::frame) % 16 == 0, "splat frame attribs layout");
static SplatFrameStaging splatFrameStaging;

// The PBR pass' CSM-receive condition (pbrShadowsOn in GltfDiligent.cpp):
// the PBR receiver only receives in PCF mode — the filterable VSM/EVSM
// atlas stores variance, not depth.
static bool splatShadowsOn(void) {
    if (getenv("ENGINE_PBR_NO_RECEIVE")) {
        return false;
    }
    return shadowDiligentActive() && shadowDiligentMode() == 1 &&
           shadowDiligentShadowSRV() != nullptr;
}

// The PBRFrameAttribs matrices are filled in Diligent's row-major math
// (like fillFrameAttribs) and then TRANSPOSED for the runtime-glslang
// cbuffer convention (docs/lessons.md 2026-09-05) — the splat shaders
// consume them row-vector style. PBRLightAttribs carries no matrices.
static void splatFrameTranspose(Diligent::HLSL::PBRFrameAttribs& f) {
    Diligent::HLSL::CameraAttribs* cams[2] = {&f.Camera, &f.PrevCamera};
    for (Diligent::HLSL::CameraAttribs* cam : cams) {
        cam->mView        = cam->mView.Transpose();
        cam->mProj        = cam->mProj.Transpose();
        cam->mViewProj    = cam->mViewProj.Transpose();
        cam->mViewInv     = cam->mViewInv.Transpose();
        cam->mProjInv     = cam->mProjInv.Transpose();
        cam->mViewProjInv = cam->mViewProjInv.Transpose();
    }
    f.ShadowMaps[0].WorldToLightProjSpace = f.ShadowMaps[0].WorldToLightProjSpace.Transpose();
}

// The PBR frame attribs the splat PS consumes (HLSL::PBRFrameAttribs):
// filled with the same getters GltfDiligent.cpp fillFrameAttribs uses.
static void splatFrameFill(void) {
    Diligent::HLSL::PBRFrameAttribs& f = splatFrameStaging.frame;
    const float4x4 view = diligentFrameView();
    const float4x4 proj = diligentFrameProj();
    f64 anchorF64[3];
    diligentWorldAnchor(anchorF64);

    Diligent::HLSL::CameraAttribs& camera = f.Camera;
    camera.mView        = view;
    camera.mProj        = proj;
    camera.mViewProj    = view * proj;
    camera.mViewInv     = view.Inverse();
    camera.mProjInv     = proj.Inverse();
    camera.mViewProjInv = camera.mViewProj.Inverse();
    camera.f4Position   = float4(float3::MakeVector(camera.mViewInv[3]), 1.0f);
    u32 vpW = 0, vpH = 0;
    taaTargetSize(&vpW, &vpH);
    const SwapChainDesc& scDesc = swapChain->GetDesc();
    if (vpW == 0 || vpH == 0) {
        vpW = scDesc.Width;
        vpH = scDesc.Height;
    }
    camera.f4ViewportSize = float4{(float)vpW, (float)vpH, 1.0f / (float)vpW, 1.0f / (float)vpH};
    camera.SetClipPlanes(engine::renderer::kCameraNear, engine::renderer::kCameraFar);
    camera.fHandness = view.Determinant() > 0 ? 1.0f : -1.0f;
    camera.f2Jitter  = float2{taaCurrentJitterX(), taaCurrentJitterY()};
    f.PrevCamera = *static_cast<const Diligent::HLSL::CameraAttribs*>(taaPrevCameraAttribs());
    {  // prev-camera anchor correction (same as fillFrameAttribs)
        f32 dEye[3];
        taaPrevAnchorDelta(dEye);
        float4x4 t = float4x4::Identity();
        t._41 = dEye[0];
        t._42 = dEye[1];
        t._43 = dEye[2];
        f.PrevCamera.mViewProj = t * f.PrevCamera.mViewProj;
    }

    Diligent::HLSL::PBRRendererShaderParameters& renderer = f.Renderer;
    renderer.OcclusionStrength = 1.0f;
    renderer.EmissionScale     = 1.0f;
    renderer.AverageLogLum     = 0.25f;
    renderer.MiddleGray        = 0.18f;
    renderer.WhitePoint        = 3.0f;
    renderer.PrefilteredCubeLastMip =
            iblDiligentReady() ? iblDiligentPrefilteredLastMip() : 0.0f;
    renderer.EnvironmentRotation = float2(1.0f, 0.0f);
    const f32 iblScale = iblDiligentGetIntensity();
    renderer.IBLScale = float4{iblScale, iblScale, iblScale, 1.0f};
    renderer.HighlightColor = float4{1.0f, 0.0f, 0.0f, 0.0f};
    renderer.UnshadedColor  = float4{0.5f, 0.5f, 0.5f, 1.0f};
    renderer.PointSize      = 1.0f;
    renderer.MipBias        = 0.0f;
    renderer.LightCount     = 1;
    renderer.DebugView      = 0;

    // Directional sun (frame.Lights[0]), scaled like the PBR pass
    Diligent::HLSL::PBRLightAttribs& light = f.Lights[0];
    const f32* sunDir     = diligentSunDirection();
    const f32* sunColor   = diligentSunColor();
    const f32  sunIntens = diligentSunIntensity();
    const f32  sl = std::sqrt(sunDir[0] * sunDir[0] + sunDir[1] * sunDir[1] + sunDir[2] * sunDir[2]);
    light.Type           = 1;  // GLTF::Light::TYPE::DIRECTIONAL
    light.PosX           = 0.0f;
    light.PosY           = 0.0f;
    light.PosZ           = 0.0f;
    light.DirectionX     = sl > 0.0f ? sunDir[0] / sl : 0.0f;
    light.DirectionY     = sl > 0.0f ? sunDir[1] / sl : 1.0f;
    light.DirectionZ     = sl > 0.0f ? sunDir[2] / sl : 0.0f;
    light.ShadowMapIndex = splatShadowsOn() ? 0 : -1;
    const f32 scaled = sunIntens > 0.0f ? sunIntens * (3.0f / 110000.0f) : 0.0f;
    light.IntensityR     = sunColor[0] * scaled;
    light.IntensityG     = sunColor[1] * scaled;
    light.IntensityB     = sunColor[2] * scaled;
    light.Range4         = 0.0f;
    light.SpotAngleScale = 0.0f;
    light.SpotAngleOffset = 0.0f;
    if (light.ShadowMapIndex >= 0) {
        Diligent::HLSL::PBRShadowMapInfo& sm = f.ShadowMaps[0];
        sm.WorldToLightProjSpace = *shadowDiligentPbrWorldToLightProj();
        sm.UVScale               = float2{1.0f, 1.0f};
        sm.UVBias                = float2{0.0f, 0.0f};
        sm.ShadowMapSlice        = shadowDiligentPbrSlice();
        float bias               = shadowDiligentPbrDepthBias();
        if (const char* s = getenv("ENGINE_PBR_BIAS_SCALE")) {
            bias *= (float)atof(s);
        }
        sm.Padding0 = bias;
        sm.Padding1 = 0.0f;
        sm.Padding2 = 0.0f;
    }

    splatFrameTranspose(f);
    splatFrameStaging.anchor = float4{f32(anchorF64[0]), f32(anchorF64[1]), f32(anchorF64[2]), 0.0f};
}

static IShader*                    splatVS = nullptr;
static IShader*                    splatPS = nullptr;
static IPipelineState*             splatPipeline = nullptr;
static IPipelineResourceSignature* splatPRS = nullptr;
static IShaderResourceBinding*     splatSrb = nullptr;
static ISampler*                   splatSampler = nullptr;       // group weights (linear clamp trilinear)
static ISampler*                   splatDetailSampler = nullptr; // tiled details (linear repeat, aniso 16)
static ISampler*                   splatIblSampler = nullptr;    // env cubes + GGX LUT (linear clamp)
static ISampler*                   splatShadowSampler = nullptr; // comparison linear clamp (PCF)
static IBuffer*                    splatFrameCB = nullptr;
static ITexture*                   splatShadowDummyTex = nullptr;
static ITextureView*               splatShadowDummySRV = nullptr;
static ITexture*                   splatIrradianceDummyTex = nullptr;
static ITextureView*               splatIrradianceDummySRV = nullptr;
static ITexture*                   splatPrefilteredDummyTex = nullptr;
static ITextureView*               splatPrefilteredDummySRV = nullptr;
// Dynamic SRB slots (re-set only when the bound resource changes).
static IShaderResourceVariable*    splatSrvIrradiance = nullptr;
static IShaderResourceVariable*    splatSrvPrefiltered = nullptr;
static IShaderResourceVariable*    splatSrvShadow = nullptr;
static ITexture*                   splatBoundIrradiance = nullptr;
static ITexture*                   splatBoundPrefiltered = nullptr;
static ITextureView*               splatBoundShadow = nullptr;
static bool                        splatPassReady = false;
static bool                        splatPassFailed = false;
static u64                         splatFrameNo = 0;

// The static 4x4 chunk grid, derived once from the chunk AABBs (the window
// cull keeps the streaming shape: per-frame cull = frustum + eye-cell ±1).
static bool splatGridValid = false;
static f32  splatGridMin[2] = {0, 0};   // [0] x, [1] z
static f32  splatGridCell[2] = {0, 0};  // [0] x, [1] z
static u32  splatGridW = 0, splatGridH = 0;

IShader* createSplatHlsl(const char* pakPath, const char* name, SHADER_TYPE type) {
    utils::String blob = utils::dataManagerRead(pakPath);
    if (!blob.data || blob.size == 0) {
        utils::warn("splatTerrain: shader source missing from pak: %s", pakPath);
        utils::stringDestroy(&blob);
        return nullptr;
    }
    // Tuning knob for the PS' world-space detail tiling: the shader defaults
    // SPLAT_DETAIL_METERS to 7000.0 (old-engine parity, 1024 px / 7000 m ≈
    // 6.84 m per repeat); a prepended #define overrides it without a repak.
    // (The VS simply ignores the macro.)
    std::string source(blob.data, blob.size);
    if (const char* meters = getenv("ENGINE_SPLAT_DETAIL_METERS")) {
        double m = atof(meters);
        if (m > 0.0) {
            char def[96];
            snprintf(def, sizeof(def), "#define SPLAT_DETAIL_METERS %.6f\n", m);
            source.insert(0, def);
            utils::info("splatTerrain: detail tiling override %.3f m/repeat", m);
        }
    }
    static const char* splatBandTuning[6][2] = {
        {"ENGINE_SPLAT_SAND_LO",   "SPLAT_SAND_LO"},
        {"ENGINE_SPLAT_SAND_HI",    "SPLAT_SAND_HI"},
        {"ENGINE_SPLAT_CLIFF_LO",  "SPLAT_CLIFF_LO"},
        {"ENGINE_SPLAT_CLIFF_HI",  "SPLAT_CLIFF_HI"},
        {"ENGINE_SPLAT_SNOW_LO",   "SPLAT_SNOW_LO"},
        {"ENGINE_SPLAT_SNOW_HI",   "SPLAT_SNOW_HI"},
    };
    for (const auto& bt : splatBandTuning) {
        if (const char* v = getenv(bt[0])) {
            double x = atof(v);
            if (std::isfinite(x)) {
                char def[96];
                snprintf(def, sizeof(def), "#define %s %.6f\n", bt[1], x);
                source.insert(0, def);
                utils::info("splatTerrain: band threshold override %s = %.3f", bt[0], x);
            }
        }
    }
    ShaderCreateInfo ci;
    ci.Desc.Name              = name;
    ci.Desc.ShaderType        = type;
    ci.EntryPoint              = "main";
    ci.Source                  = source.c_str();
    ci.SourceLength            = (Uint32)source.length();
    ci.SourceLanguage          = SHADER_SOURCE_LANGUAGE_HLSL;
    ci.pShaderSourceStreamFactory = &DiligentFXShaderSourceStreamFactory::GetInstance();

    RefCntAutoPtr<IShader> shader;
    RefCntAutoPtr<IDataBlob> output;
    device->CreateShader(ci, &shader, &output);
    utils::stringDestroy(&blob);
    if (!shader) {
        const char* msg = output ? (const char*)output->GetConstDataPtr() : "(no compiler output)";
        utils::warn("splatTerrain: shader compile failed %s: %s", name, msg);
        return nullptr;
    }
    shader->AddRef();
    return shader;
}

void splatPassDeriveGrid(const SplatTerrain* t) {
    f32 mnx = FLT_MAX, mxx = -FLT_MAX, mnz = FLT_MAX, mxz = -FLT_MAX;
    for (const SplatChunk& c : t->chunks) {
        mnx = std::min(mnx, c.aabbMin[0]);
        mxx = std::max(mxx, c.aabbMax[0]);
        mnz = std::min(mnz, c.aabbMin[2]);
        mxz = std::max(mxz, c.aabbMax[2]);
    }
    const u32 n = (u32)t->chunks.size();
    splatGridW = (u32)std::lround(std::sqrt((double)n));
    splatGridH = n / splatGridW;
    if (splatGridH * splatGridW != n) {
        splatGridW = splatGridH = 1;  // non-square grid: the window degenerates to full
    }
    splatGridMin[0] = mnx;
    splatGridMin[1] = mnz;
    splatGridCell[0] = (mxx - mnx) / (f32)splatGridW;
    splatGridCell[1] = (mxz - mnz) / (f32)splatGridH;
    splatGridValid = true;
    utils::info("splatTerrain: chunk grid %ux%u derived from AABBs (cell %.0fx%.0f m)",
            splatGridW, splatGridH, (double)splatGridCell[0], (double)splatGridCell[1]);
}

void splatPassInit(const SplatTerrain* t) {
    if (splatPassReady || splatPassFailed || !device || !context) {
        return;
    }
    if (t->groups.size() != 2) {
        utils::warn("splatTerrain: the minimal pass needs exactly 2 splat groups (got %zu) — draw disabled",
                t->groups.size());
        splatPassFailed = true;
        return;
    }

    splatVS = createSplatHlsl("materials/splat_terrain_vs.hlsl", "splatTerrainVS", SHADER_TYPE_VERTEX);
    splatPS = createSplatHlsl("materials/splat_terrain_ps.hlsl", "splatTerrainPS", SHADER_TYPE_PIXEL);
    if (!splatVS || !splatPS) {
        splatPassRelease();
        splatPassFailed = true;
        return;
    }

    SamplerDesc sd;
    sd.Name       = "splat linear clamp";
    sd.MinFilter  = FILTER_TYPE_LINEAR;
    sd.MagFilter  = FILTER_TYPE_LINEAR;
    sd.MipFilter  = FILTER_TYPE_LINEAR;
    sd.AddressU   = TEXTURE_ADDRESS_CLAMP;
    sd.AddressV   = TEXTURE_ADDRESS_CLAMP;
    device->CreateSampler(sd, &splatSampler);
    if (!splatSampler) {
        splatPassRelease();
        splatPassFailed = true;
        return;
    }
    // The world-tiled detail textures: REPEAT so the ~6.8 m tiling wraps, and
    // anisotropic min/mag (Diligent's Vulkan backend derives
    // anisotropyEnable from IsAnisotropicFilter(MinFilter) — MaxAnisotropy
    // alone on a LINEAR filter is silently ignored) so grazing-angle grain
    // does not alias into soup; the mip chain already ships in the ktx2.
    SamplerDesc sdDetail;
    sdDetail.Name          = "splat detail repeat aniso";
    sdDetail.MinFilter     = FILTER_TYPE_ANISOTROPIC;
    sdDetail.MagFilter     = FILTER_TYPE_ANISOTROPIC;
    sdDetail.MipFilter     = FILTER_TYPE_LINEAR;
    sdDetail.AddressU      = TEXTURE_ADDRESS_WRAP;
    sdDetail.AddressV      = TEXTURE_ADDRESS_WRAP;
    sdDetail.MaxAnisotropy = 16;
    device->CreateSampler(sdDetail, &splatDetailSampler);
    if (!splatDetailSampler) {
        splatPassRelease();
        splatPassFailed = true;
        return;
    }
    device->CreateSampler(sd, &splatIblSampler);
    if (!splatIblSampler) {
        splatPassRelease();
        splatPassFailed = true;
        return;
    }
    // The comparison sampler the PS' SampleCmpLevelZero PCF uses (the PBR
    // pass' Sam_ComparisonLinearClamp equivalent).
    SamplerDesc sdShadow;
    sdShadow.Name           = "splat shadow comparison";
    sdShadow.MinFilter      = FILTER_TYPE_LINEAR;
    sdShadow.MagFilter      = FILTER_TYPE_LINEAR;
    sdShadow.MipFilter      = FILTER_TYPE_LINEAR;
    sdShadow.AddressU       = TEXTURE_ADDRESS_CLAMP;
    sdShadow.AddressV       = TEXTURE_ADDRESS_CLAMP;
    sdShadow.AddressW       = TEXTURE_ADDRESS_CLAMP;
    sdShadow.ComparisonFunc = COMPARISON_FUNC_LESS;
    device->CreateSampler(sdShadow, &splatShadowSampler);
    if (!splatShadowSampler) {
        splatPassRelease();
        splatPassFailed = true;
        return;
    }

    // Fallback 1x1 depth-array shadow SRV + dim constant IBL cubes (the PS
    // binds these while the shadow pass / IBL module are not ready; nothing
    // ever reads the depth, ShadowMapIndex = -1 skips the sample).
    {
        TextureDesc d;
        d.Type      = RESOURCE_DIM_TEX_2D_ARRAY;
        d.Width     = 1;
        d.Height    = 1;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format    = TEX_FORMAT_D32_FLOAT;
        d.Usage     = USAGE_IMMUTABLE;
        d.BindFlags = BIND_DEPTH_STENCIL | BIND_SHADER_RESOURCE;
        d.Name      = "splat shadow dummy";
        u8 zero[4] = {0, 0, 0, 0};  // depth 1.0
        std::vector<TextureSubResData> subres(1);
        subres[0] = TextureSubResData(zero, 4);
        TextureData data;
        data.pSubResources   = subres.data();
        data.NumSubresources = 1;
        data.pContext        = context;
        RefCntAutoPtr<ITexture> tex;
        device->CreateTexture(d, &data, &tex);
        if (!tex) {
            splatPassRelease();
            splatPassFailed = true;
            return;
        }
        splatShadowDummyTex = tex.RawPtr();
        tex->AddRef();
        splatShadowDummySRV = tex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    }
    auto makeDummyCube = [](const char* name) {
        RefCntAutoPtr<ITexture> tex;
        TextureDesc d;
        d.Type      = RESOURCE_DIM_TEX_CUBE;
        d.Width     = 4;
        d.Height    = 4;
        d.MipLevels = 1;
        d.ArraySize = 6;
        d.Format    = TEX_FORMAT_RGBA8_UNORM;
        d.Usage     = USAGE_IMMUTABLE;
        d.BindFlags = BIND_SHADER_RESOURCE;
        d.Name      = name;
        std::vector<u8> px(u64(4) * 4 * 4 * 6, 13);  // 6 faces x 4x4 RGBA8, dim sky grey
        std::vector<TextureSubResData> subres(6);
        for (size_t f = 0; f < 6; f++) {
            subres[f] = TextureSubResData(px.data() + f * 16, 16);
        }
        TextureData data;
        data.pSubResources   = subres.data();
        data.NumSubresources = 6;
        data.pContext        = context;
        device->CreateTexture(d, &data, &tex);
        return tex;
    };
    {
        auto irrTex = makeDummyCube("splat irradiance dummy");
        if (!irrTex) {
            splatPassRelease();
            splatPassFailed = true;
            return;
        }
        splatIrradianceDummyTex = irrTex.RawPtr();
        irrTex->AddRef();
        splatIrradianceDummySRV = irrTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    }
    {
        auto pfTex = makeDummyCube("splat prefiltered dummy");
        if (!pfTex) {
            splatPassRelease();
            splatPassFailed = true;
            return;
        }
        splatPrefilteredDummyTex = pfTex.RawPtr();
        pfTex->AddRef();
        splatPrefilteredDummySRV = pfTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    }

    CreateUniformBuffer(device, sizeof(SplatFrameStaging), "splat frame attribs", &splatFrameCB);
    if (!splatFrameCB) {
        splatPassRelease();
        splatPassFailed = true;
        return;
    }

    // Everything else is static: the weights/detail SRVs never change
    // (immutable textures living until destroy), the dynamic cbuffer is
    // re-committed through the SRB every frame, and the IBL/shadow SRVs are
    // the three dynamic slots (the rmlui pattern — one SRB for the pass).
    PipelineResourceDesc resources[] = {
            {SHADER_TYPE_VERTEX | SHADER_TYPE_PIXEL, "cbSplatFrame", 1,
                    SHADER_RESOURCE_TYPE_CONSTANT_BUFFER, SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_Sampler", 1, SHADER_RESOURCE_TYPE_SAMPLER,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_DetailSampler", 1, SHADER_RESOURCE_TYPE_SAMPLER,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_LinearClampSampler", 1, SHADER_RESOURCE_TYPE_SAMPLER,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_ShadowMap_sampler", 1, SHADER_RESOURCE_TYPE_SAMPLER,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_Weights0", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_Weights1", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_BaseAlbedo", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_BaseNormal", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_Detail0", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_Detail1", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_Detail2", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_Detail3", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_Detail4", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_Detail5", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_Detail6", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_Detail7", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_DetailN0", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_DetailN1", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_DetailN2", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_DetailN3", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_DetailN4", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_DetailN5", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_DetailN6", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_DetailN7", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_SnowAlbedo", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_SnowNormal", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_SandAlbedo", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_SandNormal", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_CliffAlbedo", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_CliffNormal", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_PreintegratedGGX", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_IrradianceMap", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
            {SHADER_TYPE_PIXEL, "g_PrefilteredEnvMap", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
            {SHADER_TYPE_PIXEL, "g_ShadowMap", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
    };
    PipelineResourceSignatureDesc prsDesc;
    prsDesc.Resources      = resources;
    prsDesc.NumResources   = (Uint32)(sizeof(resources) / sizeof(resources[0]));
    prsDesc.BindingIndex   = 0;
    device->CreatePipelineResourceSignature(prsDesc, &splatPRS);
    if (!splatPRS) {
        splatPassRelease();
        splatPassFailed = true;
        return;
    }
    if (IShaderResourceVariable* v = splatPRS->GetStaticVariableByName(SHADER_TYPE_VERTEX, "cbSplatFrame")) {
        v->Set(splatFrameCB, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    }
    if (IShaderResourceVariable* v = splatPRS->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_Sampler")) {
        v->Set(splatSampler, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    }
    if (IShaderResourceVariable* v = splatPRS->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_DetailSampler")) {
        v->Set(splatDetailSampler, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    }
    if (IShaderResourceVariable* v = splatPRS->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_LinearClampSampler")) {
        v->Set(splatIblSampler, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    }
    if (IShaderResourceVariable* v = splatPRS->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_ShadowMap_sampler")) {
        v->Set(splatShadowSampler, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    }
    auto setView = [](const char* name, Diligent::ITextureView* view) {
        if (IShaderResourceVariable* v = splatPRS->GetStaticVariableByName(SHADER_TYPE_PIXEL, name)) {
            v->Set(view);
        }
    };
    setView("g_Weights0", t->groups[0].weightsView);
    setView("g_Weights1", t->groups[1].weightsView);
    // Base material: the first detail set of the first group (always loaded —
    // a detail load failure fails the whole pass), sampled under the splat
    // chain by the PS so Noop-weight regions are not black.
    setView("g_BaseAlbedo", t->groups[0].details[0].albedoView);
    setView("g_BaseNormal", t->groups[0].details[0].normalView);
    const char* detailNames[8] = {"g_Detail0", "g_Detail1", "g_Detail2", "g_Detail3",
            "g_Detail4", "g_Detail5", "g_Detail6", "g_Detail7"};
    const char* normalNames[8] = {"g_DetailN0", "g_DetailN1", "g_DetailN2", "g_DetailN3",
            "g_DetailN4", "g_DetailN5", "g_DetailN6", "g_DetailN7"};
    for (int c = 0; c < 8; c++) {
        setView(detailNames[c], t->groups[c / 4].details[c % 4].albedoView);
        setView(normalNames[c], t->groups[c / 4].details[c % 4].normalView);
    }
    {
        const char* bandAlbedoNames[3] = {"g_SnowAlbedo", "g_SandAlbedo", "g_CliffAlbedo"};
        const char* bandNormalNames[3] = {"g_SnowNormal", "g_SandNormal", "g_CliffNormal"};
        for (int b = 0; b < 3; b++) {
            if (!t->band[b].albedoView || !t->band[b].normalView) {
                utils::warn("splatTerrain: band set '%s' views missing — draw disabled", t->band[b].name.c_str());
                splatPassRelease();
                splatPassFailed = true;
                return;
            }
            setView(bandAlbedoNames[b], t->band[b].albedoView);
            setView(bandNormalNames[b], t->band[b].normalView);
        }
    }
    // The preintegrated GGX LUT lives in the PBR renderer (borrowed view —
    // owned by the renderer, never released here).
    if (Diligent::ITextureView* ggx = (Diligent::ITextureView*)engine::gltf::gltfDiligentPreintegratedGGX()) {
        setView("g_PreintegratedGGX", ggx);
    } else {
        utils::warn("splatTerrain: PBR preintegrated GGX LUT unavailable — draw disabled");
        splatPassRelease();
        splatPassFailed = true;
        return;
    }

    RefCntAutoPtr<IShaderResourceBinding> b;
    splatPRS->CreateShaderResourceBinding(&b, true);
    if (!b) {
        splatPassRelease();
        splatPassFailed = true;
        return;
    }
    b->AddRef();
    splatSrb = b;
    splatSrvIrradiance  = splatSrb->GetVariableByName(SHADER_TYPE_PIXEL, "g_IrradianceMap");
    splatSrvPrefiltered = splatSrb->GetVariableByName(SHADER_TYPE_PIXEL, "g_PrefilteredEnvMap");
    splatSrvShadow      = splatSrb->GetVariableByName(SHADER_TYPE_PIXEL, "g_ShadowMap");
    if (!splatSrvIrradiance || !splatSrvPrefiltered || !splatSrvShadow) {
        utils::warn("splatTerrain: dynamic SRB slots missing");
        splatPassRelease();
        splatPassFailed = true;
        return;
    }

    // The world pass always renders into the TAA offscreen chain (linear
    // RGBA16F + RG16F motion + RGBA16F normal + D32 — the PBR pass' formats),
    // so the pipeline declares all three RTs even though task 2's PS writes
    // them as flat-sun color / zero motion / world normal.
    GraphicsPipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name             = "splatTerrain";
    psoCI.ppResourceSignatures     = &splatPRS;
    psoCI.ResourceSignaturesCount  = 1;

    GraphicsPipelineDesc& gp = psoCI.GraphicsPipeline;
    gp.NumRenderTargets           = 3;
    gp.RTVFormats[0]               = TEX_FORMAT_RGBA16_FLOAT;
    gp.RTVFormats[1]               = TEX_FORMAT_RG16_FLOAT;
    gp.RTVFormats[2]               = TEX_FORMAT_RGBA16_FLOAT;
    gp.DSVFormat                   = TEX_FORMAT_D32_FLOAT;
    gp.PrimitiveTopology           = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    gp.RasterizerDesc.FrontCounterClockwise = True;  // glTF front faces are CCW (the PBR pass')
    gp.RasterizerDesc.CullMode     = CULL_MODE_BACK;
    gp.DepthStencilDesc.DepthEnable = True;
    gp.DepthStencilDesc.DepthWriteEnable = True;
    static const LayoutElement inputLayout[] = {
            {"ATTRIB", 0, 0, 3, VT_FLOAT32, False, 0u, 48u},
            {"ATTRIB", 1, 0, 3, VT_FLOAT32, False, 12u, 48u},
            {"ATTRIB", 2, 0, 4, VT_FLOAT32, False, 24u, 48u},
            {"ATTRIB", 3, 0, 2, VT_FLOAT32, False, 40u, 48u},
    };
    gp.InputLayout.LayoutElements = inputLayout;
    gp.InputLayout.NumElements   = 4;

    psoCI.pVS = splatVS;
    psoCI.pPS = splatPS;
    device->CreateGraphicsPipelineState(psoCI, &splatPipeline);
    if (!splatPipeline) {
        utils::warn("splatTerrain: PSO creation failed");
        splatPassRelease();
        splatPassFailed = true;
        return;
    }
    splatPassReady = true;
    utils::info("splatTerrain: draw pass ready (shaders + PSO + SRB)");
}

i32 splatCellOf(const SplatChunk& ch, int axis) {
    const int a = axis == 2 ? 1 : 0;  // the grid arrays are [x, z]
    const f32 center = (ch.aabbMin[axis] + ch.aabbMax[axis]) * 0.5f;
    i32 cell = (i32)std::floor((f64)(center - splatGridMin[a]) / (f64)splatGridCell[a]);
    const i32 maxCell = (i32)(a == 0 ? splatGridW : splatGridH) - 1;
    return cell < 0 ? 0 : (cell > maxCell ? maxCell : cell);
}

// Re-set the dynamic IBL/shadow SRV slots only when the bound resource
// changes (the IBL env cycling recreates the cubes, the shadow mode toggles
// the atlas SRV; while the modules are off the dummies keep the slots set).
static void splatBindDynamicResources(void) {
    ITexture* irr = iblDiligentReady() ? iblDiligentIrradianceCube() : splatIrradianceDummyTex;
    ITexture* pfl = iblDiligentReady() ? iblDiligentPrefilteredCube() : splatPrefilteredDummyTex;
    ITextureView* shd = splatShadowsOn() ? shadowDiligentShadowSRV() : splatShadowDummySRV;
    if (splatBoundIrradiance != irr) {
        splatBoundIrradiance = irr;
        splatSrvIrradiance->Set(irr ? irr->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr);
    }
    if (splatBoundPrefiltered != pfl) {
        splatBoundPrefiltered = pfl;
        splatSrvPrefiltered->Set(pfl ? pfl->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr);
    }
    if (splatBoundShadow != shd) {
        splatBoundShadow = shd;
        splatSrvShadow->Set(shd);
    }
}

bool splatTerrainDrawDiligent(Diligent::IDeviceContext* ctx) {
    (void)ctx;  // the pass uses the shared context (the world pass set it up)
    const char* gate = getenv("ENGINE_SPLAT_TERRAIN");
    if (gate && gate[0] == '0') {
        return false;
    }
    const SplatTerrain* t = splatTerrainDiligent();
    if (!t || t->chunks.empty()) {
        return false;
    }
    // The splat PSO is a 3-RT + D32 pipeline: it only fits the offscreen
    // world chain (the legacy single-RT fallback keeps the PBR scene draw).
    if (!taaColorRTV()) {
        return false;
    }
    if (!splatPassReady && !splatPassFailed) {
        splatPassInit(t);
    }
    if (!splatPassReady) {
        return false;
    }

    const float4x4 view = diligentFrameView();
    const float4x4 proj = diligentFrameProj();
    f64 anchorF64[3];
    diligentWorldAnchor(anchorF64);
    const f32 anchor[3] = {f32(anchorF64[0]), f32(anchorF64[1]), f32(anchorF64[2])};

    // Per-frame cbuffer upload (dynamic ring: MapBuffer, then re-commit the
    // SRB below with this frame's offset).
    splatFrameFill();
    {
        void* dst = nullptr;
        context->MapBuffer(splatFrameCB, MAP_WRITE, MAP_FLAG_DISCARD, dst);
        std::memcpy(dst, &splatFrameStaging, sizeof(splatFrameStaging));
        context->UnmapBuffer(splatFrameCB, MAP_WRITE);
    }
    splatBindDynamicResources();

    // Culling: the D3D-style Vulkan NDC (z 0..1) frustum of view*proj in the
    // anchored space (the AABBs are shifted by the anchor) + the 1-chunk
    // camera window over the static grid. ENGINE_SPLAT_WINDOW=0 disables the
    // window (frustum-only) for whole-map validation shots.
    const char* windowGate = getenv("ENGINE_SPLAT_WINDOW");
    const bool windowOn = !(windowGate && windowGate[0] == '0');
    FrustumCullPlanes planes;
    frustumCullFromVP(view * proj, false, planes);
    if (!splatGridValid) {
        splatPassDeriveGrid(t);
    }
    const i32 maxCellX = (i32)splatGridW - 1;
    const i32 maxCellZ = (i32)splatGridH - 1;
    i32 eyeCellX = (i32)std::floor((f64)(anchor[0] - splatGridMin[0]) / (f64)splatGridCell[0]);
    i32 eyeCellZ = (i32)std::floor((f64)(anchor[2] - splatGridMin[1]) / (f64)splatGridCell[1]);
    eyeCellX = eyeCellX < 0 ? 0 : (eyeCellX > maxCellX ? maxCellX : eyeCellX);
    eyeCellZ = eyeCellZ < 0 ? 0 : (eyeCellZ > maxCellZ ? maxCellZ : eyeCellZ);

    context->SetPipelineState(splatPipeline);
    context->CommitShaderResources(splatSrb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    size_t drawn = 0;
    for (size_t ci = 0; ci < t->chunks.size(); ci++) {
        const SplatChunk& ch = t->chunks[ci];
        if (windowOn &&
            (std::abs(splatCellOf(ch, 0) - eyeCellX) > 1 ||
             std::abs(splatCellOf(ch, 2) - eyeCellZ) > 1)) {
            continue;
        }
        const f32 bmin[3] = {ch.aabbMin[0] - anchor[0], ch.aabbMin[1] - anchor[1],
                ch.aabbMin[2] - anchor[2]};
        const f32 bmax[3] = {ch.aabbMax[0] - anchor[0], ch.aabbMax[1] - anchor[1],
                ch.aabbMax[2] - anchor[2]};
        if (frustumCullAabbOutside(bmin, bmax, planes)) {
            continue;
        }
        IBuffer* vb = ch.vbo;
        context->SetVertexBuffers(0, 1, &vb, nullptr,
                RESOURCE_STATE_TRANSITION_MODE_TRANSITION, SET_VERTEX_BUFFERS_FLAG_RESET);
        context->SetIndexBuffer(ch.ibo, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->DrawIndexed(DrawIndexedAttribs{
                (Uint32)ch.indexCount, VT_UINT16, DRAW_FLAG_NONE, 1, 0, 0, 0});
        drawn++;
    }
    splatFrameNo++;
    if (getenv("ENGINE_SPLAT_PROBE") || splatFrameNo == 1 || splatFrameNo % 300 == 0) {
        utils::info("splatTerrain: frame %llu — %zu/%zu chunks drawn (frustum + 1-chunk window)",
                (unsigned long long)splatFrameNo, drawn, t->chunks.size());
    }
    return true;
}

struct SplatShadowCasterStaging {
    Diligent::float4x4 cLightViewProj;
    Diligent::float4 g_Anchor;
};
static_assert(sizeof(SplatShadowCasterStaging) == 80, "splat shadow caster cbuffer layout");

static IShader*                    splatShadowVS = nullptr;
static IShader*                    splatShadowPS = nullptr;
static IPipelineState*             splatShadowCasterPipeline = nullptr;
static IPipelineResourceSignature* splatShadowCasterPRS = nullptr;
static IShaderResourceBinding*     splatShadowCasterSrb = nullptr;
static IBuffer*                    splatShadowCasterCB = nullptr;
static ITexture*                   splatShadowCasterDummyRT = nullptr;
static ITextureView*               splatShadowCasterDummyRTV = nullptr;
static u32                          splatShadowCasterDummySize = 0;
static bool                        splatShadowCasterUsesDummyRT = false;
static bool                        splatShadowCasterReady = false;
static bool                        splatShadowCasterFailed = false;
static u64                         splatShadowCasterFrameNo = 0;

bool splatTerrainShadowDrawsDiligent(void) {
    const char* gate = getenv("ENGINE_SPLAT_TERRAIN");
    if (gate && gate[0] == '0') {
        return false;
    }
    const SplatTerrain* t = splatTerrainDiligent();
    return t && !t->chunks.empty() && taaColorRTV() != nullptr && splatShadowsOn();
}

static void splatShadowCasterRelease(void) {
    if (splatShadowCasterSrb) { splatShadowCasterSrb->Release(); splatShadowCasterSrb = nullptr; }
    if (splatShadowCasterPRS) { splatShadowCasterPRS->Release(); splatShadowCasterPRS = nullptr; }
    if (splatShadowCasterPipeline) { splatShadowCasterPipeline->Release(); splatShadowCasterPipeline = nullptr; }
    if (splatShadowVS) { splatShadowVS->Release(); splatShadowVS = nullptr; }
    if (splatShadowPS) { splatShadowPS->Release(); splatShadowPS = nullptr; }
    if (splatShadowCasterCB) { splatShadowCasterCB->Release(); splatShadowCasterCB = nullptr; }
    if (splatShadowCasterDummyRT) { splatShadowCasterDummyRT->Release(); splatShadowCasterDummyRT = nullptr; }
    splatShadowCasterDummyRTV = nullptr;
    splatShadowCasterDummySize = 0;
    splatShadowCasterUsesDummyRT = false;
    splatShadowCasterReady = false;
    splatShadowCasterFailed = false;
    splatShadowCasterFrameNo = 0;
}

static void splatShadowCasterInit(void) {
    if (splatShadowCasterReady || splatShadowCasterFailed || !device || !context) {
        return;
    }
    splatShadowVS = createSplatHlsl("materials/splat_terrain_shadow_vs.hlsl", "splatTerrainShadowVS", SHADER_TYPE_VERTEX);
    splatShadowPS = createSplatHlsl("materials/splat_terrain_shadow_ps.hlsl", "splatTerrainShadowPS", SHADER_TYPE_PIXEL);
    if (!splatShadowVS || !splatShadowPS) {
        splatShadowCasterRelease();
        splatShadowCasterFailed = true;
        return;
    }

    CreateUniformBuffer(device, sizeof(SplatShadowCasterStaging), "splat shadow caster cb", &splatShadowCasterCB);
    if (!splatShadowCasterCB) {
        splatShadowCasterRelease();
        splatShadowCasterFailed = true;
        return;
    }

    PipelineResourceDesc resources[] = {
            {SHADER_TYPE_VERTEX | SHADER_TYPE_PIXEL, "cbSplatShadowCaster", 1,
                    SHADER_RESOURCE_TYPE_CONSTANT_BUFFER, SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    PipelineResourceSignatureDesc prsDesc;
    prsDesc.Resources      = resources;
    prsDesc.NumResources   = 1;
    prsDesc.BindingIndex   = 0;
    device->CreatePipelineResourceSignature(prsDesc, &splatShadowCasterPRS);
    if (!splatShadowCasterPRS) {
        splatShadowCasterRelease();
        splatShadowCasterFailed = true;
        return;
    }
    if (IShaderResourceVariable* v = splatShadowCasterPRS->GetStaticVariableByName(SHADER_TYPE_VERTEX, "cbSplatShadowCaster")) {
        v->Set(splatShadowCasterCB, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    }
    RefCntAutoPtr<IShaderResourceBinding> b;
    splatShadowCasterPRS->CreateShaderResourceBinding(&b, true);
    if (!b) {
        splatShadowCasterRelease();
        splatShadowCasterFailed = true;
        return;
    }
    b->AddRef();
    splatShadowCasterSrb = b;

    GraphicsPipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name             = "splatTerrainShadowCaster";
    psoCI.ppResourceSignatures     = &splatShadowCasterPRS;
    psoCI.ResourceSignaturesCount  = 1;
    GraphicsPipelineDesc& gp = psoCI.GraphicsPipeline;
    gp.NumRenderTargets           = 0;
    gp.DSVFormat                   = TEX_FORMAT_D32_FLOAT;
    gp.PrimitiveTopology           = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    gp.RasterizerDesc.FrontCounterClockwise = True;
    gp.RasterizerDesc.CullMode     = CULL_MODE_BACK;
    gp.DepthStencilDesc.DepthEnable = True;
    gp.DepthStencilDesc.DepthWriteEnable = True;
    static const LayoutElement inputLayout[] = {
            {"ATTRIB", 0, 0, 3, VT_FLOAT32, False, 0u, 48u},
            {"ATTRIB", 1, 0, 3, VT_FLOAT32, False, 12u, 48u},
            {"ATTRIB", 2, 0, 4, VT_FLOAT32, False, 24u, 48u},
            {"ATTRIB", 3, 0, 2, VT_FLOAT32, False, 40u, 48u},
    };
    gp.InputLayout.LayoutElements = inputLayout;
    gp.InputLayout.NumElements   = 4;
    psoCI.pVS = splatShadowVS;
    psoCI.pPS = splatShadowPS;
    device->CreateGraphicsPipelineState(psoCI, &splatShadowCasterPipeline);
    if (!splatShadowCasterPipeline) {
        gp.NumRenderTargets           = 1;
        gp.RTVFormats[0]               = TEX_FORMAT_RGBA16_FLOAT;
        device->CreateGraphicsPipelineState(psoCI, &splatShadowCasterPipeline);
        if (!splatShadowCasterPipeline) {
            utils::warn("splatTerrain: shadow caster PSO creation failed (0-RT and dummy-RT)");
            splatShadowCasterRelease();
            splatShadowCasterFailed = true;
            return;
        }
        splatShadowCasterUsesDummyRT = true;
    }
    splatShadowCasterReady = true;
    utils::info("splatTerrain: shadow caster pass ready%s",
            splatShadowCasterUsesDummyRT ? " (dummy RT fallback)" : "");
}

static void splatShadowCasterCornerNDC(const Diligent::float4x4& m, const f32 p[3], f32 out[3]) {
    const f32 w = m.m[3][0] * p[0] + m.m[3][1] * p[1] + m.m[3][2] * p[2] + m.m[3][3];
    for (int i = 0; i < 3; ++i) {
        out[i] = (m.m[i][0] * p[0] + m.m[i][1] * p[1] + m.m[i][2] * p[2] + m.m[i][3]) / w;
    }
}

static bool splatShadowCasterChunkVisible(const Diligent::float4x4& lightViewProjRowMajor,
        const f32 aabbMin[3], const f32 aabbMax[3], const f32 anchor[3],
        f32 ndcMinZ, f32 margin) {
    f32 minx = FLT_MAX, maxx = -FLT_MAX;
    f32 miny = FLT_MAX, maxy = -FLT_MAX;
    f32 minz = FLT_MAX, maxz = -FLT_MAX;
    for (int i = 0; i < 8; ++i) {
        const f32 p[3] = {(i & 1) ? aabbMax[0] : aabbMin[0],
                          (i & 2) ? aabbMax[1] : aabbMin[1],
                          (i & 4) ? aabbMax[2] : aabbMin[2]};
        const f32 q[3] = {p[0] - anchor[0], p[1] - anchor[1], p[2] - anchor[2]};
        f32 c[3];
        splatShadowCasterCornerNDC(lightViewProjRowMajor, q, c);
        minx = std::min(minx, c[0]);
        maxx = std::max(maxx, c[0]);
        miny = std::min(miny, c[1]);
        maxy = std::max(maxy, c[1]);
        minz = std::min(minz, c[2]);
        maxz = std::max(maxz, c[2]);
    }
    return minx < 1.0f + margin && maxx > -1.0f - margin &&
           miny < 1.0f + margin && maxy > -1.0f - margin &&
           minz < 1.0f + margin && maxz > ndcMinZ - margin;
}

void splatTerrainShadowDrawDiligent(Diligent::IDeviceContext* ctx,
                                    const Diligent::float4x4& lightViewProjRowMajor,
                                    Diligent::ITextureView* cascadeDSV,
                                    int cascadeIndex) {
    if (!splatTerrainShadowDrawsDiligent()) {
        return;
    }
    if (!splatShadowCasterReady && !splatShadowCasterFailed) {
        splatShadowCasterInit();
    }
    if (!splatShadowCasterReady) {
        return;
    }
    const SplatTerrain* t = splatTerrainDiligent();

    static const bool casterNoCull = getenv("ENGINE_SHADOW_CASTER_NOCULL") != nullptr;
    const u32 cascadeSize = cascadeDSV->GetTexture()->GetDesc().Width;

    if (splatShadowCasterUsesDummyRT) {
        if (splatShadowCasterDummySize != cascadeSize) {
            if (splatShadowCasterDummyRT) {
                splatShadowCasterDummyRT->Release();
                splatShadowCasterDummyRT = nullptr;
            }
            splatShadowCasterDummyRTV = nullptr;
            TextureDesc desc;
            desc.Type      = RESOURCE_DIM_TEX_2D;
            desc.Width     = cascadeSize;
            desc.Height    = cascadeSize;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.BindFlags = BIND_RENDER_TARGET;
            desc.Format    = TEX_FORMAT_RGBA16_FLOAT;
            desc.Name      = "splat shadow caster dummy RT";
            device->CreateTexture(desc, nullptr, &splatShadowCasterDummyRT);
            if (splatShadowCasterDummyRT) {
                splatShadowCasterDummyRTV = splatShadowCasterDummyRT->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
            }
            splatShadowCasterDummySize = cascadeSize;
        }
        if (!splatShadowCasterDummyRTV) {
            return;
        }
        ITextureView* rtv = splatShadowCasterDummyRTV;
        ctx->SetRenderTargets(1, &rtv, cascadeDSV, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    } else {
        ctx->SetRenderTargets(0, nullptr, cascadeDSV, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }

    SplatShadowCasterStaging staging;
    staging.cLightViewProj = lightViewProjRowMajor.Transpose();
    f64 anchorF64[3];
    diligentWorldAnchor(anchorF64);
    staging.g_Anchor = float4{f32(anchorF64[0]), f32(anchorF64[1]), f32(anchorF64[2]), 0.0f};
    {
        void* dst = nullptr;
        ctx->MapBuffer(splatShadowCasterCB, MAP_WRITE, MAP_FLAG_DISCARD, dst);
        std::memcpy(dst, &staging, sizeof(staging));
        ctx->UnmapBuffer(splatShadowCasterCB, MAP_WRITE);
    }

    f32 ndcMinZ = 0.0f;
    f32 margin  = 0.0f;
    if (!casterNoCull) {
        ndcMinZ = device->GetDeviceInfo().NDC.MinZ;
        margin  = 4.0f / static_cast<f32>(cascadeSize);
    }
    const f32 anchor[3] = {staging.g_Anchor.x, staging.g_Anchor.y, staging.g_Anchor.z};

    ctx->SetPipelineState(splatShadowCasterPipeline);
    ctx->CommitShaderResources(splatShadowCasterSrb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    size_t drawn = 0;
    for (const SplatChunk& ch : t->chunks) {
        if (!casterNoCull && !splatShadowCasterChunkVisible(lightViewProjRowMajor,
                ch.aabbMin, ch.aabbMax, anchor, ndcMinZ, margin)) {
            continue;
        }
        IBuffer* vb = ch.vbo;
        ctx->SetVertexBuffers(0, 1, &vb, nullptr,
                RESOURCE_STATE_TRANSITION_MODE_TRANSITION, SET_VERTEX_BUFFERS_FLAG_RESET);
        ctx->SetIndexBuffer(ch.ibo, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        ctx->DrawIndexed(DrawIndexedAttribs{
                (Uint32)ch.indexCount, VT_UINT16, DRAW_FLAG_NONE, 1, 0, 0, 0});
        drawn++;
    }
    splatShadowCasterFrameNo++;
    if (getenv("ENGINE_SHADOW_CASTER_PROBE") || splatShadowCasterFrameNo == 1 || splatShadowCasterFrameNo % 300 == 0) {
        if (casterNoCull) {
            utils::info("splatTerrain: shadow caster frame %llu — %zu/%zu chunks drawn (unculled, per-cascade)",
                    (unsigned long long)splatShadowCasterFrameNo, drawn, t->chunks.size());
        } else {
            utils::info("splatTerrain: shadow caster frame %llu cascade %d — %zu/%zu chunks drawn (culled)",
                    (unsigned long long)splatShadowCasterFrameNo, cascadeIndex, drawn, t->chunks.size());
        }
    }
}

void splatPassRelease(void) {
    if (splatSrvIrradiance) { splatSrvIrradiance = nullptr; }
    if (splatSrvPrefiltered) { splatSrvPrefiltered = nullptr; }
    if (splatSrvShadow) { splatSrvShadow = nullptr; }
    splatBoundIrradiance = nullptr;
    splatBoundPrefiltered = nullptr;
    splatBoundShadow = nullptr;
    if (splatSrb) { splatSrb->Release(); splatSrb = nullptr; }
    if (splatPRS) { splatPRS->Release(); splatPRS = nullptr; }
    if (splatPipeline) { splatPipeline->Release(); splatPipeline = nullptr; }
    if (splatVS) { splatVS->Release(); splatVS = nullptr; }
    if (splatPS) { splatPS->Release(); splatPS = nullptr; }
    if (splatFrameCB) { splatFrameCB->Release(); splatFrameCB = nullptr; }
    if (splatShadowDummyTex) { splatShadowDummyTex->Release(); splatShadowDummyTex = nullptr; }
    splatShadowDummySRV = nullptr;
    if (splatIrradianceDummyTex) { splatIrradianceDummyTex->Release(); splatIrradianceDummyTex = nullptr; }
    splatIrradianceDummySRV = nullptr;
    if (splatPrefilteredDummyTex) { splatPrefilteredDummyTex->Release(); splatPrefilteredDummyTex = nullptr; }
    splatPrefilteredDummySRV = nullptr;
    if (splatSampler) { splatSampler->Release(); splatSampler = nullptr; }
    if (splatDetailSampler) { splatDetailSampler->Release(); splatDetailSampler = nullptr; }
    if (splatIblSampler) { splatIblSampler->Release(); splatIblSampler = nullptr; }
    if (splatShadowSampler) { splatShadowSampler->Release(); splatShadowSampler = nullptr; }
    splatPassReady = false;
    splatPassFailed = false;
    splatFrameNo = 0;
    splatGridValid = false;
    splatShadowCasterRelease();
}

void splatTerrainDestroyDiligent(void) {
    splatPassRelease();
    if (!terrain) {
        return;
    }
    for (const SplatChunk& c : terrain->chunks) {
        if (c.vbo) {
            c.vbo->Release();
        }
        if (c.ibo) {
            c.ibo->Release();
        }
    }
    // Default views are owned by their textures (GetDefaultView does not
    // AddRef) — release only the textures and the views disappear with them.
    for (const SplatGroup& g : terrain->groups) {
        if (g.weights) {
            g.weights->Release();
        }
    }
    // Detail textures are shared by reference between groups (deduped by
    // name at load) — release each unique texture once.
    {
        std::unordered_set<const ITexture*> texReleased;
        for (const SplatGroup& g : terrain->groups) {
            for (const SplatDetail& d : g.details) {
                if (d.albedo && texReleased.insert(d.albedo).second) {
                    d.albedo->Release();
                }
                if (d.normal && texReleased.insert(d.normal).second) {
                    d.normal->Release();
                }
            }
        }
    }
    for (const SplatDetail& band : terrain->band) {
        if (band.albedo) {
            band.albedo->Release();
        }
        if (band.normal) {
            band.normal->Release();
        }
    }
    delete terrain;
    terrain = nullptr;
    utils::info("splatTerrain: destroyed");
}

const SplatTerrain* splatTerrainDiligent(void) {
    return terrain;
}

}
