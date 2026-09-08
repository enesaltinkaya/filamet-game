#include "renderer/diligent/PropsRenderDiligent.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "ecs/system/heightmap/HeightmapTerrainRender.h"
#include "renderer/diligent/HeightmapTerrainDiligent.h"
#include "renderer/diligent/IblDiligent.h"
#include "ecs/system/player/Player.h"
#include "gltf/GltfInternal.h"
#include "logger/Logger.h"
#include "renderer/RenderBackend.h"
#include "renderer/Renderer.h"
#include "renderer/diligent/DiligentRenderer.h"
#include "renderer/diligent/ShadowDiligent.h"
#include "renderer/diligent/TaaDiligent.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "DiligentFXShaderSourceStreamFactory.hpp"
#include "Engine.h"
#include "Graphics/GraphicsEngine/interface/Buffer.h"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/PipelineResourceSignature.h"
#include "Graphics/GraphicsEngine/interface/PipelineState.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Sampler.h"
#include "Graphics/GraphicsEngine/interface/SwapChain.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"
#include "Graphics/GraphicsTools/interface/CommonlyUsedStates.h"
#include "Graphics/GraphicsTools/interface/GraphicsUtilities.h"
#include "Graphics/GraphicsTools/interface/MapHelper.hpp"

#include <GLTFLoader.hpp>
#include <GLTF_PBR_Renderer.hpp>
#include <TextureLoader.h>

