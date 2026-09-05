#include "renderer/diligent/HeightmapTerrainDiligent.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "ecs/system/heightmap/HeightmapLattice.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "gltf/GltfInternal.h"
#include "image/Image.h"
#include "logger/Logger.h"
#include "renderer/RenderBackend.h"
#include "renderer/Renderer.h"
#include "renderer/diligent/DiligentRenderer.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "DiligentFXShaderSourceStreamFactory.hpp"
#include "Engine.h"
#include "Graphics/GraphicsEngine/interface/Buffer.h"
#include "Graphics/GraphicsEngine/interface/BufferView.h"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/PipelineResourceSignature.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Sampler.h"
#include "Graphics/GraphicsEngine/interface/SwapChain.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"
#include "Graphics/GraphicsTools/interface/GraphicsUtilities.h"
#include "Graphics/GraphicsTools/interface/MapHelper.hpp"

#include <GLTFLoader.hpp>
#include <GLTF_PBR_Renderer.hpp>
#include <TextureLoader.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// PBR frame attribs layout (identical fill as the glTF PBR pass, see
// GltfDiligent.cpp fillFrameAttribs)
namespace Diligent {
namespace HLSL {
#include <Shaders/Common/public/BasicStructures.fxh>
#include <Shaders/PBR/public/PBR_Structures.fxh>
#include <Shaders/PBR/private/RenderPBR_Structures.fxh>
}
}  // namespace Diligent

/*
 * Diligent half of the heightmap terrain pass (see HeightmapTerrainRender.h
 * and plans/azgaar-terrain.md phase 6).
 *
 * Per READY tile: the CPU lattice (256^2 world-space corners,
 * HeightmapLattice) is repacked into (pos3, normal3) and uploaded as an
 * immutable VertexBuffer; one shared 255-segment lattice IndexBuffer backs
 * every tile. The look lives entirely in the pixel shader
 * (shaders/heightmap_terrain_ps.hlsl, the DiligentFX-PBR port of
 * terrain.mat); the vertex stage is a thin clip transform. The lighting
 * reads the same PBR frame/sun attribs and the same preintegrated GGX LUT
 * as the glTF PBR pass, so terrain + model read as one scene.
 *
 * The shaders are compiled at pass init via device->CreateShader
 * (HLSL + DiligentFXShaderSourceStreamFactory — the repo's runtime HLSL
 * precedent, the repo's runtime-HLSL shaders are compiled at pass init via
 * device->CreateShader); the
 * sources ship in pak_1 (CMake copy step).
 *
 * Uploads are budgeted (kUploadsPerFrame) and nearest-to-camera first, so
 * the visible ring fills before the window edge (the old engine's Vulkan
 * pass policy).
 */