namespace Diligent {
namespace HLSL {
#include <Shaders/Common/public/BasicStructures.fxh>
#include <Shaders/PBR/public/PBR_Structures.fxh>
#include <Shaders/PBR/private/RenderPBR_Structures.fxh>
}
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

/*
 * Diligent half of the Azgaar props pass (see PropsRenderDiligent.h for the
 * design; PropsRender.h for the backend-agnostic contract; the old engine's
 * game-001-cpp VulkanAzgaarPropsPass and the filament PropsRenderFilament
 * (git history) are the behaviour/look references).
 *
 * Per (tile, species, variant) range: one instanced draw of the shared
 * merged species mesh (VBO + IBO bound once per frame), instanced through a
 * per-tile RGBA32F texture (4 texels per instance, fetched with
 * SV_InstanceID). Per-range SRBs bind the tile's instance texture + the
 * variant's base texture. Tile applies are budgeted per frame (nearest to
 * the camera first); tiles that leave the active HeightmapTerrain window
 * are evicted; GPU releases are deferred a few frames (the context keeps
 * strong references on in-flight draws).
 */

namespace engine {
using namespace Diligent;
using engine::renderer::diligent::device;
using engine::renderer::diligent::context;
using engine::renderer::diligent::swapChain;
using engine::renderer::diligent::FrustumCullPlanes;
using engine::renderer::diligent::frustumCullAabbOutside;
using engine::renderer::diligent::frustumCullFromVP;

namespace {

constexpr u32 kInstanceTexWidth   = 2048;  // texels per row; multiple of 4
constexpr u32 kTexelsPerInstance  = 4;
constexpr u32 kChunkInstances     = 32767; // per-draw instance clamp (contract)
constexpr u32 kMaxTileApplies     = 4;
constexpr size_t kApplyByteBudget = 8u << 20;  // ~2 dense tiles per frame

constexpr float kTileM          = 2048.0f;
constexpr float kReach          = 64.0f;
constexpr float kYMin           = -100.0f;
constexpr float kYMax           = 4000.0f;
constexpr float kShadowSwayPad  = 4.0f;
constexpr u32 kDeferredDestroyFrames = 3;

// Private per-instance flag: the variant has a base texture (the PS's
// textured path). Bits 0..2 are the game's AZGAAR_PROPS_FLAG_* contract.
constexpr u32 kFlagTextured = 8u;

// ── CPU state (main thread only; no locks — Game.cpp drives everything) ───

std::vector<float> meshVertsCpu;   // 13 floats per vertex, upload verbatim
std::vector<u32>   meshIdxCpu;
bool               meshDirty = false;

std::vector<PropsRenderMeshVariant> variantsCpu;

struct PendingTile {
    i32  tileX = 0, tileZ = 0;
    u64  readyStamp = 0;
    u32  buildSeq = 0;
    std::vector<PropsRenderInstance> instances;
    std::vector<PropsRenderRange> ranges;
};
std::vector<PendingTile> pendingTiles;

bool  enabled = false;
float windDir[2]     = {0.70710678f, 0.70710678f};
float windSpeed      = 0.60f;   // rad/s; the old engine's 0.10 read as static
float windStrength   = 0.35f;   // m of tip drift at full sway weight
double windTimeSec   = 0.0;
double windPhaseRad  = 0.0;  // integrated sway phase (rad, gust-speed aware)
float  gustStrength  = 0.35f;
u64   lastNanos      = 0;

// Player reaction push (the old engine's azgaar_props.vert player term): the
// grass parts around the player and swishes while they move. f4ExtraData[2]
// = xyz feet position (m), w horizontal speed (m/s); a far-away position is
// the "no player / push off" state (the falloff reads as 0).
float playerPush[4] = {1e9f, 0.0f, 1e9f, 0.0f};
static bool playerPushEnabled(void) {
    static int v = -1;
    if (v < 0) {
        const char* env = getenv("ENGINE_PROPS_PLAYER_PUSH");
        v = !(env && *env && atof(env) <= 0.0f);
    }
    return v;
}

bool passReady = false;
bool initFailed = false;

IPipelineState*             pipeline = nullptr;
IPipelineResourceSignature* prs = nullptr;
IShader* vs = nullptr;
IShader* ps = nullptr;

// CSM shadow depth pipeline (ShadowDiligent.cpp owns the atlas + cascade
// distribution; rebuilt with the lit pipeline when the shadow settings
// generation moves — the sampling macros are compile-time). The per-tile
// shadow SRBs bind the tile's instance texture; g_BaseTex is set per range
// draw (the cutout PS alpha-discards the card silhouettes); cbShadowPass
// (the shared per-cascade cbuffer) and g_BaseSampler are statics of the
// shadow PRS.
IShader* shadowVs = nullptr;
IShader* shadowPs = nullptr;
IPipelineState* shadowPipeline = nullptr;
IPipelineResourceSignature* shadowPrs = nullptr;
u32  cachedShadowGen = 0;
IBuffer* meshVbo = nullptr;
IBuffer* meshIbo = nullptr;
u32      meshIdxCount = 0;
IBuffer* frameAttribsCB = nullptr;
ISampler* clampSampler = nullptr;
ISampler* baseSampler = nullptr;     // variant base textures (linear mip wrap)
ITexture* whiteTex = nullptr;        // 1x1 white (procedural species)
ITexture* iblIrradiance = nullptr;
ITexture* iblPrefiltered = nullptr;
ITextureView* ggxLUT = nullptr;      // gltf PBR pass' LUT (borrowed, ref held)
ITexture* ggxFallbackLUT = nullptr;
f32  lastAmbientColor[3] = {-1.0f, -1.0f, -1.0f};
f32  lastAmbientIntensity = -1.0f;

struct GpuRange {
    IShaderResourceBinding* srb = nullptr;
    u32 start = 0;          // first instance (FirstInstanceLocation)
    u32 count = 0;
    u32 indexOffset = 0;
    u32 indexCount = 0;
    float aabbMin[3] = {0.0f, 0.0f, 0.0f};
    float aabbMax[3] = {0.0f, 0.0f, 0.0f};
    ITexture* baseTex = nullptr;  // borrowed (variant cache / whiteTex); for SRB rebuilds
};

struct GpuTile {
    bool inUse = false;
    i32  tileX = 0, tileZ = 0;
    u64  readyStamp = 0;
    u32  instanceCount = 0;
    ITexture* instanceTex = nullptr;
    IShaderResourceBinding* shadowSrb = nullptr;  // binds instanceTex for the depth pass
    std::vector<GpuRange> ranges;
};
std::vector<GpuTile> gpuTiles;

struct DeferredGpu {
    ITexture* tex = nullptr;
    u32 framesLeft = kDeferredDestroyFrames;
};
std::vector<DeferredGpu> deferred;

// Variant base textures, loaded lazily on first use (device required).
std::unordered_map<std::string, ITexture*> variantTextures;

constexpr u32 kStatWarmupFrames = 120;
constexpr u32 kStatFrames       = 1000;
u64    statFrame  = 0;
double statSum    = 0.0;
u32    statCount  = 0;
double statAvgMs  = 0.0;
double applySum   = 0.0;
u32    applyCount = 0;
double applyAvgMs = 0.0;
u32    statDrawsThisFrame = 0;
u32    statInstancesThisFrame = 0;

void transitionToShaderResource(ITexture* tex) {
    StateTransitionDesc barrier{tex, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE,
            STATE_TRANSITION_FLAG_UPDATE_STATE};
    context->TransitionResourceStates(1, &barrier);
}

ITexture* loadPngTexture(const char* path, bool srgb) {
    utils::String blob = utils::dataManagerRead(path);
    if (!blob.data || blob.size == 0) {
        utils::warn("props: texture load failed: %s", path);
        utils::stringDestroy(&blob);
        return nullptr;
    }
    TextureLoadInfo info;
    info.Name         = path;
    info.IsSRGB       = srgb ? True : False;
    info.GenerateMips = True;
    RefCntAutoPtr<ITextureLoader> loader;
    CreateTextureLoaderFromMemory(blob.data, blob.size, true, info, &loader);
    utils::stringDestroy(&blob);
    if (!loader) {
        utils::warn("props: texture loader failed: %s", path);
        return nullptr;
    }
    RefCntAutoPtr<ITexture> tex;
    loader->CreateTexture(device, &tex);
    if (!tex) {
        utils::warn("props: texture creation failed: %s", path);
        return nullptr;
    }
    transitionToShaderResource(tex);
    tex->AddRef();  // keep one ref past the auto ptr scope (caller owns)
    return tex;
}

ITexture* variantTexture(const char* path) {
    if (!path || !path[0]) return nullptr;
    auto it = variantTextures.find(path);
    if (it != variantTextures.end()) return it->second;
    ITexture* tex = loadPngTexture(path, true);
    variantTextures.emplace(path, tex);  // failures stay cached as null
    return tex;
}

// The engine's ambient baked into 1x1x6 RGBA8 constant cubes — the terrain
// pass' rebuildIblCubes, duplicated so the passes stay self-contained.
void rebuildIblCubes(const f32 color[3], f32 intensity) {
    const float exposure = 1.2f * (float)std::exp2(-15.0);
    const float k = std::max(0.0f, intensity) * exposure * 0.318309886f;  // 1/pi
    float rgb[3] = {std::min(1.0f, color[0] * k), std::min(1.0f, color[1] * k),
            std::min(1.0f, color[2] * k)};

    u8 px[24];
    for (int face = 0; face < 6; face++) {
        for (int c = 0; c < 3; c++) px[face * 4 + c] = (u8)(rgb[c] * 255.0f);
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
        for (int face = 0; face < 6; face++) subres[face] = TextureSubResData(px + face * 4, 4);
        TextureData data;
        data.pSubResources   = subres;
        data.NumSubresources = 6;
        data.pContext        = context;
        RefCntAutoPtr<ITexture> tex;
        device->CreateTexture(desc, &data, &tex);
        if (!tex) return nullptr;
        transitionToShaderResource(tex);
        tex->AddRef();
        return tex;
    };

    ITexture* irr = makeCube("props IBL irradiance");
    ITexture* pf  = makeCube("props IBL prefiltered");
    if (!irr || !pf) {
        utils::warn("props: IBL cube creation failed");
        return;
    }
    if (iblIrradiance) iblIrradiance->Release();
    if (iblPrefiltered) iblPrefiltered->Release();
    iblIrradiance  = irr;
    iblPrefiltered = pf;
}

ITexture* makeGgxFallbackLUT(void) {
    if (ggxFallbackLUT) return ggxFallbackLUT;
    static const u8 rg16Ones[16] = {0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C,
            0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C};
    TextureDesc desc;
    desc.Name        = "props GGX LUT fallback";
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

IShader* createHlslShader(const char* pakPath, const char* name, SHADER_TYPE type,
        const ShaderMacroHelper* macros = nullptr) {
    utils::String blob = utils::dataManagerRead(pakPath);
    if (!blob.data || blob.size == 0) {
        utils::warn("props: shader source missing from pak: %s", pakPath);
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
    if (macros) ci.Macros = *macros;
    ci.pShaderSourceStreamFactory = &DiligentFXShaderSourceStreamFactory::GetInstance();

    RefCntAutoPtr<IShader> shader;
    RefCntAutoPtr<IDataBlob> output;
    device->CreateShader(ci, &shader, &output);
    utils::stringDestroy(&blob);
    if (!shader) {
        const char* msg = output ? (const char*)output->GetConstDataPtr() : "(no compiler output)";
        utils::warn("props: shader compile failed %s: %s", name, msg);
        return nullptr;
    }
    shader->AddRef();
    return shader;
}

void bindSharedStatics(void) {
    if (!prs) return;
    if (IShaderResourceVariable* v = prs->GetStaticVariableByName(SHADER_TYPE_VERTEX, "cbFrameAttribs"))
        v->Set(frameAttribsCB, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    if (IShaderResourceVariable* v = prs->GetStaticVariableByName(SHADER_TYPE_PIXEL, "cbFrameAttribs"))
        v->Set(frameAttribsCB, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    if (IShaderResourceVariable* v = prs->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_IblIrradiance"))
        v->Set((renderer::diligent::iblDiligentReady() ? renderer::diligent::iblDiligentIrradianceCube() : iblIrradiance)->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE),
                SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    if (IShaderResourceVariable* v = prs->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_IblPrefiltered"))
        v->Set((renderer::diligent::iblDiligentReady() ? renderer::diligent::iblDiligentPrefilteredCube() : iblPrefiltered)->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE),
                SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    if (IShaderResourceVariable* v = prs->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_PreintegratedGGX")) {
        if (ggxLUT) {
            v->Set(ggxLUT, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
        } else if (ITexture* fb = makeGgxFallbackLUT()) {
            v->Set(fb->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE),
                    SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
        }
    }
    if (IShaderResourceVariable* v = prs->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_ClampSampler"))
        v->Set(clampSampler, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    if (IShaderResourceVariable* v = prs->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_BaseSampler"))
        v->Set(baseSampler, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);

    // CSM shadow resources: the texture is MUTABLE (bound in every range
    // SRB, see applyTile/rebuildTileSrbs); the sampler is an IMMUTABLE
    // sampler of the signature. Nothing to bind here.
}

// ── pipeline builders (lit + CSM shadow depth) ─────────────────────────────
// Rebuilt whenever the shadow settings generation moves (mode/quality
// change): the sampling macros (ENABLE_CSM_SHADOWS/SHADOW_MODE/...) are
// compile-time, and the lit PRS must match the active mode's shader
// resources. A rebuild also recreates the per-range lit SRBs and the
// per-tile shadow SRBs (rebuildTileSrbs).

void litMacros(ShaderMacroHelper& m) {
    const bool active = engine::renderer::diligent::shadowDiligentActive();
    m.AddShaderMacro("ENABLE_CSM_SHADOWS", active ? 1 : 0);
    if (active) {
        m.AddShaderMacro("SHADOW_MODE", engine::renderer::diligent::shadowDiligentMode());
        m.AddShaderMacro("PCF_FILTER_SIZE", engine::renderer::diligent::shadowDiligentPcfFilterSize());
        m.AddShaderMacro("FILTER_ACROSS_CASCADES", 1);
        m.AddShaderMacro("BEST_CASCADE_SEARCH", 1);
    }
}

bool buildLitPipeline(void) {
    ShaderMacroHelper macros;
    litMacros(macros);

    vs = createHlslShader("materials/props_vs.hlsl", "propsVS", SHADER_TYPE_VERTEX, &macros);
    ps = createHlslShader("materials/props_ps.hlsl", "propsPS", SHADER_TYPE_PIXEL, &macros);
    if (!vs || !ps) return false;

    std::vector<PipelineResourceDesc> resources = {
            {SHADER_TYPE_VERTEX | SHADER_TYPE_PIXEL, "cbFrameAttribs", 1,
                    SHADER_RESOURCE_TYPE_CONSTANT_BUFFER, SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_VERTEX, "g_InstanceTex", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
            {SHADER_TYPE_PIXEL, "g_BaseTex", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
            {SHADER_TYPE_PIXEL, "g_IblIrradiance", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_IblPrefiltered", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_PreintegratedGGX", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_ClampSampler", 1, SHADER_RESOURCE_TYPE_SAMPLER,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_BaseSampler", 1, SHADER_RESOURCE_TYPE_SAMPLER,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    std::vector<ImmutableSamplerDesc> immutableSamplers;
    if (engine::renderer::diligent::shadowDiligentActive()) {
        const bool pcf = engine::renderer::diligent::shadowDiligentMode() == 1;
        if (pcf) {
            resources.push_back({SHADER_TYPE_PIXEL, "g_tex2DShadowMap", 1,
                    SHADER_RESOURCE_TYPE_TEXTURE_SRV, SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE});
            // Immutable sampler (the DiligentFX PBR_Renderer shadow pattern).
            immutableSamplers.push_back({SHADER_TYPE_PIXEL, "g_tex2DShadowMap_sampler",
                    Diligent::Sam_ComparisonLinearClamp});
        } else {
            resources.push_back({SHADER_TYPE_PIXEL, "g_tex2DFilterableShadowMap", 1,
                    SHADER_RESOURCE_TYPE_TEXTURE_SRV, SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE});
            immutableSamplers.push_back({SHADER_TYPE_PIXEL, "g_tex2DFilterableShadowMap_sampler",
                    Diligent::Sam_LinearClamp});
        }
    }

    PipelineResourceSignatureDesc prsDesc;
    prsDesc.Resources      = resources.data();
    prsDesc.NumResources   = (Uint32)resources.size();
    prsDesc.ImmutableSamplers    = immutableSamplers.data();
    prsDesc.NumImmutableSamplers = (Uint32)immutableSamplers.size();
    prsDesc.BindingIndex   = 0;
    device->CreatePipelineResourceSignature(prsDesc, &prs);
    if (!prs) {
        utils::warn("props: resource signature creation failed");
        return false;
    }
    bindSharedStatics();

    {
        GraphicsPipelineStateCreateInfo psoCI;
        psoCI.PSODesc.Name             = "azgaarProps";
        psoCI.ppResourceSignatures     = &prs;
        psoCI.ResourceSignaturesCount  = 1;

        GraphicsPipelineDesc& gp = psoCI.GraphicsPipeline;
        // Low-poly props: no back-face culling (the old pass' noCull —
        // avoids back-face pops on the faceted canopies and lets thin
        // vegetation cards read from both sides).
        gp.RasterizerDesc.CullMode              = CULL_MODE_NONE;
        gp.RasterizerDesc.FrontCounterClockwise = true;
        gp.DepthStencilDesc.DepthEnable         = true;
        gp.DepthStencilDesc.DepthWriteEnable    = true;
        // LESS_EQUAL: props draw after the terrain on the same depth buffer,
        // and trunk bases sit exactly ON the physics-grid surface (equal
        // depth must pass, or every contact ring z-fights off).
        gp.DepthStencilDesc.DepthFunc           = COMPARISON_FUNC_LESS_EQUAL;
        gp.PrimitiveTopology                    = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        gp.NumRenderTargets                     = 2;
        gp.RTVFormats[0]                        = TEX_FORMAT_RGBA16_FLOAT;  // linear scene color (TAA input)
        gp.RTVFormats[1]                        = TEX_FORMAT_RG16_FLOAT;    // motion vectors (NDC deltas)
        gp.DSVFormat                            = swapChain->GetDesc().DepthBufferFormat;

        // AzgaarPropVertex layout, 52 B stride (ATTRIB semantics are the
        // only style this backend's input layout accepts; offsets explicit —
        // the auto-resolution left garbage at location 0, see lessons).
        static const LayoutElement inputLayout[] = {
                {"ATTRIB", 0, 0, 3, VT_FLOAT32, False, 0u,  52u},  // float3 Position
                {"ATTRIB", 1, 0, 4, VT_FLOAT32, False, 12u, 52u}, // float4 Normal (w=0)
                {"ATTRIB", 2, 0, 2, VT_FLOAT32, False, 28u, 52u}, // float2 UV
                {"ATTRIB", 3, 0, 4, VT_FLOAT32, False, 36u, 52u}, // float4 PartColor
        };
        gp.InputLayout.LayoutElements = inputLayout;
        gp.InputLayout.NumElements    = 4;

        psoCI.pVS = vs;
        psoCI.pPS = ps;
        device->CreateGraphicsPipelineState(psoCI, &pipeline);
        if (!pipeline) {
            utils::warn("props: PSO creation failed");
            return false;
        }
    }
    return true;
}

bool buildShadowPipeline(void) {
    // The shadow module's shared cbuffer may not exist yet (this pass lazy-
    // inits before the first shadowDiligentUpdateFrame): skip building — the
    // generation bump on the module's first update triggers the rebuild with
    // a valid binding.
    if (engine::renderer::diligent::shadowDiligentShadowPassCB() == nullptr) return true;

    ShaderMacroHelper dm;
    // Defined-ness IS the switch (the shader's #if tests value 1): a macro
    // added with value 0 is still "defined" and would flip the VS into the
    // debug branch every normal frame — never add it unless the env is set.
    if (getenv("ENGINE_PROPS_SHADOW_ANCHOR") != nullptr)
        dm.AddShaderMacro("PROPS_SHADOW_DEBUG_BARE", 1);
#ifdef PROPS_SHADOW_PRS_NOTEX
    dm.AddShaderMacro("PROPS_SHADOW_PRS_NOTEX", 1);
#endif
    shadowVs = createHlslShader("materials/props_shadow_vs.hlsl", "propsShadowVS",
            SHADER_TYPE_VERTEX, &dm);
    if (!shadowVs) return false;
    shadowPs = createHlslShader("materials/props_shadow_ps.hlsl", "propsShadowPS",
            SHADER_TYPE_PIXEL, nullptr);
    if (!shadowPs) return false;

    PipelineResourceSignatureDesc prsDesc;
    PipelineResourceDesc resources[4] = {
            {SHADER_TYPE_VERTEX, "cbShadowPass", 1,
                    SHADER_RESOURCE_TYPE_CONSTANT_BUFFER, SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {},
            {SHADER_TYPE_PIXEL, "g_BaseTex", 1,
                    SHADER_RESOURCE_TYPE_TEXTURE_SRV, SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
            {SHADER_TYPE_PIXEL, "g_BaseSampler", 1,
                    SHADER_RESOURCE_TYPE_SAMPLER, SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    Uint32 numResources = 3;
    if (getenv("ENGINE_PROPS_SHADOW_ANCHOR") == nullptr) {
        resources[1] = {SHADER_TYPE_VERTEX, "g_InstanceTex", 1,
                SHADER_RESOURCE_TYPE_TEXTURE_SRV, SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE};
        numResources = 4;
    }
    prsDesc.Resources      = resources;
    prsDesc.NumResources   = numResources;
    prsDesc.BindingIndex   = 0;
    device->CreatePipelineResourceSignature(prsDesc, &shadowPrs);
    if (!shadowPrs) {
        utils::warn("props: shadow PRS creation failed");
        return false;
    }
    if (IShaderResourceVariable* v = shadowPrs->GetStaticVariableByName(SHADER_TYPE_VERTEX, "cbShadowPass"))
        v->Set(engine::renderer::diligent::shadowDiligentShadowPassCB());
    if (IShaderResourceVariable* v = shadowPrs->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_BaseSampler"))
        v->Set(baseSampler);

    GraphicsPipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name             = "azgaarPropsShadow";
    psoCI.ppResourceSignatures    = &shadowPrs;
    psoCI.ResourceSignaturesCount = 1;

    GraphicsPipelineDesc& gp = psoCI.GraphicsPipeline;
    // Depth-only, nothing culled (the light view's winding flips with the
    // sun azimuth; thin cards must cast from both faces anyway).
    gp.RasterizerDesc.CullMode              = CULL_MODE_NONE;
    gp.RasterizerDesc.FrontCounterClockwise = true;
    float casterSlopeBias = 0.0f, casterDepthBias = 0.0f, casterBiasClamp = 0.0f;
    engine::renderer::diligent::shadowDiligentCasterBias(casterSlopeBias, casterDepthBias, casterBiasClamp);
    gp.RasterizerDesc.SlopeScaledDepthBias = casterSlopeBias;
    gp.RasterizerDesc.DepthBias            = (Int32)casterDepthBias;
    gp.RasterizerDesc.DepthBiasClamp       = casterBiasClamp;
    gp.DepthStencilDesc.DepthEnable      = true;
    gp.DepthStencilDesc.DepthWriteEnable = true;
    gp.DepthStencilDesc.DepthFunc        = COMPARISON_FUNC_LESS;
    gp.PrimitiveTopology                 = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    gp.NumRenderTargets                  = 0;
    gp.DSVFormat                         = TEX_FORMAT_D32_FLOAT;

    // The props mesh VBO is 52 B (float3 pos / float4 norm / float2 uv /
    // float4 partColor) — the same layout as the lit PSO. A 24 B lattice
    // stride here decodes every vertex past index 0 out of misaligned
    // floats: 4 M shadow "instances" drew into empty cascades (the readback
    // shows all-far depth) because the garbage positions land off-map.
    // The cross-test env draws the TERRAIN LATTICE through this PSO, which
    // keeps the 24 B layout.
    static const LayoutElement inputLayoutProps[] = {
            {"ATTRIB", 0, 0, 3, VT_FLOAT32, False, 0u,  52u},  // float3 Position
            {"ATTRIB", 1, 0, 4, VT_FLOAT32, False, 12u, 52u}, // float4 Normal
            {"ATTRIB", 2, 0, 2, VT_FLOAT32, False, 28u, 52u}, // float2 UV
            {"ATTRIB", 3, 0, 4, VT_FLOAT32, False, 36u, 52u}, // float4 PartColor
    };
    static const LayoutElement inputLayoutLattice[] = {
            {"ATTRIB", 0, 0, 3, VT_FLOAT32, False, 0u,  24u},
            {"ATTRIB", 1, 0, 3, VT_FLOAT32, False, 12u, 24u},
    };
    const bool latticeTest = getenv("ENGINE_PROPS_SHADOW_ANCHOR") != nullptr;
    gp.InputLayout.LayoutElements =
            latticeTest ? inputLayoutLattice : inputLayoutProps;
    gp.InputLayout.NumElements = latticeTest ? 2 : 4;

    psoCI.pVS = shadowVs;
    psoCI.pPS = shadowPs;
    device->CreateGraphicsPipelineState(psoCI, &shadowPipeline);
    if (!shadowPipeline) {
        utils::warn("props: shadow PSO creation failed");
        return false;
    }
    return true;
}

void destroyLitPipeline(void) {
    if (prs) { prs->Release(); prs = nullptr; }
    if (pipeline) { pipeline->Release(); pipeline = nullptr; }
    if (vs) { vs->Release(); vs = nullptr; }
    if (ps) { ps->Release(); ps = nullptr; }
}

void destroyShadowPipeline(void) {
    if (shadowPrs) { shadowPrs->Release(); shadowPrs = nullptr; }
    if (shadowPipeline) { shadowPipeline->Release(); shadowPipeline = nullptr; }
    if (shadowVs) { shadowVs->Release(); shadowVs = nullptr; }
    if (shadowPs) { shadowPs->Release(); shadowPs = nullptr; }
}

// Recreate every range lit-SRB and every tile shadow-SRB from the current
// signatures (the stored instance/base texture references stay valid).
void rebuildTileSrbs(void) {
    for (GpuTile& t : gpuTiles) {
        if (!t.inUse) continue;
        if (t.shadowSrb) { t.shadowSrb->Release(); t.shadowSrb = nullptr; }
        if (shadowPrs && t.instanceTex) {
            RefCntAutoPtr<IShaderResourceBinding> srb;
            shadowPrs->CreateShaderResourceBinding(&srb, true);
            if (srb) {
                if (IShaderResourceVariable* var =
                        srb->GetVariableByName(SHADER_TYPE_VERTEX, "g_InstanceTex"))
                    var->Set(t.instanceTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
                t.shadowSrb = srb.Detach();
            }
        }
        for (GpuRange& r : t.ranges) {
            if (r.srb) { r.srb->Release(); r.srb = nullptr; }
            if (!r.baseTex) continue;
            RefCntAutoPtr<IShaderResourceBinding> srb;
            prs->CreateShaderResourceBinding(&srb, true);
            if (!srb) continue;
            if (IShaderResourceVariable* var =
                    srb->GetVariableByName(SHADER_TYPE_VERTEX, "g_InstanceTex"))
                var->Set(t.instanceTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
            if (IShaderResourceVariable* var =
                    srb->GetVariableByName(SHADER_TYPE_PIXEL, "g_BaseTex"))
                var->Set(r.baseTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
            // CSM shadow map (MUTABLE; the view carries the comparison sampler).
            if (engine::renderer::diligent::shadowDiligentActive()) {
                const bool pcf = engine::renderer::diligent::shadowDiligentMode() == 1;
                const char* texName = pcf ? "g_tex2DShadowMap" : "g_tex2DFilterableShadowMap";
                if (ITextureView* srv = engine::renderer::diligent::shadowDiligentShadowSRV()) {
                    if (IShaderResourceVariable* var = srb->GetVariableByName(SHADER_TYPE_PIXEL, texName))
                        var->Set(srv);
                }
            }
            r.srb = srb.Detach();
        }
    }
}

void checkShadowGeneration(void) {
    const u32 gen = engine::renderer::diligent::shadowDiligentGeneration();
    if (gen == cachedShadowGen) return;
    cachedShadowGen = gen;
    if (!passReady) return;

    destroyLitPipeline();
    destroyShadowPipeline();
    if (!buildLitPipeline() || !buildShadowPipeline()) {
        utils::warn("props: shadow-generation pipeline rebuild failed");
        initFailed = true;
        passReady = false;
        // Release the half-built lit state so the caller's in-flight draw
        // (it checks passReady BEFORE calling here) hits the !pipeline guard
        // instead of drawing the NEW lit PSO against the STALE per-range SRBs
        // (VUID 00358/08600: 10-descriptor PRS vs 8-descriptor set). The
        // per-range SRBs keep the old prs alive via refcount.
        destroyLitPipeline();
        destroyShadowPipeline();
        return;
    }
    // The tiles survive (instance textures are signature-independent); only
    // their SRBs reference the destroyed signatures — recreate them.
    rebuildTileSrbs();
    utils::info("props: pipelines rebuilt (shadow generation %u)", gen);
}

bool initPass(void) {
    if (passReady || !device) return passReady;
    if (initFailed) return false;

    {
        SamplerDesc desc;
        desc.Name      = "props clamp sampler";
        desc.MinFilter = FILTER_TYPE_LINEAR;
        desc.MagFilter = FILTER_TYPE_LINEAR;
        desc.MipFilter = FILTER_TYPE_LINEAR;
        device->CreateSampler(desc, &clampSampler);

        SamplerDesc base;
        base.Name          = "props base sampler";
        base.MinFilter     = FILTER_TYPE_ANISOTROPIC;
        base.MagFilter     = FILTER_TYPE_LINEAR;
        base.MipFilter     = FILTER_TYPE_LINEAR;
        base.AddressU      = TEXTURE_ADDRESS_WRAP;
        base.AddressV      = TEXTURE_ADDRESS_WRAP;
        base.MaxAnisotropy = 8;
        device->CreateSampler(base, &baseSampler);
        if (!clampSampler || !baseSampler) {
            utils::warn("props: sampler creation failed");
            initFailed = true;
            return false;
        }
    }

    {
        const size_t size = sizeof(HLSL::PBRFrameAttribs) + sizeof(HLSL::PBRLightAttribs) +
                sizeof(HLSL::LightAttribs) + 4 * sizeof(float);
        CreateUniformBuffer(device, (Uint64)size, "props PBR frame attribs", &frameAttribsCB);
        if (!frameAttribsCB) {
            utils::warn("props: cbuffer creation failed");
            initFailed = true;
            return false;
        }
    }

    rebuildIblCubes(engine::renderer::diligent::diligentAmbientColor(),
            engine::renderer::diligent::diligentAmbientIntensity());
    lastAmbientColor[0] = engine::renderer::diligent::diligentAmbientColor()[0];
    lastAmbientColor[1] = engine::renderer::diligent::diligentAmbientColor()[1];
    lastAmbientColor[2] = engine::renderer::diligent::diligentAmbientColor()[2];
    lastAmbientIntensity = engine::renderer::diligent::diligentAmbientIntensity();

    {
        const u8 white[4] = {255, 255, 255, 255};
        TextureDesc desc;
        desc.Name        = "props white fallback";
        desc.Type        = RESOURCE_DIM_TEX_2D;
        desc.Usage       = USAGE_IMMUTABLE;
        desc.BindFlags   = BIND_SHADER_RESOURCE;
        desc.Format      = TEX_FORMAT_RGBA8_UNORM;
        desc.Width       = 1;
        desc.Height      = 1;
        desc.MipLevels   = 1;
        desc.ArraySize   = 1;
        TextureSubResData subres((const void*)white, 4);
        TextureData data;
        data.pSubResources   = &subres;
        data.NumSubresources = 1;
        data.pContext        = context;
        device->CreateTexture(desc, &data, &whiteTex);
        if (whiteTex) transitionToShaderResource(whiteTex);
        if (!whiteTex) {
            utils::warn("props: fallback texture creation failed");
            initFailed = true;
            return false;
        }
    }

    // Resource signature + PSOs: statics are shared; the per-draw resources
    // (the tile's instance texture + the variant's base texture) are MUTABLE
    // and bound through per-range SRBs. Built in buildLitPipeline /
    // buildShadowPipeline so a shadow-settings change can rebuild them (the
    // sampling macros are compile-time and the PRS resource list must match
    // the active mode's shader resources).
    if (!buildLitPipeline() || !buildShadowPipeline()) {
        initFailed = true;
        return false;
    }
    cachedShadowGen = engine::renderer::diligent::shadowDiligentGeneration();

    passReady = true;
    utils::info("props: diligent pass initialized (shaders + PSO)");
    return true;
}

void uploadMesh(void) {
    if (!meshDirty || !device) return;
    meshDirty = false;
    if (meshVbo) { meshVbo->Release(); meshVbo = nullptr; }
    if (meshIbo) { meshIbo->Release(); meshIbo = nullptr; }
    meshIdxCount = 0;
    if (meshVertsCpu.empty() || meshIdxCpu.empty()) return;

    const size_t vboSize = meshVertsCpu.size() * sizeof(float);
    BufferDesc vboDesc;
    vboDesc.Name      = "props merged mesh VBO";
    vboDesc.Size      = (Uint64)vboSize;
    vboDesc.BindFlags = BIND_VERTEX_BUFFER;
    vboDesc.Usage     = USAGE_IMMUTABLE;
    BufferData vboData{meshVertsCpu.data(), (Uint64)vboSize, context};
    device->CreateBuffer(vboDesc, &vboData, &meshVbo);

    const size_t iboSize = meshIdxCpu.size() * sizeof(u32);
    BufferDesc iboDesc;
    iboDesc.Name      = "props merged mesh IBO";
    iboDesc.Size      = (Uint64)iboSize;
    iboDesc.BindFlags = BIND_INDEX_BUFFER;
    iboDesc.Usage     = USAGE_IMMUTABLE;
    BufferData iboData{meshIdxCpu.data(), (Uint64)iboSize, context};
    device->CreateBuffer(iboDesc, &iboData, &meshIbo);

    if (!meshVbo || !meshIbo) {
        utils::warn("props: merged mesh upload failed (%zu verts / %zu idx)",
                meshVertsCpu.size() / 13u, meshIdxCpu.size());
        if (meshVbo) { meshVbo->Release(); meshVbo = nullptr; }
        if (meshIbo) { meshIbo->Release(); meshIbo = nullptr; }
        return;
    }
    meshIdxCount = (u32)meshIdxCpu.size();
    utils::info("props: merged mesh uploaded (%u verts / %u idx)",
            (u32)(meshVertsCpu.size() / 13u), meshIdxCount);
}

const PropsRenderMeshVariant* findVariant(u32 species, u32 variant) {
    for (const PropsRenderMeshVariant& v : variantsCpu) {
        if (v.species == species && v.variant == variant) return &v;
    }
    return nullptr;
}

// Pack one tile's instances into RGBA32F texels (props_vs.hlsl documents the
// layout) and create the per-range SRBs. Runs on the main thread during the
// budgeted apply.
bool applyTile(PendingTile& p) {
    const u32 count = (u32)p.instances.size();
    const u32 texelCount = count * kTexelsPerInstance;
    const u32 rows = (texelCount + kInstanceTexWidth - 1) / kInstanceTexWidth;

    std::vector<float> texels((size_t)rows * kInstanceTexWidth * 4u, 0.0f);
    for (const PropsRenderRange& r : p.ranges) {
        const PropsRenderMeshVariant* v = findVariant(r.species, r.variant);
        const u32 end = std::min(r.start + r.count, count);
        for (u32 i = r.start; i < end; i++) {
            const PropsRenderInstance& inst = p.instances[i];
            float* dst = &texels[(size_t)i * kTexelsPerInstance * 4u];
            dst[0]  = inst.pos[0]; dst[1]  = inst.pos[1]; dst[2]  = inst.pos[2];
            dst[3]  = inst.yaw;
            dst[4]  = inst.scale;
            dst[5]  = inst.color[0]; dst[6]  = inst.color[1]; dst[7]  = inst.color[2];
            dst[8]  = inst.phase;
            dst[9]  = v ? v->boundsMin[1] : 0.0f;
            dst[10] = v ? v->boundsMax[1] : 1.0f;
            dst[11] = v ? v->swayFactor : 0.0f;
            u32 flags = v ? v->flags : 0u;
            if (v && v->texturePath && v->texturePath[0] && variantTexture(v->texturePath))
                flags |= kFlagTextured;
            // uint bits stored in a float texel (the VS reads asuint)
            float flagBits = 0.0f;
            static_assert(sizeof(flagBits) == sizeof(flags), "bit-cast sizes");
            memcpy(&flagBits, &flags, sizeof(flagBits));
            dst[12] = flagBits;
            // dst[13..15] pad
        }
    }

    TextureDesc desc;
    desc.Name        = "props instance texture";
    desc.Type        = RESOURCE_DIM_TEX_2D;
    desc.Usage       = USAGE_IMMUTABLE;
    desc.BindFlags   = BIND_SHADER_RESOURCE;
    desc.Format      = TEX_FORMAT_RGBA32_FLOAT;
    desc.Width       = kInstanceTexWidth;
    desc.Height      = rows;
    desc.MipLevels   = 1;
    desc.ArraySize   = 1;
    TextureSubResData subres(texels.data(), (Uint64)kInstanceTexWidth * 4u * sizeof(float));
    TextureData data;
    data.pSubResources   = &subres;
    data.NumSubresources = 1;
    data.pContext        = context;

    ITexture* tex = nullptr;
    device->CreateTexture(desc, &data, &tex);
    if (!tex) {
        utils::warn("props: instance texture creation failed tile(%d,%d)", p.tileX, p.tileZ);
        return false;
    }
    transitionToShaderResource(tex);

    GpuTile gt;
    gt.inUse         = true;
    gt.tileX         = p.tileX;
    gt.tileZ         = p.tileZ;
    gt.readyStamp    = p.readyStamp;
    gt.instanceCount = count;
    gt.instanceTex   = tex;
    gt.ranges.reserve(p.ranges.size());

    ITextureView* instView = tex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);

    // Per-tile shadow SRB (binds the instance texture for the depth pass;
    // cbShadowPass is a static of the shadow PRS, bound at its creation).
    if (shadowPrs) {
        RefCntAutoPtr<IShaderResourceBinding> sSrb;
        shadowPrs->CreateShaderResourceBinding(&sSrb, true);
        if (sSrb) {
            if (IShaderResourceVariable* var =
                    sSrb->GetVariableByName(SHADER_TYPE_VERTEX, "g_InstanceTex"))
                var->Set(instView);
            gt.shadowSrb = sSrb.Detach();
        }
    }

    for (const PropsRenderRange& r : p.ranges) {
        const PropsRenderMeshVariant* v = findVariant(r.species, r.variant);
        if (!v || v->indexCount == 0) continue;

        ITexture* baseTex = whiteTex;
        if (v->texturePath && v->texturePath[0]) {
            if (ITexture* t = variantTexture(v->texturePath)) baseTex = t;
        }

        RefCntAutoPtr<IShaderResourceBinding> srb;
        prs->CreateShaderResourceBinding(&srb, true);
        if (!srb) continue;
        if (IShaderResourceVariable* var =
                srb->GetVariableByName(SHADER_TYPE_VERTEX, "g_InstanceTex"))
            var->Set(instView);
        if (IShaderResourceVariable* var =
                srb->GetVariableByName(SHADER_TYPE_PIXEL, "g_BaseTex"))
            var->Set(baseTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        // CSM shadow map (MUTABLE; the view carries the comparison sampler).
        if (engine::renderer::diligent::shadowDiligentActive()) {
            const bool pcf = engine::renderer::diligent::shadowDiligentMode() == 1;
            const char* texName = pcf ? "g_tex2DShadowMap" : "g_tex2DFilterableShadowMap";
            if (ITextureView* srv = engine::renderer::diligent::shadowDiligentShadowSRV()) {
                if (IShaderResourceVariable* var = srb->GetVariableByName(SHADER_TYPE_PIXEL, texName))
                    var->Set(srv);
            }
        }

        GpuRange gr;
        gr.srb         = srb.Detach();
        gr.start       = r.start;
        gr.count       = r.count;
        gr.indexOffset = v->indexOffset;
        gr.indexCount  = v->indexCount;
        gr.baseTex     = baseTex;
        memcpy(gr.aabbMin, r.aabbMin, sizeof(gr.aabbMin));
        memcpy(gr.aabbMax, r.aabbMax, sizeof(gr.aabbMax));
        gt.ranges.push_back(gr);
    }

    gpuTiles.push_back(std::move(gt));
    return true;
}

void extractCameraFrustumPlanes(FrustumCullPlanes& out) {
    const float4x4 vp = engine::renderer::diligent::diligentFrameView() *
            engine::renderer::diligent::diligentFrameProj();
    frustumCullFromVP(vp, device && device->GetDeviceInfo().NDC.MinZ < -0.5f, out);
}

bool rangeAabbValid(const GpuRange& r) {
    for (int c = 0; c < 3; c++) {
        if (r.aabbMin[c] != 0.0f || r.aabbMax[c] != 0.0f) return true;
    }
    return false;  // all-zero = unavailable (contract) -> always visible
}

void fillFrameAttribs(void) {
    const size_t cbSize = sizeof(HLSL::PBRFrameAttribs) + sizeof(HLSL::PBRLightAttribs) +
            sizeof(HLSL::LightAttribs) + 4 * sizeof(float);
    // Exclusive per-frame dynamic-heap region via MapBuffer (the glTF
    // pass' MapHelper pattern) — NOT UpdateBuffer: that path blind-writes
    // dynamic-heap offset 0 on virtual dynamic buffers, which collides
    // with the RMLUI vbo's ring region parked at offset 0 (garbled UI
    // line 1; cbuffer could read back another pass' staged bytes). The
    // map must stay before the SRB commit in drawImpl (the descriptor's
    // dynamic offset is written at commit time). See docs/lessons.md,
    // the 2026-09-05 dynamic-heap offset-0 entry.
    MapHelper<HLSL::PBRFrameAttribs> frameMap(context, frameAttribsCB, MAP_WRITE,
            MAP_FLAG_DISCARD);
    u8* base = reinterpret_cast<u8*>(static_cast<HLSL::PBRFrameAttribs*>(frameMap));
    memset(base, 0, cbSize);

    const float4x4& view = engine::renderer::diligent::diligentFrameView();
    const float4x4& proj = engine::renderer::diligent::diligentFrameProj();
    const SwapChainDesc& scDesc = swapChain->GetDesc();

    HLSL::PBRFrameAttribs* frame = (HLSL::PBRFrameAttribs*)base;
    HLSL::CameraAttribs& camera = frame->Camera;
    // Runtime glslang HLSL consumes cbuffer matrices TRANSPOSED
    // (docs/lessons.md, the 2026-09-05 entry — the terrain pass' convention).
    camera.mView = view.Transpose();
    camera.mProj = proj.Transpose();
    camera.mViewProj = (view * proj).Transpose();
    camera.mViewInv = view.Inverse().Transpose();
    camera.mProjInv = proj.Inverse().Transpose();
    camera.mViewProjInv = (view * proj).Inverse().Transpose();
    camera.f4Position = float4(float3::MakeVector(camera.mViewInv[3]), 1.0f);
    // The MV's pixel→NDC divisor must be the buffer the world actually
    // rendered into (the offscreen chain at renderScale): with the swapchain
    // size the motion vectors come out scaled by renderScale and TAA's depth
    // rejection drops the history every frame (shimmer, renderScale != 1).
    u32 vpW = 0, vpH = 0;
    engine::renderer::diligent::taaTargetSize(&vpW, &vpH);
    if (vpW == 0 || vpH == 0) {
        vpW = scDesc.Width;
        vpH = scDesc.Height;
    }
    camera.f4ViewportSize = float4{(float)vpW, (float)vpH,
            1.0f / (float)vpW, 1.0f / (float)vpH};
    camera.SetClipPlanes(engine::renderer::kCameraNear, engine::renderer::kCameraFar);
    camera.fHandness = view.Determinant() > 0 ? 1.0f : -1.0f;
    // TAA: this frame's sub-pixel jitter + the previous frame's camera for
    // the VS' prev-clip position (the wind-swayed position is reused for
    // the previous frame — the per-frame sway delta is sub-pixel, so the
    // camera term dominates the motion vector). PrevCamera TRANSPOSED like
    // the current camera above (the terrain pass' glslang convention).
    camera.f2Jitter = float2{engine::renderer::diligent::taaCurrentJitterX(),
            engine::renderer::diligent::taaCurrentJitterY()};
    {
        const auto* prev = static_cast<const HLSL::CameraAttribs*>(
                engine::renderer::diligent::taaPrevCameraAttribs());
        frame->PrevCamera.mViewProj = prev->mViewProj.Transpose();
        // Prev-camera anchor correction (taaPrevAnchorDelta): fold the
        // per-frame camera translation in — the VS reuses the current
        // anchor's rel for PrevClipPos. Post-multiply on the transposed
        // upload (column-consumed), like the terrain pass (TaaDiligent.h).
        f32 dEye[3];
        engine::renderer::diligent::taaPrevAnchorDelta(dEye);
        float4x4 t = float4x4::Identity();
        t._14 = dEye[0];
        t._24 = dEye[1];
        t._34 = dEye[2];
        frame->PrevCamera.mViewProj = frame->PrevCamera.mViewProj * t;
        frame->PrevCamera.f2Jitter = prev->f2Jitter;
    }

    // f4ExtraData[0] = wind (dir.xy, speed, gust-modulated strength);
    // [1].x = integrated sway phase (rad); [2] = player push (feet.xyz,
    // horizontal speed); [3]/[4] = the split world anchor (f32 high + sub-mm
    // residual).
    camera.f4ExtraData[0] = float4{windDir[0], windDir[1], windSpeed, gustStrength};
    camera.f4ExtraData[1] = float4{(float)windPhaseRad, 0.0f, 0.0f, 0.0f};
    camera.f4ExtraData[2] = float4{playerPush[0], playerPush[1], playerPush[2],
                                   playerPush[3]};
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
    renderer.PrefilteredCubeLastMip = renderer::diligent::iblDiligentReady() ? renderer::diligent::iblDiligentPrefilteredLastMip() : 0.0f;
    renderer.EnvironmentRotation = float2(1.0f, 0.0f);
    // The 3x lift compensates the constant-cube placeholder; the real env
    // cubes carry physical radiance, so scale 1 once IBL is precomputed.
    renderer.IBLScale = renderer::diligent::iblDiligentReady()
            ? float4{1.0f, 1.0f, 1.0f, 1.0f}
            : float4{3.0f, 3.0f, 3.0f, 1.0f};
    renderer.HighlightColor = float4{1.0f, 0.0f, 0.0f, 0.0f};
    renderer.UnshadedColor = float4{0.5f, 0.5f, 0.5f, 1.0f};
    renderer.PointSize = 1.0f;
    renderer.MipBias = 0.0f;
    renderer.LightCount = 1;
    renderer.DebugView = 0;

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

    // The DiligentFX LightAttribs (CSM) — filled by shadowDiligentUpdateFrame
    // earlier this frame (matrices already transposed); copied verbatim.
    if (const void* src = engine::renderer::diligent::shadowDiligentLightAttribs()) {
        memcpy(base + sizeof(HLSL::PBRFrameAttribs) + sizeof(HLSL::PBRLightAttribs),
                src, sizeof(HLSL::LightAttribs));
    }

    // Shadow receiver fade (f4ShadowFade): see the terrain pass' fill — the
    // PS fades the shadow to lit by .x (view-space m) starting at .y, so the
    // effective cutoff is the tier's distance, not the cascade box's reach.
    const float tierDist = engine::renderer::diligent::shadowDiligentTierDistance();
    float* fade = (float*)(base + sizeof(HLSL::PBRFrameAttribs) + sizeof(HLSL::PBRLightAttribs) +
            sizeof(HLSL::LightAttribs));
    fade[0] = tierDist;
    fade[1] = 0.75f * tierDist;

    // `frameMap` (MapHelper) unmaps on scope exit.
}

void syncEnvironment(void) {
    const f32* ambColor = engine::renderer::diligent::diligentAmbientColor();
    f32 ambIntensity = engine::renderer::diligent::diligentAmbientIntensity();
    if (ambIntensity != lastAmbientIntensity ||
        ambColor[0] != lastAmbientColor[0] || ambColor[1] != lastAmbientColor[1] ||
        ambColor[2] != lastAmbientColor[2]) {
        rebuildIblCubes(ambColor, ambIntensity);
        lastAmbientColor[0] = ambColor[0];
        lastAmbientColor[1] = ambColor[1];
        lastAmbientColor[2] = ambColor[2];
        lastAmbientIntensity = ambIntensity;
        bindSharedStatics();
    }
    ITextureView* lut = (ITextureView*)gltf::gltfDiligentPreintegratedGGX();
    if (lut) {
        if (lut != ggxLUT) {
            if (ggxLUT) ggxLUT->Release();
            ggxLUT = lut;
            bindSharedStatics();
        } else {
            lut->Release();
        }
    } else if (ggxLUT) {
        ggxLUT->Release();
        ggxLUT = nullptr;
        bindSharedStatics();
    }
}

void destroyTileAt(u32 index) {
    GpuTile& t = gpuTiles[index];
    // SRBs release immediately (the context holds its own strong references
    // until the next commit); the instance texture defers a few frames like
    // the terrain pass' VBOs.
    if (t.shadowSrb) { t.shadowSrb->Release(); t.shadowSrb = nullptr; }
    if (t.instanceTex) deferred.push_back({t.instanceTex, kDeferredDestroyFrames});
    for (GpuRange& r : t.ranges) {
        if (r.srb) r.srb->Release();
    }
    gpuTiles[index] = gpuTiles.back();
    gpuTiles.pop_back();
}

void destroyAllTiles(void) {
    for (u32 i = (u32)gpuTiles.size(); i > 0; i--) {
        GpuTile& t = gpuTiles[i - 1];
        if (t.shadowSrb) t.shadowSrb->Release();
        if (t.instanceTex) t.instanceTex->Release();
        for (GpuRange& r : t.ranges) {
            if (r.srb) r.srb->Release();
        }
    }
    gpuTiles.clear();
}

void tickDeferred(void) {
    for (i32 i = (i32)deferred.size() - 1; i >= 0; i--) {
        if (deferred[i].framesLeft > 1) {
            deferred[i].framesLeft--;
            continue;
        }
        if (deferred[i].tex) deferred[i].tex->Release();
        deferred[i] = deferred.back();
        deferred.pop_back();
    }
}

void applyPendingTiles(void) {
    if (pendingTiles.empty()) return;
    if (meshVbo == nullptr || meshIbo == nullptr) return;

    float camPos[3] = {0.0f, 0.0f, 0.0f};
    float camFwd[3] = {0.0f, 0.0f, 0.0f};
    renderer::rendererCameraGet(camPos, camFwd);
    std::stable_sort(pendingTiles.begin(), pendingTiles.end(),
            [&](const PendingTile& a, const PendingTile& b) {
                const float tileM = 2048.0f;
                float ax = (a.tileX + 0.5f) * tileM - camPos[0];
                float az = (a.tileZ + 0.5f) * tileM - camPos[2];
                float bx = (b.tileX + 0.5f) * tileM - camPos[0];
                float bz = (b.tileZ + 0.5f) * tileM - camPos[2];
                return ax * ax + az * az < bx * bx + bz * bz;
            });

    const double t0 = utils::elapsedBegin();
    u32 applied = 0;
    size_t bytes = 0;
    for (u32 i = 0; i < pendingTiles.size() && applied < kMaxTileApplies &&
            bytes < kApplyByteBudget;) {
        PendingTile& p = pendingTiles[i];
        // A same-tile entry further back is newer — skip stale duplicates.
        bool superseded = false;
        for (u32 j = i + 1; j < pendingTiles.size(); j++) {
            if (pendingTiles[j].tileX == p.tileX && pendingTiles[j].tileZ == p.tileZ) {
                superseded = true;
                break;
            }
        }
        if (superseded) {
            pendingTiles.erase(pendingTiles.begin() + i);
            continue;
        }
        for (i32 k = (i32)gpuTiles.size() - 1; k >= 0; k--) {
            if (gpuTiles[k].tileX == p.tileX && gpuTiles[k].tileZ == p.tileZ) {
                destroyTileAt((u32)k);
            }
        }
        if (p.instances.empty()) {
            pendingTiles.erase(pendingTiles.begin() + i);
            continue;
        }
        if (!applyTile(p)) break;  // creation failure: retry next frame
        bytes += p.instances.size() * kTexelsPerInstance * 16u;
        applied++;
        pendingTiles.erase(pendingTiles.begin() + i);
    }
    if (applied > 0) {
        double ms = utils::elapsedEnd(t0);
        applySum += ms;
        applyCount++;
        applyAvgMs = applySum / (double)applyCount;
    }
}

void evictOutsideWindow(void) {
    HeightmapTerrain* ht = heightmapTerrainGetActive();
    if (!ht || !ht->initialized) return;
    const u32 cap = ht->windowSize * ht->windowSize;
    std::vector<HeightmapTileView> views(cap);
    const u32 n = heightmapTerrainSnapshotTiles(ht, views.data(), cap);
    if (n == 0) return;

    for (i32 i = (i32)gpuTiles.size() - 1; i >= 0; i--) {
        bool resident = false;
        for (u32 j = 0; j < n; j++) {
            if (views[j].tileX == gpuTiles[i].tileX && views[j].tileZ == gpuTiles[i].tileZ) {
                resident = true;
                break;
            }
        }
        if (!resident) destroyTileAt((u32)i);
    }
}

}

void propsRenderDiligentSetMesh(const float* verts, u32 vertCount, const u32* idx, u32 idxCount) {
    meshVertsCpu.assign(verts, verts + (size_t)vertCount * 13u);
    meshIdxCpu.assign(idx, idx + idxCount);
    meshDirty = true;
    if (passReady) uploadMesh();
}

void propsRenderDiligentSetVariants(const PropsRenderMeshVariant* variants, u32 variantCount) {
    variantsCpu.assign(variants, variants + variantCount);
}

void propsRenderDiligentSetTile(i32 tileX, i32 tileZ, u64 readyStamp,
        const PropsRenderInstance* instances, u32 instanceCount,
        const PropsRenderRange* ranges, u32 rangeCount) {
    PendingTile p;
    p.tileX = tileX;
    p.tileZ = tileZ;
    p.readyStamp = readyStamp;
    if (instances && instanceCount > 0) {
        p.instances.assign(instances, instances + instanceCount);
    }
    if (ranges && rangeCount > 0) {
        p.ranges.assign(ranges, ranges + rangeCount);
    }
    pendingTiles.push_back(std::move(p));
}

void propsRenderDiligentClearAll(void) {
    pendingTiles.clear();
    if (device && context) destroyAllTiles();
    else gpuTiles.clear();
}

void propsRenderDiligentSetWind(float dirX, float dirZ, float speed, float strength) {
    float len = std::sqrt(dirX * dirX + dirZ * dirZ);
    if (len > 1e-6f) {
        windDir[0] = dirX / len;
        windDir[1] = dirZ / len;
    }
    windSpeed = speed;
    windStrength = strength;
    windTimeSec = 0.0;
    windPhaseRad = 0.0;
    lastNanos = 0;
}

void propsRenderDiligentSetEnabled(bool e) {
    enabled = e;
}

void propsRenderDiligentGetWindState(f32 wind[4], f32 phase[4], f32 player[4]) {
    if (wind) {
        wind[0] = windDir[0]; wind[1] = windDir[1];
        wind[2] = windSpeed;  wind[3] = gustStrength;
    }
    if (phase) {
        phase[0] = (f32)windPhaseRad; phase[1] = phase[2] = phase[3] = 0.0f;
    }
    if (player) {
        player[0] = playerPush[0]; player[1] = playerPush[1];
        player[2] = playerPush[2]; player[3] = playerPush[3];
    }
}

void propsRenderDiligentUpdate(void) {
    ++statFrame;
    statDrawsThisFrame = 0;
    statInstancesThisFrame = 0;

    // Advance the wind clock (capped so a hitch does not snap the canopy).
    const u64 now = utils::nanos();
    double dt = 0.0;
    if (lastNanos != 0) {
        dt = (double)(now - lastNanos) * 1e-9;
        windTimeSec += std::min(dt, 0.1);
    }
    lastNanos = now;

    // Gust envelope: the old engine's weather module drove the sway strength
    // (0.17..0.38 at 2.6..4.4 m/s gusts); with no weather module yet, two
    // slow incommensurate sines stand in so the field visibly breathes
    // instead of drifting at one unchanging amplitude. The phase integrates
    // the gust-modulated speed (a plain t*speed would jump as the gust
    // changes).
    {
        float t    = (float)windTimeSec;
        float gust = 0.5f + 0.5f * sinf(t * 0.35f + 2.0f * sinf(t * 0.11f + 1.3f));
        gustStrength = windStrength * (0.35f + 1.15f * gust);  // 0.35 → 0.12..0.52 m
        windPhaseRad += dt * (double)windSpeed * (0.7 + 0.8 * (double)gust); // 0.42..0.86 rad/s
    }

    // Player reaction: feet position + horizontal speed (the character's
    // own tick-rate speed — playerGetFootSpeed, a finite difference over the
    // FIXED timer.dt). NOT a rendered-frame difference: the feet only
    // advance on 1/UPS ticks, so dxz/dt-frame reads 0 between ticks and the
    // full tick step / frame-dt on tick frames — at >UPS fps the push
    // amplitude flickers (the unlimited-fps grass shake). Capped at 30 m/s
    // so a teleport reads as a full swish, not a discontinuity.
    {
        double foot[3];
        if (!playerPushEnabled() || !playerGetFootPos(foot)) {
            playerPush[0] = 1e9f; playerPush[1] = 0.0f;
            playerPush[2] = 1e9f; playerPush[3] = 0.0f;
        } else {
            float speed = (float)playerGetFootSpeed();
            if (speed > 30.0f) speed = 30.0f;
            playerPush[0] = (float)foot[0];
            playerPush[1] = (float)foot[1];
            playerPush[2] = (float)foot[2];
            playerPush[3] = speed;
        }
    }

    const double t0 = utils::elapsedBegin();
    if (!passReady) {
        if (!initPass()) return;
    }
    tickDeferred();
    syncEnvironment();
    if (meshDirty) uploadMesh();
    if (enabled) {
        evictOutsideWindow();
        applyPendingTiles();
    }
    const double ms = utils::elapsedEnd(t0);
    if (statFrame > kStatWarmupFrames && statCount < kStatFrames) {
        statSum += ms;
        statCount++;
        statAvgMs = statSum / (double)statCount;
    }
}

void propsRenderDiligentDraw(void) {
    static bool entryLogged = false;
    if (!entryLogged) {
        entryLogged = true;
        utils::info("props draw: first entry passReady=%d enabled=%d shadowPipeline=%p cachedGen=%u gen=%u",
                (int)passReady, (int)enabled, (void*)shadowPipeline, cachedShadowGen,
                engine::renderer::diligent::shadowDiligentGeneration());
    }
    if (!passReady || !enabled) return;
    if (!meshVbo || !meshIbo || meshIdxCount == 0) return;
    if (variantsCpu.empty()) return;

    GpuTile* first = nullptr;
    for (GpuTile& t : gpuTiles) {
        if (t.inUse && t.instanceCount > 0) { first = &t; break; }
    }
    if (!first) return;

    checkShadowGeneration();
    if (!passReady || !pipeline) return;  // a failed rebuild destroyed the lit state
    fillFrameAttribs();

    context->SetPipelineState(pipeline);
    context->SetVertexBuffers(0, 1, &meshVbo, nullptr,
            RESOURCE_STATE_TRANSITION_MODE_TRANSITION, SET_VERTEX_BUFFERS_FLAG_RESET);
    context->SetIndexBuffer(meshIbo, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    FrustumCullPlanes planes;
    extractCameraFrustumPlanes(planes);

    // The planes are in CAMERA-RELATIVE space (the view is rotation-only —
    // the eye is the render-space origin), so the AABBs must be tested
    // relative to the camera eye too.
    float camPos[3] = {0.0f, 0.0f, 0.0f};
    float camFwd[3] = {0.0f, 0.0f, 0.0f};
    renderer::rendererCameraGet(camPos, camFwd);

    static const bool dbgProps = getenv("ENGINE_PROPS_DEBUG") != nullptr;
    static bool dbgLogged = false;
    static bool dbgDrewLogged = false;
    static bool dbgCullLogged = false;
    if (dbgProps && !dbgLogged) {
        dbgLogged = true;
        utils::info("props dbg: tiles=%zu meshIdx=%u firstTile=(%d,%d) inst=%u ranges=%zu",
                gpuTiles.size(), meshIdxCount, first->tileX, first->tileZ,
                first->instanceCount, first->ranges.size());
        if (!first->ranges.empty()) {
            for (u32 ri = 0; ri < first->ranges.size() && ri < 12; ri++) {
                const GpuRange& rr = first->ranges[ri];
                utils::info("props dbg: range%u start=%u count=%u idx=[%u..%u] aabb=(%.0f %.0f %.0f)..(%.0f %.0f %.0f)",
                        ri, rr.start, rr.count, rr.indexOffset, rr.indexOffset + rr.indexCount,
                        rr.aabbMin[0], rr.aabbMin[1], rr.aabbMin[2],
                        rr.aabbMax[0], rr.aabbMax[1], rr.aabbMax[2]);
            }
        }
    }

    // Per-tile cull box: the tile's XZ footprint plus a reach margin for
    // props leaning over the edge; Y is deliberately loose (the per-range
    // AABBs cull precisely).

    bool drew = false;
    for (GpuTile& t : gpuTiles) {
        if (!t.inUse || t.instanceCount == 0) continue;

        const float x0 = (float)t.tileX * kTileM - kReach;
        const float z0 = (float)t.tileZ * kTileM - kReach;
        const float tileMin[3] = {x0 - camPos[0], kYMin - camPos[1], z0 - camPos[2]};
        const float tileMax[3] = {x0 + kTileM + 2.0f * kReach - camPos[0],
                kYMax - camPos[1],
                z0 + kTileM + 2.0f * kReach - camPos[2]};
        if (frustumCullAabbOutside(tileMin, tileMax, planes)) continue;

        for (GpuRange& r : t.ranges) {
            if (r.count == 0 || r.indexCount == 0) continue;
            float rmin[3] = {r.aabbMin[0] - camPos[0], r.aabbMin[1] - camPos[1],
                    r.aabbMin[2] - camPos[2]};
            float rmax[3] = {r.aabbMax[0] - camPos[0], r.aabbMax[1] - camPos[1],
                    r.aabbMax[2] - camPos[2]};
            if (rangeAabbValid(r) && frustumCullAabbOutside(rmin, rmax, planes)) continue;
            if (!r.srb) continue;

            context->CommitShaderResources(r.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            u32 done = 0;
            while (done < r.count) {
                const u32 chunk = std::min(r.count - done, kChunkInstances);
                DrawIndexedAttribs attrs;
                attrs.NumIndices          = r.indexCount;
                attrs.IndexType           = VT_UINT32;
                attrs.Flags               = DRAW_FLAG_NONE;
                attrs.FirstIndexLocation  = r.indexOffset;
                attrs.BaseVertex          = 0;
                attrs.NumInstances        = chunk;
                attrs.FirstInstanceLocation = r.start + done;
                context->DrawIndexed(attrs);
                done += chunk;
                statDrawsThisFrame++;
                if (dbgProps && !dbgDrewLogged) {
                    dbgDrewLogged = true;
                    utils::info("props dbg: first draw idx=%u inst=%u firstInst=%u", r.indexCount,
                            chunk, attrs.FirstInstanceLocation);
                }
            }
            statInstancesThisFrame += r.count;
            drew = true;
        }
    }
    if (dbgProps && !drew && !dbgCullLogged) {
        dbgCullLogged = true;
        utils::info("props dbg: culled everything");
    }
    (void)drew;
}

// The shadow pass' per-cascade instanced depth draw (called by
// shadowDiligentRenderCascades with the cascade DSV bound + the shared
// cbShadowPass filled).
void propsRenderDiligentShadowDraw(const FrustumCullPlanes* planes, const f32 camPos[3]) {
    if (!passReady || !shadowPipeline) return;
    if (!meshVbo || !meshIbo || meshIdxCount == 0) return;

    static bool srbChecked = false;
    if (!srbChecked) {
        srbChecked = true;
        u32 with = 0, without = 0;
        for (GpuTile& t : gpuTiles) {
            if (!t.inUse) continue;
            if (t.shadowSrb) with++; else without++;
        }
        utils::info("props shadow: tiles with srb %u, without %u", with, without);
    }

    // ENGINE_PROPS_SHADOW_ANCHOR: cross test — draw the TERRAIN LATTICE
    // through the PROPS shadow PSO (bare VS = 5 km bush, lattice input
    // layout, tile SRB). If the cascade map changes, the PSO works and the
    // problem is the mesh path.
    if (getenv("ENGINE_PROPS_SHADOW_ANCHOR")) {
        IShaderResourceBinding* dbgTileSrb = nullptr;
        for (GpuTile& t : gpuTiles) {
            if (t.inUse && t.shadowSrb) { dbgTileSrb = t.shadowSrb; break; }
        }
        static bool crossLogged = false;
        if (!crossLogged) {
            crossLogged = true;
            utils::info("props shadow: CROSS TEST %s (dbgTileSrb=%p, shadowPipeline=%p)",
                    dbgTileSrb ? "ACTIVE" : "SKIPPED", (void*)dbgTileSrb, (void*)shadowPipeline);
        }
        if (dbgTileSrb) {
            context->SetPipelineState(shadowPipeline);
            IShaderResourceBinding* commitSrb = dbgTileSrb;
            if (getenv("ENGINE_PROPS_SHADOW_TERRAINSRB")) {
                commitSrb = (IShaderResourceBinding*)engine::heightmapTerrainDiligentShadowSrb();
                utils::info("props shadow: TERRAIN SRB OVERRIDE %p", (void*)commitSrb);
            }
            if (commitSrb) {
                context->CommitShaderResources(commitSrb,
                        RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                engine::heightmapTerrainDiligentShadowDrawTilesOnly();
            }
            return;
        }
    }

    static const bool useTerrainPso = getenv("ENGINE_PROPS_SHADOW_TERRAINPSO") != nullptr;
    IPipelineState* drawPso = shadowPipeline;
    IShaderResourceBinding* drawSrb = nullptr;
    if (useTerrainPso) {
        drawPso = (IPipelineState*)engine::heightmapTerrainDiligentShadowPSO();
        drawSrb = (IShaderResourceBinding*)engine::heightmapTerrainDiligentShadowSrb();
        static bool expLogged = false;
        if (!expLogged) {
            expLogged = true;
            utils::info("props shadow exp: terrainPso=%p terrainSrb=%p meshVbo=%p meshIbo=%p idxCount=%u tiles=%zu",
                    (void*)drawPso, (void*)drawSrb, (void*)meshVbo, (void*)meshIbo, meshIdxCount, gpuTiles.size());
            for (GpuTile& t : gpuTiles) {
                if (!t.inUse) continue;
                for (u32 ri = 0; ri < t.ranges.size() && ri < 3; ri++) {
                    const GpuRange& r = t.ranges[ri];
                    utils::info("props shadow exp: range%u start=%u count=%u idxOff=%u idxCount=%u",
                            ri, r.start, r.count, r.indexOffset, r.indexCount);
                }
            }
        }
        if (!drawPso || !drawSrb) return;
        context->SetPipelineState(drawPso);
        context->CommitShaderResources(drawSrb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    } else {
        context->SetPipelineState(drawPso);
    }
    context->SetVertexBuffers(0, 1, &meshVbo, nullptr,
            RESOURCE_STATE_TRANSITION_MODE_TRANSITION, SET_VERTEX_BUFFERS_FLAG_RESET);
    context->SetIndexBuffer(meshIbo, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    static const bool dbgCull = getenv("ENGINE_SHADOW_CULL_DEBUG") != nullptr;
    u32 dbgTilesTotal = 0, dbgTilesKept = 0;
    u32 dbgRangesTotal = 0, dbgRangesKept = 0;

    for (GpuTile& t : gpuTiles) {
        if (!t.inUse || t.instanceCount == 0 || !t.shadowSrb) continue;
        dbgTilesTotal++;
        if (planes) {
            const float x0 = (float)t.tileX * kTileM - kReach;
            const float z0 = (float)t.tileZ * kTileM - kReach;
            const float tileMin[3] = {x0 - camPos[0], kYMin - camPos[1], z0 - camPos[2]};
            const float tileMax[3] = {x0 + kTileM + 2.0f * kReach - camPos[0],
                    kYMax - camPos[1],
                    z0 + kTileM + 2.0f * kReach - camPos[2]};
            if (frustumCullAabbOutside(tileMin, tileMax, *planes)) continue;
        }
        dbgTilesKept++;
        if (!useTerrainPso) {
            context->CommitShaderResources(t.shadowSrb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        for (GpuRange& r : t.ranges) {
            if (r.count == 0 || r.indexCount == 0) continue;
            dbgRangesTotal++;
            if (planes && rangeAabbValid(r)) {
                float rmin[3], rmax[3];
                for (int c = 0; c < 3; c++) {
                    rmin[c] = r.aabbMin[c] - kShadowSwayPad - camPos[c];
                    rmax[c] = r.aabbMax[c] + kShadowSwayPad - camPos[c];
                }
                if (frustumCullAabbOutside(rmin, rmax, *planes)) continue;
            }
            dbgRangesKept++;
            if (!useTerrainPso && r.baseTex) {
                if (IShaderResourceVariable* v =
                        t.shadowSrb->GetVariableByName(SHADER_TYPE_PIXEL, "g_BaseTex"))
                    v->Set(r.baseTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
                context->CommitShaderResources(t.shadowSrb,
                        RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            }
            u32 done = 0;
            while (done < r.count) {
                const u32 chunk = std::min(r.count - done, kChunkInstances);
                DrawIndexedAttribs attrs;
                attrs.NumIndices          = r.indexCount;
                attrs.IndexType           = VT_UINT32;
                attrs.Flags               = DRAW_FLAG_NONE;
                attrs.FirstIndexLocation  = r.indexOffset;
                attrs.BaseVertex          = 0;
#ifdef PROPS_SHADOW_DEBUG_NONINSTANCED
                attrs.NumInstances        = 1;
                attrs.FirstInstanceLocation = 0;
#else
                attrs.NumInstances        = chunk;
                attrs.FirstInstanceLocation = r.start + done;
#endif
                context->DrawIndexed(attrs);
                done += chunk;
            }
        }
    }
    if (dbgCull) {
        static bool dbgLogged = false;
        if (!dbgLogged) {
            dbgLogged = true;
            utils::info("props shadow cull: tiles %u/%u ranges %u/%u kept",
                    dbgTilesKept, dbgTilesTotal, dbgRangesKept, dbgRangesTotal);
        }
    }
}

PropsRenderStats propsRenderDiligentStats(void) {
    PropsRenderStats st = {};
    st.renderAvgMs = statAvgMs;
    st.applyAvgMs  = applyAvgMs;
    st.frame       = (u32)statFrame;
    st.gpuTiles    = (u32)gpuTiles.size();
    st.gpuDraws    = statDrawsThisFrame;
    st.gpuInstances = statInstancesThisFrame;
    size_t bytes = 0;
    for (const GpuTile& t : gpuTiles) {
        bytes += (size_t)((t.instanceCount * kTexelsPerInstance + kInstanceTexWidth - 1) /
                kInstanceTexWidth) * kInstanceTexWidth * 16u;
    }
    bytes += (size_t)meshVertsCpu.size() * sizeof(float) + (size_t)meshIdxCpu.size() * sizeof(u32);
    for (const auto& kv : variantTextures) {
        if (kv.second) bytes += 512u * 512u * 4u * 2u;
    }
    st.gpuBytes = bytes;
    size_t stagingBytes = 0;
    for (const PendingTile& p : pendingTiles) {
        stagingBytes += p.instances.size() * sizeof(PropsRenderInstance) +
                p.ranges.size() * sizeof(PropsRenderRange);
    }
    st.cpuStagingBytes = stagingBytes;
    st.pending = (u32)pendingTiles.size();
    return st;
}

void propsRenderDiligentDestroy(void) {
    pendingTiles.clear();
    variantsCpu.clear();
    meshVertsCpu.clear();
    meshIdxCpu.clear();
    meshDirty = false;
    enabled = false;

    if (!device) {
        gpuTiles.clear();
        deferred.clear();
        variantTextures.clear();
        passReady = false;
        initFailed = false;
        return;
    }

    destroyAllTiles();
    for (DeferredGpu& d : deferred) {
        if (d.tex) d.tex->Release();
    }
    deferred.clear();
    for (auto& kv : variantTextures) {
        if (kv.second) kv.second->Release();
    }
    variantTextures.clear();

    if (meshVbo) { meshVbo->Release(); meshVbo = nullptr; }
    if (meshIbo) { meshIbo->Release(); meshIbo = nullptr; }
    meshIdxCount = 0;
    if (frameAttribsCB) { frameAttribsCB->Release(); frameAttribsCB = nullptr; }
    if (clampSampler) { clampSampler->Release(); clampSampler = nullptr; }
    if (baseSampler) { baseSampler->Release(); baseSampler = nullptr; }
    if (whiteTex) { whiteTex->Release(); whiteTex = nullptr; }
    if (iblIrradiance) { iblIrradiance->Release(); iblIrradiance = nullptr; }
    if (iblPrefiltered) { iblPrefiltered->Release(); iblPrefiltered = nullptr; }
    if (ggxLUT) { ggxLUT->Release(); ggxLUT = nullptr; }
    if (ggxFallbackLUT) { ggxFallbackLUT->Release(); ggxFallbackLUT = nullptr; }
    if (shadowPipeline) { shadowPipeline->Release(); shadowPipeline = nullptr; }
    if (shadowPrs) { shadowPrs->Release(); shadowPrs = nullptr; }
    if (shadowVs) { shadowVs->Release(); shadowVs = nullptr; }
    if (pipeline) { pipeline->Release(); pipeline = nullptr; }
    if (vs) { vs->Release(); vs = nullptr; }
    if (ps) { ps->Release(); ps = nullptr; }
    if (prs) { prs->Release(); prs = nullptr; }

    statSum = 0.0;
    statCount = 0;
    statAvgMs = 0.0;
    applySum = 0.0;
    applyCount = 0;
    applyAvgMs = 0.0;
    passReady = false;
    initFailed = false;
}
}