namespace engine {
using namespace Diligent;
using engine::renderer::diligent::device;
using engine::renderer::diligent::context;
using engine::renderer::diligent::swapChain;

namespace {

constexpr u32 kUploadsPerFrame        = 3;
constexpr u32 kDeferredDestroyFrames  = 3;

// Default terrain textures (engine pak, PNG — the exact ETC1S decode of the
// .ktx2 originals, which this backend cannot read: imageLoadKtx is a stub
// since KTX-Software was unlinked, see scripts/unpack-terrain-ktx2.sh).
// Albedos are created sRGB so the hardware decode matches the CPU-composited look.
struct DefaultTexture {
    const char* path;
    const char* srvName;
    bool srgb;
};

const DefaultTexture kDefaultTextures[] = {
    {"images/terrain/grass_default/albedo.png", "g_GrassAlbedo", true},
    {"images/terrain/grass_default/normal.png", "g_GrassNormal", false},
    {"images/terrain/cliff_side_default/albedo.png", "g_CliffAlbedo", true},
    {"images/terrain/cliff_side_default/normal.png", "g_CliffNormal", false},
    {"images/terrain/snow_default/albedo.png", "g_SnowAlbedo", true},
    {"images/terrain/sand_default/albedo.png", "g_SandAlbedo", true},
};

// Per-tile GPU state (main thread only).
struct GpuTile {
    bool          inUse = false;
    i32           tileX = 0, tileZ = 0;
    u64           readyStamp = 0;
    IBuffer*      vbo = nullptr;
};

// GPU destruction is deferred a few frames: the context keeps strong
// references to the buffers bound on the last draw, so a freshly released
// VBO can only actually be freed once it is no longer referenced.
struct DeferredDestroy {
    IBuffer*      vbo = nullptr;
    u32           framesLeft = kDeferredDestroyFrames;
};

// ── Pass state ────────────────────────────────────────────────────────────

IPipelineState*        pipeline = nullptr;
IPipelineResourceSignature* prs = nullptr;
IShaderResourceBinding* srb = nullptr;
IShaderResourceVariable* prsVar(SHADER_TYPE stage, const char* name);
IShader*               vs = nullptr;
IShader*               ps = nullptr;
IBuffer*               latticeIbo = nullptr;
u32                    latticeIdxCount = 0;
IBuffer*               frameAttribsCB = nullptr;
ISampler*              tilingSampler = nullptr;
ISampler*              clampSampler = nullptr;
ISampler*              clampNearestSampler = nullptr;
ITexture*              fallbackTex = nullptr;   // 1x1 white (empty slots)
ITexture*              defaultTex[6] = {};      // kDefaultTextures order
ITexture*              biomeColorTex = nullptr;
ITexture*              climateTex = nullptr;
ITexture*              iblIrradiance = nullptr;
ITexture*              iblPrefiltered = nullptr;
ITextureView*          ggxLUT = nullptr;       // gltf LUT SRV (borrowed, ref held)
ITexture*              ggxFallbackLUT = nullptr;
bool                   passReady = false;
bool                   initFailed = false;

// The gltf PBR pass owns the ambient IBL cubes' input; rebuild the terrain's
// own constant cubes when it changes (it only changes at world load).
f32  lastAmbientColor[3] = {-1.0f, -1.0f, -1.0f};
f32  lastAmbientIntensity = -1.0f;
bool iblInitialized = false;

std::vector<GpuTile>         gpuTiles;
std::vector<DeferredDestroy> deferred;
HeightmapTerrain*            cachedHt = nullptr;

HeightmapTerrainLook look = {};
bool                 lookRegistered = false;
std::vector<u8>      lookBiomePixels;
std::vector<u8>      lookClimatePixels;
float                debugView = 0.0f;

// Per-frame cost tracking (120-frame warmup + 1000-frame hold, the old
// engine's acceptance pattern).
constexpr u32 kStatWarmupFrames = 120;
constexpr u32 kStatFrames       = 1000;
u64    statFrame   = 0;
double statSum     = 0.0;
u32    statCount   = 0;
double statAvgMs   = 0.0;

// ── Resource helpers ─────────────────────────────────────────────────────

void transitionToShaderResource(ITexture* tex) {
    StateTransitionDesc barrier{tex, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE,
            STATE_TRANSITION_FLAG_UPDATE_STATE};
    context->TransitionResourceStates(1, &barrier);
}

void transitionToShaderResource(ITexture* tex);

// Load a pak PNG through DiligentTools' TextureLoader (stb decode + mip-chain
// generation) and create an immutable texture. The TextureLoader copies the
// input, so the pak blob only needs to outlive this call.
ITexture* loadKtx2Texture(const char* path, bool srgb) {
    utils::String blob = utils::dataManagerRead(path);
    if (!blob.data || blob.size == 0) {
        utils::warn("heightmapTerrain: texture load failed: %s", path);
        utils::stringDestroy(&blob);
        return nullptr;
    }

    TextureLoadInfo info;
    info.Name        = path;
    info.IsSRGB      = srgb ? True : False;
    info.GenerateMips = True;
    RefCntAutoPtr<ITextureLoader> loader;
    CreateTextureLoaderFromMemory(blob.data, blob.size, true, info, &loader);
    utils::stringDestroy(&blob);
    if (!loader) {
        utils::warn("heightmapTerrain: texture loader failed: %s", path);
        return nullptr;
    }

    RefCntAutoPtr<ITexture> tex;
    loader->CreateTexture(device, &tex);
    if (!tex) {
        utils::warn("heightmapTerrain: texture creation failed: %s", path);
        return nullptr;
    }
    transitionToShaderResource(tex);
    // Keep one ref past this scope: the caller owns the raw pointer, and a
    // plain `return tex;` would drop the only reference in the
    // RefCntAutoPtr destructor (dangling pointer).
    tex->AddRef();
    return tex;
}

// Upload packed RGBA8 pixels (one mip level).
ITexture* createRgba8(const u8* pixels, u32 w, u32 h, TEXTURE_FORMAT fmt, const char* name) {
    TextureDesc desc;
    desc.Name        = name;
    desc.Type        = RESOURCE_DIM_TEX_2D;
    desc.Usage       = USAGE_IMMUTABLE;
    desc.BindFlags   = BIND_SHADER_RESOURCE;
    desc.Format      = fmt;
    desc.Width       = w;
    desc.Height      = h;
    desc.MipLevels   = 1;
    desc.ArraySize   = 1;

    TextureSubResData subres((const void*)pixels, (Uint64)w * 4u);
    TextureData data;
    data.pSubResources   = &subres;
    data.NumSubresources = 1;
    data.pContext        = context;

    RefCntAutoPtr<ITexture> tex;
    device->CreateTexture(desc, &data, &tex);
    if (!tex) return nullptr;
    transitionToShaderResource(tex);
    // keep one ref past this scope (see loadKtx2Texture)
    tex->AddRef();
    return tex;
}

// Forward declarations (resource helpers defined below).
void rebuildSharedSRB(void);
ITexture* makeGgxFallbackLUT(void);
// (Re)create the per-world look textures from the registered look pixels
// (no-ops to the fallback when no look is registered).
void createLookTextures(void);

// The engine's ambient (color * lux * 1.2 * 2^-15 / pi) baked into two
// 1x1x6 RGBA8 constant cubes — the same formula as gltfIblUpdateDiligent,
// so terrain and model share one ambient by construction.
void rebuildIblCubes(const f32 color[3], f32 intensity) {
    const float exposure = 1.2f * (float)std::exp2(-15.0);
    const float k = std::max(0.0f, intensity) * exposure * 0.318309886f;  // 1/pi
    float rgb[3] = {std::min(1.0f, color[0] * k), std::min(1.0f, color[1] * k),
            std::min(1.0f, color[2] * k)};

    u8 px[24];
    for (int face = 0; face < 6; face++) {
        for (int c = 0; c < 3; c++) {
            px[face * 4 + c] = (u8)(rgb[c] * 255.0f);
        }
        px[face * 4 + 3] = 255;
    }

    auto makeCube = [&](const char* name) -> ITexture* {
        TextureDesc desc;
        desc.Name        = name;
        desc.Type        = RESOURCE_DIM_TEX_CUBE;
        desc.Usage       = USAGE_IMMUTABLE;
        desc.BindFlags   = BIND_SHADER_RESOURCE;
        desc.Format      = TEX_FORMAT_RGBA8_UNORM;
        desc.Width       = 1;
        desc.Height      = 1;
        desc.MipLevels   = 1;
        desc.ArraySize   = 6;
        TextureSubResData subres[6];
        for (int face = 0; face < 6; face++) {
            subres[face] = TextureSubResData(px + face * 4, 4);
        }
        TextureData data;
        data.pSubResources   = subres;
        data.NumSubresources = 6;
        data.pContext        = context;
        RefCntAutoPtr<ITexture> tex;
        device->CreateTexture(desc, &data, &tex);
        if (!tex) return nullptr;
        transitionToShaderResource(tex);
        // keep one ref past this scope (see loadKtx2Texture)
        tex->AddRef();
        return tex;
    };

    ITexture* irr = makeCube("terrain IBL irradiance");
    ITexture* pf  = makeCube("terrain IBL prefiltered");
    if (!irr || !pf) {
        utils::warn("heightmapTerrain: IBL cube creation failed");
        return;
    }
    if (iblIrradiance) iblIrradiance->Release();
    if (iblPrefiltered) iblPrefiltered->Release();
    iblIrradiance  = irr;
    iblPrefiltered = pf;
    if (prs) {
        if (IShaderResourceVariable* v = prsVar(SHADER_TYPE_PIXEL, "g_IblIrradiance"))
            v->Set(irr->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE),
                    SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
        if (IShaderResourceVariable* v = prsVar(SHADER_TYPE_PIXEL, "g_IblPrefiltered"))
            v->Set(pf->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE),
                    SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    }
    rebuildSharedSRB();
}

// ── SRB (shared across all tiles: only the per-draw VBO varies) ───────────

// All pipeline resources are STATIC in the signature (bound once through
// the PRS); the per-tile VBO is NOT a shader resource — it is bound per
// draw call via SetVertexBuffers (slot 0), the shared lattice IBO via
// SetIndexBuffer. One SRB therefore serves the whole pass; it is recreated
// (cheap) whenever a static resource changes (IBL rebuild, look
// registration, GGX LUT swap).
PipelineResourceDesc gResources[] = {
        {SHADER_TYPE_VERTEX | SHADER_TYPE_PIXEL, "cbFrameAttribs", 1,
                SHADER_RESOURCE_TYPE_CONSTANT_BUFFER, SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "g_GrassAlbedo", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "g_GrassNormal", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "g_CliffAlbedo", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "g_CliffNormal", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "g_SnowAlbedo", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "g_SandAlbedo", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "g_BiomeColor", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "g_Climate", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "g_ClimateNearest", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "g_IblIrradiance", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "g_IblPrefiltered", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "g_PreintegratedGGX", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "g_TilingSampler", 1, SHADER_RESOURCE_TYPE_SAMPLER,
                SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "g_ClampSampler", 1, SHADER_RESOURCE_TYPE_SAMPLER,
                SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "g_ClampNearestSampler", 1, SHADER_RESOURCE_TYPE_SAMPLER,
                SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
};

void rebuildSharedSRB(void) {
    if (!prs) return;
    IShaderResourceBinding* newSrb = nullptr;
    prs->CreateShaderResourceBinding(&newSrb, true);  // init static resources
    if (!newSrb) {
        utils::warn("heightmapTerrain: SRB creation failed");
        return;
    }
    IShaderResourceBinding* old = srb;
    srb = newSrb;
    if (old) old->Release();
}

// Bind the currently-owned shared resources into the PRS' static variables
// (ALLOW_OVERWRITE: the look textures, IBL cubes and the GGX LUT change
// over the pass' lifetime).
// Null-safe PRS static-variable lookup (a missing variable is a hard
// init error; the pass must not crash on it, it must log and degrade).
IShaderResourceVariable* prsVar(SHADER_TYPE stage, const char* name) {
    if (!prs) return nullptr;
    IShaderResourceVariable* v = prs->GetStaticVariableByName(stage, name);
    if (!v) {
        utils::warn("heightmapTerrain: no static PRS variable '%s'", name);
    }
    return v;
}

void bindSharedStatics(void) {
    if (IShaderResourceVariable* v = prsVar(SHADER_TYPE_VERTEX, "cbFrameAttribs"))
        v->Set(frameAttribsCB, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);

    auto bindSRV = [&](const char* name, ITexture* tex) {
        if (!tex) return;  // the slot keeps its previous (fallback) binding
        IShaderResourceVariable* v = prsVar(SHADER_TYPE_PIXEL, name);
        if (v)
            v->Set(tex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE),
                    SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    };
    for (int i = 0; i < 6; i++) {
        bindSRV(kDefaultTextures[i].srvName, defaultTex[i] ? defaultTex[i] : fallbackTex);
    }
    bindSRV("g_BiomeColor", lookRegistered && biomeColorTex ? biomeColorTex : fallbackTex);
    bindSRV("g_Climate", lookRegistered && climateTex ? climateTex : fallbackTex);
    bindSRV("g_ClimateNearest", lookRegistered && climateTex ? climateTex : fallbackTex);
    bindSRV("g_IblIrradiance", iblIrradiance);
    bindSRV("g_IblPrefiltered", iblPrefiltered);
    if (ggxLUT) {
        if (IShaderResourceVariable* v = prsVar(SHADER_TYPE_PIXEL, "g_PreintegratedGGX"))
            v->Set(ggxLUT, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    } else {
        ITexture* fb = makeGgxFallbackLUT();
        if (fb)
            bindSRV("g_PreintegratedGGX", fb);
    }
    if (IShaderResourceVariable* v = prsVar(SHADER_TYPE_PIXEL, "g_TilingSampler"))
        v->Set(tilingSampler, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    if (IShaderResourceVariable* v = prsVar(SHADER_TYPE_PIXEL, "g_ClampSampler"))
        v->Set(clampSampler, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    if (IShaderResourceVariable* v = prsVar(SHADER_TYPE_PIXEL, "g_ClampNearestSampler"))
        v->Set(clampNearestSampler, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
}

// ── Pass lifecycle ───────────────────────────────────────────────────────

// Shared 255-segment lattice index buffer (one per engine, all tiles).
bool latticeIboEnsure(void) {
    latticeIdxCount = heightmapLatticeIndexCount();
    u32* idx = new u32[latticeIdxCount];
    heightmapLatticeBuildIndices(idx);
    BufferDesc desc;
    desc.Name        = "heightmap terrain lattice IBO";
    desc.Size        = (Uint64)latticeIdxCount * sizeof(u32);
    desc.BindFlags   = BIND_INDEX_BUFFER;
    desc.Usage       = USAGE_IMMUTABLE;
    BufferData data{(const void*)idx, (Uint64)latticeIdxCount * sizeof(u32), context};
    device->CreateBuffer(desc, &data, &latticeIbo);
    delete[] idx;
    if (!latticeIbo) {
        utils::warn("heightmapTerrain: lattice IBO creation failed");
        latticeIdxCount = 0;
        return false;
    }
    return true;
}

IShader* createHlslShader(const char* pakPath, const char* name, SHADER_TYPE type) {
    utils::String blob = utils::dataManagerRead(pakPath);
    if (!blob.data || blob.size == 0) {
        utils::warn("heightmapTerrain: shader source missing from pak: %s", pakPath);
        utils::stringDestroy(&blob);
        return nullptr;
    }
    ShaderCreateInfo ci;
    ci.Desc.Name        = name;
    ci.Desc.ShaderType  = type;
    ci.EntryPoint       = "main";
    ci.Source           = blob.data;
    ci.SourceLength     = blob.size;
    ci.SourceLanguage   = SHADER_SOURCE_LANGUAGE_HLSL;
    // The "Name.fxh" includes resolve against DiligentFX's embedded stream
    // factory (bare file names) — the same mechanism the prebuilt PBR
    // renderer uses for its own shaders.
    ci.pShaderSourceStreamFactory = &DiligentFXShaderSourceStreamFactory::GetInstance();

    RefCntAutoPtr<IShader> shader;
    RefCntAutoPtr<IDataBlob> output;
    device->CreateShader(ci, &shader, &output);
    utils::stringDestroy(&blob);
    if (!shader) {
        const char* msg = output ? (const char*)output->GetConstDataPtr() : "(no compiler output)";
        utils::warn("heightmapTerrain: shader compile failed %s: %s", name, msg);
        return nullptr;
    }

    // keep one ref past this scope (see loadKtx2Texture)
    shader->AddRef();
    return shader;
}

void initSamplers(void) {
    // FILTER_TYPE_ANISOTROPIC on min (D3D-style combined min+mip filter) with
    // MaxAnisotropy 16 — the world-tiling terrain textures repeat every few
    // metres and are seen from kilometres away (docs/lessons.md: no plain
    // LINEAR tiling samplers).
    SamplerDesc tiling;
    tiling.Name          = "terrain tiling sampler";
    tiling.MinFilter     = FILTER_TYPE_ANISOTROPIC;
    tiling.MagFilter     = FILTER_TYPE_LINEAR;
    tiling.MipFilter     = FILTER_TYPE_LINEAR;
    tiling.AddressU      = TEXTURE_ADDRESS_WRAP;
    tiling.AddressV      = TEXTURE_ADDRESS_WRAP;
    tiling.AddressW      = TEXTURE_ADDRESS_WRAP;
    tiling.MaxAnisotropy = 16;
    device->CreateSampler(tiling, &tilingSampler);

    SamplerDesc clamp;
    clamp.Name        = "terrain clamp sampler";
    clamp.MinFilter   = FILTER_TYPE_LINEAR;
    clamp.MagFilter   = FILTER_TYPE_LINEAR;
    clamp.MipFilter   = FILTER_TYPE_LINEAR;
    device->CreateSampler(clamp, &clampSampler);

    SamplerDesc clampNearest;
    clampNearest.Name        = "terrain clamp nearest sampler";
    clampNearest.MinFilter   = FILTER_TYPE_POINT;
    clampNearest.MagFilter   = FILTER_TYPE_POINT;
    clampNearest.MipFilter   = FILTER_TYPE_POINT;
    device->CreateSampler(clampNearest, &clampNearestSampler);

    if (!tilingSampler || !clampSampler || !clampNearestSampler) {
        utils::warn("heightmapTerrain: sampler creation failed");
    }
}

void initConstantBuffers(void) {
    // PBRFrameAttribs + one PBRLightAttribs (the sun), the gltf PBR pass'
    // "layout owned by the renderer" pairing.
    const size_t size = sizeof(HLSL::PBRFrameAttribs) + sizeof(HLSL::PBRLightAttribs);
    CreateUniformBuffer(device, (Uint64)size, "terrain PBR frame attribs", &frameAttribsCB);
    if (!frameAttribsCB) {
        utils::warn("heightmapTerrain: cbuffer creation failed");
    }
}

void createLookTextures(void) {
    if (biomeColorTex) { biomeColorTex->Release(); biomeColorTex = nullptr; }
    if (climateTex) { climateTex->Release(); climateTex = nullptr; }
    if (!lookRegistered) return;
    // sRGB: authored biome display colours.
    biomeColorTex = createRgba8(lookBiomePixels.data(), look.biomeColorW, look.biomeColorH,
            TEX_FORMAT_RGBA8_UNORM_SRGB, "terrain biome color");
    // R/G/B byte-encoded scalars are linear by design.
    climateTex = createRgba8(lookClimatePixels.data(), look.climateW, look.climateH,
            TEX_FORMAT_RGBA8_UNORM, "terrain climate");
}

// 2x2 RG16F LUT of 1.0s (half 0x3C00): used only while the glTF PBR pass
// has no preintegrated GGX LUT (its constructor precomputes one on init,
// so this window is short at best).
ITexture* makeGgxFallbackLUT(void) {
    if (ggxFallbackLUT) return ggxFallbackLUT;
    static const u8 rg16Ones[8] = {0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C};
    TextureDesc desc;
    desc.Name        = "terrain GGX LUT fallback";
    desc.Type        = RESOURCE_DIM_TEX_2D;
    desc.Usage       = USAGE_IMMUTABLE;
    desc.BindFlags   = BIND_SHADER_RESOURCE;
    desc.Format      = TEX_FORMAT_RG16_FLOAT;
    desc.Width       = 2;
    desc.Height      = 2;
    desc.MipLevels   = 1;
    desc.ArraySize   = 1;
    TextureSubResData subres((const void*)rg16Ones, 8);
    TextureData data;
    data.pSubResources   = &subres;
    data.NumSubresources = 1;
    data.pContext        = context;
    device->CreateTexture(desc, &data, &ggxFallbackLUT);
    if (ggxFallbackLUT) transitionToShaderResource(ggxFallbackLUT);
    return ggxFallbackLUT;
}

// Bind the GGX LUT SRV: the glTF PBR pass' precomputed LUT when available
// (lighting parity = literally shared state), the CPU fallback otherwise.
void syncGgxLUT(void) {
    ITextureView* lut = (ITextureView*)gltf::gltfDiligentPreintegratedGGX();
    if (lut) {
        if (lut != ggxLUT) {
            if (ggxLUT) ggxLUT->Release();
            ggxLUT = lut;
            if (IShaderResourceVariable* v = prsVar(SHADER_TYPE_PIXEL, "g_PreintegratedGGX"))
                v->Set(ggxLUT, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
            rebuildSharedSRB();
        } else {
            lut->Release();
        }
    } else if (ggxLUT) {
        ggxLUT->Release();
        ggxLUT = nullptr;
        ITexture* fb = makeGgxFallbackLUT();
        if (fb) {
            if (IShaderResourceVariable* v = prsVar(SHADER_TYPE_PIXEL, "g_PreintegratedGGX"))
                v->Set(fb->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE),
                        SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
            rebuildSharedSRB();
        }
    }
}

void initPass(void) {
    if (passReady || !device) return;
    if (initFailed) return;

    vs = createHlslShader("materials/heightmap_terrain_vs.hlsl", "heightmapTerrainVS",
            SHADER_TYPE_VERTEX);
    ps = createHlslShader("materials/heightmap_terrain_ps.hlsl", "heightmapTerrainPS",
            SHADER_TYPE_PIXEL);
    if (!vs || !ps) {
        initFailed = true;
        return;
    }

    initSamplers();
    initConstantBuffers();

    // Default terrain textures + fallbacks.
    for (int i = 0; i < 6; i++) {
        const DefaultTexture& dt = kDefaultTextures[i];
        defaultTex[i] = loadKtx2Texture(dt.path, dt.srgb);
        if (!defaultTex[i]) {
            utils::warn("heightmapTerrain: no default texture for %s (fallback white)", dt.srvName);
        }
    }

    const u8 white[4] = {255, 255, 255, 255};
    fallbackTex = createRgba8(white, 1, 1, TEX_FORMAT_RGBA8_UNORM, "terrain fallback");

    // Build the constant IBL cubes up front so the initial SRB is complete
    // (an SRB born with unassigned static variables logs a Diligent error
    // and the first frame draws with garbage IBL).
    const f32* initAmbColor = engine::renderer::diligent::diligentAmbientColor();
    f32 initAmbIntensity    = engine::renderer::diligent::diligentAmbientIntensity();
    rebuildIblCubes(initAmbColor, initAmbIntensity);
    lastAmbientColor[0]     = initAmbColor[0];
    lastAmbientColor[1]     = initAmbColor[1];
    lastAmbientColor[2]     = initAmbColor[2];
    lastAmbientIntensity   = initAmbIntensity;
    iblInitialized         = true;

    if (!latticeIboEnsure()) {
        initFailed = true;
        return;
    }

    // The game may have registered the look before the first frame — build
    // its textures now so the initial PRS binding is complete.
    createLookTextures();

    // Resource signature (all static) + shared SRB.
    PipelineResourceSignatureDesc prsDesc;
    prsDesc.Resources      = gResources;
    prsDesc.NumResources   = (Uint32)(sizeof(gResources) / sizeof(gResources[0]));
    prsDesc.BindingIndex   = 0;
    device->CreatePipelineResourceSignature(prsDesc, &prs);
    if (!prs) {
        utils::warn("heightmapTerrain: resource signature creation failed");
        initFailed = true;
        return;
    }
    bindSharedStatics();
    rebuildSharedSRB();
    if (!srb) {
        initFailed = true;
        return;
    }

    // PSO: swapchain color + D32 depth, one VBO (pos3+normal3) + shared IBO.
    // The Vulkan input layout path requires ATTRIBn-style layout elements
    // (DiligentFX convention; the VS input struct uses : ATTRIB0/: ATTRIB1).
    GraphicsPipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name      = "heightmapTerrain";
    psoCI.ppResourceSignatures = &prs;
    psoCI.ResourceSignaturesCount = 1;

    GraphicsPipelineDesc& gp = psoCI.GraphicsPipeline;
    gp.RasterizerDesc.FrontCounterClockwise = true;  // lattice winding is CCW from above
    gp.RasterizerDesc.CullMode              = CULL_MODE_BACK;
    gp.DepthStencilDesc.DepthEnable         = true;
    gp.DepthStencilDesc.DepthWriteEnable    = true;
    gp.DepthStencilDesc.DepthFunc           = COMPARISON_FUNC_LESS;
    gp.PrimitiveTopology                   = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    gp.NumRenderTargets                    = 1;
    gp.RTVFormats[0]                       = swapChain->GetDesc().ColorBufferFormat;
    gp.DSVFormat                           = swapChain->GetDesc().DepthBufferFormat;

    static const LayoutElement inputLayout[] = {
            // "ATTRIB" is the only semantic that works through the
            // HLSL->GLSL->SPIRV path on the Vulkan backend. Offsets/stride
            // are stated explicitly: the engine's auto-resolution left the
            // location-0 offset at the LAYOUT_ELEMENT_AUTO_OFFSET sentinel,
            // so the position attribute was fed garbage while the normal
            // (offset 12) rendered fine (round 5).
            {"ATTRIB", 0, 0, 3, VT_FLOAT32, False, 0u,  24u},  // float3 Position
            {"ATTRIB", 1, 0, 3, VT_FLOAT32, False, 12u, 24u}, // float3 Normal
    };
    gp.InputLayout.LayoutElements = inputLayout;
    gp.InputLayout.NumElements    = 2;

    psoCI.pVS = vs;
    psoCI.pPS = ps;
    device->CreateGraphicsPipelineState(psoCI, &pipeline);
    if (!pipeline) {
        utils::warn("heightmapTerrain: PSO creation failed");
        initFailed = true;
        return;
    }

    passReady = true;
    utils::info("heightmapTerrain: diligent pass initialized (shaders + PSO + shared IBO %u idx)",
            latticeIdxCount);
}

// ── Frame attribs / look cbuffers ─────────────────────────────────────────

// Fills the shared PBR frame attribs (camera + sun + tone map) the exact
// way GltfDiligent.cpp fillFrameAttribs does, plus the fields the terrain
// PS reads that the glTF fill leaves unset (zeroed first): the
// environment rotation, the single-mip prefiltered-cube level, the IBL
// scale and the loading-animation parameters.

void fillFrameAttribs(void) {
    const size_t cbSize = sizeof(HLSL::PBRFrameAttribs) + sizeof(HLSL::PBRLightAttribs);
    // Map the cbuffer into an EXCLUSIVE per-frame dynamic-heap region
    // (the glTF pass' MapHelper pattern) instead of UpdateBuffer:
    // UpdateBuffer on a virtual dynamic buffer blind-copies into
    // dynamic-heap offset 0 — a region the allocator never tracks — and
    // the RMLUI vbo's ring region parks at offset 0 in steady state, so
    // the two clobbered each other: the first UI line's vertices read
    // cbuffer floats (garbled), and the cbuffer could read back whatever
    // other pass had staged there (collapsed dark terrain). The map must
    // stay BEFORE the SRB commit in drawImpl: the descriptor's dynamic
    // offset is written at commit time and must point at THIS frame's
    // region — a stale offset is exactly the old "112-byte shift" symptom.
    MapHelper<HLSL::PBRFrameAttribs> frameMap(context, frameAttribsCB, MAP_WRITE,
            MAP_FLAG_DISCARD);
    u8* base = reinterpret_cast<u8*>(static_cast<HLSL::PBRFrameAttribs*>(frameMap));
    memset(base, 0, cbSize);

    const float4x4& view = engine::renderer::diligent::diligentFrameView();
    const float4x4& proj = engine::renderer::diligent::diligentFrameProj();
    const SwapChainDesc& scDesc = swapChain->GetDesc();

    HLSL::PBRFrameAttribs* frame = (HLSL::PBRFrameAttribs*)base;
    HLSL::CameraAttribs& camera = frame->Camera;
    // The runtime HLSL (glslang HLSL frontend) consumes cbuffer matrices
    // transposed relative to Diligent's row-major math: storing the plain
    // matrices made the shader apply rotation-without-translation plus the
    // translation as the projective row (terrain collapsed to a sliver that
    // hugged the world origin — see docs/lessons.md, the diligent-terrain
    // entry). The prebuilt DiligentFX SPIRV shaders are precompiled with the matching
    // convention; every runtime-compiled HLSL here must write transposed.
    camera.mView = view.Transpose();
    camera.mProj = proj.Transpose();
    camera.mViewProj = (view * proj).Transpose();
    camera.mViewInv = view.Inverse().Transpose();
    camera.mProjInv = proj.Inverse().Transpose();
    camera.mViewProjInv = (view * proj).Inverse().Transpose();
    camera.f4Position = float4(float3::MakeVector(camera.mViewInv[3]), 1.0f);
    camera.f4ViewportSize = float4{(float)scDesc.Width, (float)scDesc.Height,
            1.0f / (float)scDesc.Width, 1.0f / (float)scDesc.Height};
    camera.SetClipPlanes(engine::renderer::kCameraNear, engine::renderer::kCameraFar);
    camera.fHandness = view.Determinant() > 0 ? 1.0f : -1.0f;
    frame->PrevCamera = frame->Camera;

    // Per-world look params ride in f4ExtraData (application-specific; the
    // pixel stage reads them from here — a second cbuffer bound ambiguously,
    // see docs/lessons.md, the diligent-terrain entry).
    camera.f4ExtraData[0] = float4{look.mapMinX, look.mapMinZ, look.mapMaxX, look.mapMaxZ};
    camera.f4ExtraData[1] = float4{look.snowLoC, look.snowHiC, look.beachHeightM,
            look.climateEnabled ? 1.0f : 0.0f};
    camera.f4ExtraData[2] = float4{look.maxLandHeightM, debugView, 0.0f, 0.0f};
    // [3]/[4] = the world anchor split into f32 high + residual (the camera
    // eye, the render-space origin — docs/lessons.md, the 2026-09-04 f32
    // entry). The VS subtracts BOTH from the absolute position: the first
    // subtraction is exact (both operands in the same binade — Sterbenz) and
    // the residual (sub-mm) removes the f32(anchor) ULP step, so the ground
    // does not re-shimmer by 3.9 mm on every camera ULP crossing. The PS
    // uses the same split for the anchor-relative view direction.
    {
        f64 anchorF64[3];
        engine::renderer::diligent::diligentWorldAnchor(anchorF64);
        f32 ah[3], al[3];
        for (int i = 0; i < 3; i++) {
            ah[i] = (f32)anchorF64[i];
            al[i] = (f32)(anchorF64[i] - (f64)ah[i]);
        }
        camera.f4ExtraData[3] = float4{ah[0], ah[1], ah[2], 0.0f};
        camera.f4ExtraData[4] = float4{al[0], al[1], al[2], 0.0f};
    }

    HLSL::PBRRendererShaderParameters& renderer = frame->Renderer;
    renderer.OcclusionStrength = 1.0f;
    renderer.EmissionScale = 1.0f;
    renderer.AverageLogLum = 0.25f;
    renderer.MiddleGray = 0.18f;
    renderer.WhitePoint = 3.0f;
    renderer.PrefilteredCubeLastMip = 0.0f;  // constant env: one mip
    renderer.EnvironmentRotation = float2(1.0f, 0.0f);  // no env rotation
    renderer.IBLScale = float4{1.0f, 1.0f, 1.0f, 1.0f};
    renderer.HighlightColor = float4{1.0f, 0.0f, 0.0f, 0.0f};
    renderer.UnshadedColor = float4{0.5f, 0.5f, 0.5f, 1.0f};
    renderer.PointSize = 1.0f;
    renderer.MipBias = 0.0f;
    renderer.LightCount = 1;
    renderer.DebugView = 0;

    // Directional sun right after the frame attribs (same as the glTF fill).
    HLSL::PBRLightAttribs* light =
            reinterpret_cast<HLSL::PBRLightAttribs*>(base + sizeof(HLSL::PBRFrameAttribs));
    const f32* sunDir = engine::renderer::diligent::diligentSunDirection();
    const f32* sunColor = engine::renderer::diligent::diligentSunColor();
    f32 sunIntensity = engine::renderer::diligent::diligentSunIntensity();

    GLTF::Light sun;
    sun.Type = GLTF::Light::TYPE::DIRECTIONAL;
    sun.Color = float3{sunColor[0], sunColor[1], sunColor[2]};
    sun.Intensity = sunIntensity > 0.0f ? sunIntensity * (3.0f / 110000.0f) : 0.0f;
    float3 direction = normalize(float3{sunDir[0], sunDir[1], sunDir[2]});
    GLTF_PBR_Renderer::WritePBRLightShaderAttribs({&sun, nullptr, &direction, 1.0f}, light);

    // `frameMap` (MapHelper) unmaps on scope exit.
}

// ── Look ───────────────────────────────────────────────────────────────────

void registerLookImpl(const HeightmapTerrainLook* lookPtr) {
    lookBiomePixels.clear();
    lookClimatePixels.clear();

    if (lookPtr && lookPtr->biomeColorPixels && lookPtr->climatePixels &&
        lookPtr->biomeColorW && lookPtr->biomeColorH && lookPtr->climateW && lookPtr->climateH) {
        look = *lookPtr;
        lookBiomePixels.assign(lookPtr->biomeColorPixels,
                lookPtr->biomeColorPixels + (size_t)lookPtr->biomeColorW * (size_t)lookPtr->biomeColorH * 4u);
        lookClimatePixels.assign(lookPtr->climatePixels,
                lookPtr->climatePixels + (size_t)lookPtr->climateW * (size_t)lookPtr->climateH * 4u);
        lookRegistered = true;
    } else {
        look = HeightmapTerrainLook{};
        lookRegistered = false;
    }

    if (passReady) {
        // (Re)create the per-world look textures and rebind the PRS statics.
        createLookTextures();
        bindSharedStatics();
        rebuildSharedSRB();
        utils::info("heightmapTerrain: look %s (climate %s, snow [%.1f, %.1f] C, beach %.1f m, "
                "maxLand %.0f m, map %.0fx%.0f m)",
                lookRegistered ? "registered" : "cleared",
                look.climateEnabled ? "on" : "off", look.snowLoC, look.snowHiC, look.beachHeightM,
                look.maxLandHeightM,
                look.mapMaxX - look.mapMinX, look.mapMaxZ - look.mapMinZ);
    }
}

void releaseLookImpl(void) {
    registerLookImpl(nullptr);
}

void setDebugViewImpl(u32 mode) {
    float v = (float)mode;
    if (v == debugView) return;
    debugView = v;
}

// ── Tile cache ─────────────────────────────────────────────────────────────

void destroyAllTiles(void) {
    for (GpuTile& t : gpuTiles) {
        if (!t.inUse) continue;
        t.vbo->Release();
    }
    gpuTiles.clear();
    for (DeferredDestroy& d : deferred) {
        if (d.vbo) d.vbo->Release();
    }
    deferred.clear();
}

void destroyTile(GpuTile* e) {
    if (!e->inUse) return;
    // The context keeps its own reference until the next SetVertexBuffers
    // unbinds this buffer, so the GPU-side lifetime is already safe; the
    // deferral just keeps the release off the in-flight draw frame.
    deferred.push_back({.vbo = e->vbo, .framesLeft = kDeferredDestroyFrames});
    *e = GpuTile{};
}

bool gpuTileHasView(const GpuTile* e, const HeightmapTileView* v) {
    return e->inUse && e->tileX == v->tileX && e->tileZ == v->tileZ && e->readyStamp == v->readyStamp;
}

bool gpuTileMatchesAnyView(const GpuTile* e, const HeightmapTileView* views, u32 count) {
    for (u32 j = 0; j < count; j++) {
        if (gpuTileHasView(e, &views[j])) return true;
    }
    return false;
}

GpuTile* gpuTileAcquireFree(void) {
    for (GpuTile& t : gpuTiles) {
        if (!t.inUse) return &t;
    }
    return nullptr;
}

// Upload one tile: CPU lattice -> (pos3, normal3) VBO. The pixel shader
// rebuilds the tangent frame from the normal (no tangent attribute). The
// storage is per upload (heap): the next tile of this frame (budget > 1)
// must not clobber a previous upload before it is consumed.
bool uploadTile(GpuTile* e, const HeightmapTileView* v) {
    const u32 cornerCount = heightmapLatticeCornerCount();
    const size_t vboSize = (size_t)cornerCount * 6u * sizeof(float);

    static std::vector<HeightmapLatticeCorner> cornerScratch;
    cornerScratch.resize(cornerCount);
    heightmapLatticeBuildCorners(v->heights, v->originX, v->originZ, v->sizeMeters,
            cornerScratch.data());

    float* vboData = new float[cornerCount * 6u];
    for (u32 i = 0; i < cornerCount; i++) {
        const HeightmapLatticeCorner& c = cornerScratch[i];
        float* dst = &vboData[(size_t)i * 6u];
        dst[0] = c.pos[0];   dst[1] = c.pos[1];   dst[2] = c.pos[2];
        dst[3] = c.normal[0]; dst[4] = c.normal[1]; dst[5] = c.normal[2];
    }
    BufferDesc desc;
    desc.Name        = "heightmap terrain tile VBO";
    desc.Size        = (Uint64)vboSize;
    desc.BindFlags   = BIND_VERTEX_BUFFER;
    desc.Usage       = USAGE_IMMUTABLE;

    BufferData data{(const void*)vboData, (Uint64)vboSize, context};
    device->CreateBuffer(desc, &data, &e->vbo);
    delete[] vboData;
    if (!e->vbo) {
        utils::warn("heightmapTerrain: VBO creation failed tile(%d,%d)", v->tileX, v->tileZ);
        return false;
    }

    e->inUse      = true;
    e->tileX      = v->tileX;
    e->tileZ      = v->tileZ;
    e->readyStamp = v->readyStamp;
    return true;
}

// ── Frame entry: cache maintenance + budgeted uploads ─────────────────────

void updateImplWork(void) {
    if (!passReady) {
        initPass();
        if (!passReady) return;
    }

    // Tick deferred GPU destruction (must outlive in-flight draws).
    for (i32 i = (i32)deferred.size() - 1; i >= 0; i--) {
        if (deferred[i].framesLeft > 1) {
            deferred[i].framesLeft--;
            continue;
        }
        if (deferred[i].vbo) deferred[i].vbo->Release();
        deferred[(u32)i] = deferred.back();
        deferred.pop_back();
    }

    // Ambient IBL (rebuild the constant cubes when the scene ambient moves).
    const f32* ambColor = engine::renderer::diligent::diligentAmbientColor();
    f32 ambIntensity   = engine::renderer::diligent::diligentAmbientIntensity();
    if (!iblInitialized || ambIntensity != lastAmbientIntensity ||
        ambColor[0] != lastAmbientColor[0] || ambColor[1] != lastAmbientColor[1] ||
        ambColor[2] != lastAmbientColor[2]) {
        rebuildIblCubes(ambColor, ambIntensity);
        lastAmbientColor[0] = ambColor[0];
        lastAmbientColor[1] = ambColor[1];
        lastAmbientColor[2] = ambColor[2];
        lastAmbientIntensity = ambIntensity;
        iblInitialized = true;
    }
    syncGgxLUT();

    HeightmapTerrain* ht = heightmapTerrainGetActive();
    if (cachedHt && ht != cachedHt) {
        destroyAllTiles();
        cachedHt = nullptr;
    }
    if (!ht || !ht->initialized) return;
    cachedHt = ht;

    const u32 cap = ht->windowSize * ht->windowSize;
    if (gpuTiles.size() < cap) gpuTiles.resize(cap);

    std::vector<HeightmapTileView> views(cap);
    const u32 viewCount = heightmapTerrainSnapshotTiles(ht, views.data(), cap);
    if (viewCount == 0) return;

    // 1) Drop cache entries whose tile left the window or was regenerated.
    for (u32 i = 0; i < gpuTiles.size(); i++) {
        if (gpuTiles[i].inUse && !gpuTileMatchesAnyView(&gpuTiles[i], views.data(), viewCount)) {
            destroyTile(&gpuTiles[i]);
        }
    }

    // 2) Upload missing tiles, nearest to the camera first so the visible
    // ring fills before the distant window edge.
    float camPos[3] = {0.0f, 0.0f, 0.0f};
    float camFwd[3] = {0.0f, 0.0f, 0.0f};
    renderer::rendererCameraGet(camPos, camFwd);
    const i32 anchorTileX = heightmapWorldToTileCoord(ht, camPos[0]);
    const i32 anchorTileZ = heightmapWorldToTileCoord(ht, camPos[2]);

    // Stable insertion sort by (Manhattan ring, view order); n <= 25.
    for (u32 i = 1; i < viewCount; i++) {
        HeightmapTileView key = views[i];
        i32 kdx = key.tileX - anchorTileX;
        if (kdx < 0) kdx = -kdx;
        i32 kdz = key.tileZ - anchorTileZ;
        if (kdz < 0) kdz = -kdz;
        i32 kx = kdx + kdz;
        i32 j  = (i32)i - 1;
        for (; j >= 0; j--) {
            i32 dx = views[j].tileX - anchorTileX;
            if (dx < 0) dx = -dx;
            i32 dz = views[j].tileZ - anchorTileZ;
            if (dz < 0) dz = -dz;
            if (dx + dz <= kx) break;
            views[j + 1] = views[j];
        }
        views[j + 1] = key;
    }

    u32 budget = kUploadsPerFrame;
    for (u32 j = 0; j < viewCount && budget > 0; j++) {
        bool have = false;
        for (u32 i = 0; i < gpuTiles.size(); i++) {
            if (gpuTileHasView(&gpuTiles[i], &views[j])) {
                have = true;
                break;
            }
        }
        if (have) continue;

        GpuTile* e = gpuTileAcquireFree();
        if (!e) break;  // pool exhausted (shouldn't happen: cap = window^2)
        if (uploadTile(e, &views[j])) {
            budget--;
            utils::info("heightmapTerrain: uploaded tile(%d,%d) stamp=%llu", views[j].tileX,
                    views[j].tileZ, (unsigned long long)views[j].readyStamp);
        }
        // failed upload: retry next frame, do not consume budget
    }
}

// ── Draw: the pass' indexed draws over the current render targets ────────

void drawImpl(void) {
    if (!passReady || !pipeline) return;

    GpuTile* first = nullptr;
    for (GpuTile& t : gpuTiles) {
        if (t.inUse) { first = &t; break; }
    }
    if (!first) return;

    fillFrameAttribs();

    context->SetPipelineState(pipeline);
    context->CommitShaderResources(srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->SetIndexBuffer(latticeIbo, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    bool drew = false;
    for (GpuTile& t : gpuTiles) {
        if (!t.inUse) continue;
        IBuffer* vbo = t.vbo;
        context->SetVertexBuffers(0, 1, &vbo, nullptr,
                RESOURCE_STATE_TRANSITION_MODE_TRANSITION, SET_VERTEX_BUFFERS_FLAG_RESET);
        context->DrawIndexed(DrawIndexedAttribs{
                latticeIdxCount, VT_UINT32, DRAW_FLAG_NONE, 1, 0, 0, 0});
        drew = true;
    }
    if (drew) {
        // The UI pass LOADs over the world only when the world pass drew —
        // a terrain-only world (no glb) must not leave it on CLEAR.
        engine::renderer::diligent::setWorldDrew(true);
    }
}

void destroyImpl(void) {
    if (!device) {
        // The device is already gone (backend teardown order): drop only
        // CPU-side state.
        gpuTiles.clear();
        deferred.clear();
        cachedHt = nullptr;
        passReady = false;
        return;
    }

    destroyAllTiles();
    for (int i = 0; i < 6; i++) {
        if (defaultTex[i]) { defaultTex[i]->Release(); defaultTex[i] = nullptr; }
    }

    if (srb) { srb->Release(); srb = nullptr; }
    if (prs) { prs->Release(); prs = nullptr; }
    if (pipeline) { pipeline->Release(); pipeline = nullptr; }
    if (vs) { vs->Release(); vs = nullptr; }
    if (ps) { ps->Release(); ps = nullptr; }
    if (latticeIbo) { latticeIbo->Release(); latticeIbo = nullptr; }
    if (frameAttribsCB) { frameAttribsCB->Release(); frameAttribsCB = nullptr; }
    if (tilingSampler) { tilingSampler->Release(); tilingSampler = nullptr; }
    if (clampSampler) { clampSampler->Release(); clampSampler = nullptr; }
    if (clampNearestSampler) { clampNearestSampler->Release(); clampNearestSampler = nullptr; }
    if (biomeColorTex) { biomeColorTex->Release(); biomeColorTex = nullptr; }
    if (climateTex) { climateTex->Release(); climateTex = nullptr; }
    if (fallbackTex) { fallbackTex->Release(); fallbackTex = nullptr; }
    if (iblIrradiance) { iblIrradiance->Release(); iblIrradiance = nullptr; }
    if (iblPrefiltered) { iblPrefiltered->Release(); iblPrefiltered = nullptr; }
    if (ggxLUT) { ggxLUT->Release(); ggxLUT = nullptr; }
    if (ggxFallbackLUT) { ggxFallbackLUT->Release(); ggxFallbackLUT = nullptr; }

    lookBiomePixels.clear();
    lookClimatePixels.clear();
    lookRegistered = false;
    cachedHt = nullptr;
    iblInitialized = false;
    passReady = false;
    initFailed = false;
}
}  // namespace

// Public entry points (see HeightmapTerrainDiligent.h).

void heightmapTerrainDiligentInit(void) {
    initPass();
}

void heightmapTerrainDiligentUpdate(void) {
    double t0 = utils::elapsedBegin();
    updateImplWork();
    double ms = utils::elapsedEnd(t0);

    ++statFrame;
    if (statFrame > kStatWarmupFrames && statCount < kStatFrames) {
        statSum += ms;
        statCount++;
        statAvgMs = statSum / (double)statCount;
    }
}

void heightmapTerrainDiligentDraw(void) {
    drawImpl();
}

void heightmapTerrainDiligentRegisterLook(const HeightmapTerrainLook* lookPtr) {
    registerLookImpl(lookPtr);
}

void heightmapTerrainDiligentReleaseLook(void) {
    releaseLookImpl();
}

void heightmapTerrainDiligentSetDebugView(u32 mode) {
    setDebugViewImpl(mode);
}

void heightmapTerrainDiligentStats(HeightmapTerrainRenderStats* out) {
    if (!out) return;
    out->renderAvgMs = (statCount > 0) ? statAvgMs : 0.0;
    u32    tiles = 0;
    size_t bytes = 0;
    for (const GpuTile& t : gpuTiles) {
        if (!t.inUse) continue;
        tiles++;
        bytes += (size_t)heightmapLatticeCornerCount() * 6u * sizeof(float);
    }
    bytes += (size_t)latticeIdxCount * sizeof(u32);
    out->gpuTiles = tiles;
    out->gpuBytes = bytes;
}

void heightmapTerrainDiligentDestroy(void) {
    destroyImpl();
}
}  // namespace engine
