#include "terrain/TerrainInternal.h"

#include "Utils.h"
#include "gltf/Gltf.h"
#include "gltf/GltfInternal.h"
#include "logger/Logger.h"
#include "renderer/diligent/DiligentRenderer.h"

#include <Graphics/GraphicsEngine/interface/Buffer.h>
#include <Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <Graphics/GraphicsEngine/interface/PipelineState.h>
#include <Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <Graphics/GraphicsEngine/interface/Sampler.h>
#include <Graphics/GraphicsEngine/interface/Shader.h>
#include <Graphics/GraphicsEngine/interface/ShaderResourceVariable.h>
#include <Graphics/GraphicsEngine/interface/SwapChain.h>
#include <Graphics/GraphicsEngine/interface/Texture.h>
#include <Graphics/GraphicsTools/interface/GraphicsUtilities.h>
#include <Graphics/GraphicsTools/interface/MapHelper.hpp>
#include <Common/interface/RefCntAutoPtr.hpp>

#include <GLTFLoader.hpp>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace engine::terrain {
using namespace Diligent;
using engine::renderer::diligent::device;
using engine::renderer::diligent::context;
using engine::renderer::diligent::swapChain;

// ── splat shader (HLSL port of terrain.mat; compiled once at terrain init) ──
// Lighting is calibrated against the filament path's defaults (EV100 15 →
// exposure 1.2 * 2^-15, sun 110000 lux, ambient 30000) so both backends read
// as one scene. Row-major matrices: the app memcpys its Diligent float4x4.
static const char* kTerrainVS = R"(

cbuffer PerFrameVS
{
    row_major float4x4 g_viewProj;
};

struct VSIn
{
    float3 Pos     : ATTRIB0;
    float3 Normal  : ATTRIB1;
    float2 UV      : ATTRIB2;
    float3 Tangent : ATTRIB3;
};

struct PSIn
{
    float4 Pos      : SV_POSITION;
    float3 WorldPos : WORLDPOS;
    float2 UV       : TEXCOORD;
    float3 Normal   : NORMAL;
    float3 Tangent  : TANGENT;
};

void main(in VSIn v, out PSIn o)
{
    // terrain chunk nodes are identity (verified at load), so the vertex
    // position already is the world position
    float4 worldPos = float4(v.Pos, 1.0);
    o.Pos      = mul(worldPos, g_viewProj);
    o.WorldPos = v.Pos;
    o.UV       = v.UV;
    o.Normal   = v.Normal;
    o.Tangent  = v.Tangent;
}
)";

static const char* kTerrainPS = R"(

cbuffer PerFramePS
{
    float4 g_sunDir;      // xyz: direction the light travels (normalized)
    float4 g_sunColor;    // rgb: color, w: intensity (lux)
    float4 g_ambient;     // rgb: color, w: intensity (lux)
    float  g_diag;        // 0 = normal, 1 = height/1250, 2 = raw albedo, 3 = splat wsum
};

cbuffer MaterialPS
{
    // floats first: with int arrays first, the generated offsets violate
    // Vulkan's relaxed UBO layout (array stride 16) and the module fails
    // spirv-val — with the arrays trailing, nothing can overlap
    float g_sandHeight;
    float g_sandFade;
    float g_snowHeight;
    float g_snowFade;
    float g_cliffSlope;
    float g_cliffFade;
    float g_styleTiling;
    float g_pad0;
    int g_tileLayer0[100];
    int g_tileLayer1[100];
    int g_tileLayer2[100];
    int g_styleRemap[12];
}

Texture2DArray g_splatTiles;
Texture2DArray g_styleAlbedo;
Texture2DArray g_styleNormal;
Texture2DArray g_defaultAlbedo;
Texture2DArray g_defaultNormal;

SamplerState g_splatSampler;  // clamp at layer edges
SamplerState g_styleSampler;  // repeat for world-space tiling

struct PSIn
{
    float4 Pos      : SV_POSITION;
    float3 WorldPos : WORLDPOS;
    float2 UV       : TEXCOORD;
    float3 Normal   : NORMAL;
    float3 Tangent  : TANGENT;
};

// UDIM tile lookup: uv spans 0..10 over a 10x10 tile grid. Empty tiles never
// became array layers, so each group carries its own tile->layer table
// (-1 = absent, weights treated as zero).
// Splat images are painted in Blender, which initializes them opaque black
// (0,0,0,255): the alpha style is painted by ERASING alpha, so the alpha
// weight is 1.0 - a (r/g/b are direct weights).
void accumulate(float4 weights, int group, float2 suv,
        inout float wsum, inout float3 albedo, inout float rough,
        inout float ao, inout float2 nAcc)
{
    for (int c = 0; c < 4; c++)
    {
        float w = weights[c];
        if (w <= 0.002)
            continue;
        int style = g_styleRemap[group * 4 + c];
        if (style < 0)
            continue;
        float3 uv = float3(suv, float(style));
        float4 a = g_styleAlbedo.Sample(g_styleSampler, uv);
        float4 n = g_styleNormal.Sample(g_styleSampler, uv);
        albedo += w * a.rgb;
        rough  += w * a.a;
        ao     += w * n.z;
        nAcc   += w * (n.xy * 2.0 - 1.0);
        wsum   += w;
    }
}

// engine default fallback styles (pak_0_engine, layer order fixed by the
// loader): sand 0, grass 1, snow 2, cliff 3
void accumulateDefault(float w, float layer, float2 suv,
        inout float wsum, inout float3 albedo, inout float rough,
        inout float ao, inout float2 nAcc)
{
    if (w <= 0.002)
        return;
    float3 uv = float3(suv, layer);
    float4 a = g_defaultAlbedo.Sample(g_styleSampler, uv);
    float4 n = g_defaultNormal.Sample(g_styleSampler, uv);
    albedo += w * a.rgb;
    rough  += w * a.a;
    ao     += w * n.z;
    nAcc   += w * (n.xy * 2.0 - 1.0);
    wsum   += w;
}

float3 acesFilm(float3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

float4 main(in PSIn i) : SV_Target
{
    // filament flips the glTF V (uv0_frag = vec2(u, 1-v)) before the splat
    // math; our vertices carry the raw glTF uv, so flip here to keep the
    // double-flip (this flip + the tileUV.y flip below) identical
    float2 uv10 = clamp(float2(i.UV.x, 1.0 - i.UV.y), 0.0, 9.999999);
    float2 tileCoord = floor(uv10);
    float2 tileUV = uv10 - tileCoord;
    // tileUV.y is Blender-oriented (0 = tile bottom), while the KTX2 tile's
    // V=0 is the image's top row — flip to sample tiles right side up
    tileUV.y = 1.0 - tileUV.y;
    int tile = int(tileCoord.x) + 10 * int(tileCoord.y);

    float4 w0 = float4(0.0, 0.0, 0.0, 0.0);
    float4 w1 = float4(0.0, 0.0, 0.0, 0.0);
    float4 w2 = float4(0.0, 0.0, 0.0, 0.0);

    int layer0 = g_tileLayer0[tile];
    if (layer0 >= 0)
    {
        w0 = g_splatTiles.Sample(g_splatSampler, float3(tileUV, float(layer0)));
        w0.a = 1.0 - w0.a;
    }
    int layer1 = g_tileLayer1[tile];
    if (layer1 >= 0)
    {
        w1 = g_splatTiles.Sample(g_splatSampler, float3(tileUV, float(layer1)));
        w1.a = 1.0 - w1.a;
    }
    int layer2 = g_tileLayer2[tile];
    if (layer2 >= 0)
    {
        w2 = g_splatTiles.Sample(g_splatSampler, float3(tileUV, float(layer2)));
        w2.a = 1.0 - w2.a;
    }

    float3 worldPos = i.WorldPos;
    float2 suv = worldPos.xz * g_styleTiling;

    float wsum = 0.0;
    float3 albedo = float3(0.0, 0.0, 0.0);
    float rough = 0.0;
    float ao = 0.0;
    float2 nAcc = float2(0.0, 0.0);

    accumulate(w0, 0, suv, wsum, albedo, rough, ao, nAcc);
    accumulate(w1, 1, suv, wsum, albedo, rough, ao, nAcc);
    accumulate(w2, 2, suv, wsum, albedo, rough, ao, nAcc);

    float3 geomNormal = normalize(i.Normal);

    if (wsum < 0.004)
    {
        // unpainted area: procedural defaults from the engine pak — sand near
        // sea level, snow high up, cliff on steep slopes, grass everywhere
        // else (cliff overrides the others)
        float slope = 1.0 - geomNormal.y;
        float y = worldPos.y;

        float snowW  = smoothstep(g_snowHeight - g_snowFade, g_snowHeight, y);
        float sandW  = 1.0 - smoothstep(g_sandHeight, g_sandHeight + g_sandFade, y);
        float grassW = (1.0 - snowW) * (1.0 - sandW);
        float cliffW = smoothstep(g_cliffSlope, g_cliffSlope + g_cliffFade, slope);

        float open = 1.0 - cliffW;
        accumulateDefault(sandW * open, 0.0, suv, wsum, albedo, rough, ao, nAcc);
        accumulateDefault(grassW * open, 1.0, suv, wsum, albedo, rough, ao, nAcc);
        accumulateDefault(snowW * open, 2.0, suv, wsum, albedo, rough, ao, nAcc);
        accumulateDefault(cliffW, 3.0, suv, wsum, albedo, rough, ao, nAcc);
    }

    float inv = 1.0 / max(wsum, 1e-4);
    float3 baseColor = albedo * inv;
    float aoF = clamp(ao * inv, 0.0, 1.0);

    // whiteout-style normal reconstruction from the blended tangent xy, then
    // tangent -> world (glTF tangents; handedness assumed +)
    float2 n2 = nAcc * inv;
    float r2 = clamp(dot(n2, n2), 0.0, 0.98);
    float3 normalTS = normalize(float3(n2, sqrt(1.0 - r2)));
    float3 T = normalize(i.Tangent - geomNormal * dot(i.Tangent, geomNormal));
    float3 B = cross(geomNormal, T);
    float3 N = normalize(T * normalTS.x + B * normalTS.y + geomNormal * normalTS.z);

    // diagnostic (ENGINE_DEBUG_TERRAIN=diag|albedo|wsum|norm|geomn)
    if (g_diag > 0.5)
    {
        if (g_diag < 1.5)
            return float4(clamp(worldPos.y / 1250.0, 0.0, 1.0), 0.0, 0.0, 1.0);  // height
        if (g_diag < 2.5)
            return float4(clamp(baseColor, 0.0, 1.0), 1.0);                     // raw albedo
        if (g_diag < 3.5)
            return float4(clamp(wsum, 0.0, 1.0), 0.0, 0.0, 1.0);               // splat coverage
        if (g_diag < 4.5)
            return float4(N * 0.5 + 0.5, 1.0);                                 // final normal
        return float4(geomNormal * 0.5 + 0.5, 1.0);                           // geometry normal
    }

    // lighting calibrated against the filament path's defaults: sun 110000
    // lux + constant ambient, exposure 1.2 * 2^-15 (filament EV100 15),
    // Lambert diffuse, ACES tonemap (specular lobe skipped — terrain look is
    // diffuse-dominated)
    float exposure = 1.2 * exp2(-15.0);
    float3 L = normalize(-g_sunDir.xyz);
    float NdotL = saturate(dot(N, L));
    float3 direct = g_sunColor.rgb * (g_sunColor.w * exposure * NdotL);
    float3 ambient = g_ambient.rgb * (g_ambient.w * exposure) * aoF;
    float3 color = baseColor * (direct + ambient) / 3.14159265;
    return float4(acesFilm(color), 1.0);
}
)";

static RefCntAutoPtr<IShader> vertexShader;
static RefCntAutoPtr<IShader> pixelShader;
static RefCntAutoPtr<IPipelineState> terrainPSO;
static RefCntAutoPtr<IShaderResourceBinding> terrainSRB;
static RefCntAutoPtr<IBuffer> frameVSBuffer;
static RefCntAutoPtr<IBuffer> framePSBuffer;
static RefCntAutoPtr<IBuffer> materialBuffer;
static RefCntAutoPtr<ISampler> splatSampler;
static RefCntAutoPtr<ISampler> styleSampler;

// CPU mirror of the MaterialPS cbuffer. HLSC cbuffer packing aligns int array
// ELEMENTS to 16-byte strides (arrays align to max(element, 16)), while a plain
// C int array is packed 4-byte contiguous — a memcpy of the packed layout makes
// the GPU's g_tileLayerX[tile] read the CPU's tileLayer[tile*4], and
// g_tileLayer1/2 + g_styleRemap read past the buffer end (undefined UBO reads).
// Pad every int to 16 bytes so the CPU and GPU layouts match element for
// element; the buffer must be sized with sizeof(TerrainMaterialCpu).
struct alignas(16) TerrainInt16 {
    int v;
    int pad[3];
};
struct alignas(16) TerrainMaterialCpu {
    float sandHeight, sandFade, snowHeight, snowFade;
    float cliffSlope, cliffFade, styleTiling, pad0;
    TerrainInt16 tileLayer[TerrainParams::kMaxGroups][TerrainParams::kMaxTiles];
    TerrainInt16 styleRemap[12];
};
static_assert(offsetof(TerrainMaterialCpu, tileLayer) == 32,
        "tileLayer must start where the GPU cbuffer's float section ends");
static_assert(offsetof(TerrainMaterialCpu, styleRemap) == 32 + sizeof(TerrainMaterialCpu::tileLayer),
        "styleRemap must follow the three splat tables");

static RefCntAutoPtr<ITexture> splatTilesTex;
static RefCntAutoPtr<ITexture> styleAlbedoTex;
static RefCntAutoPtr<ITexture> styleNormalTex;
static RefCntAutoPtr<ITexture> defaultAlbedoTex;
static RefCntAutoPtr<ITexture> defaultNormalTex;

static bool shadersReady = false;
static bool resourcesReady = false;
static std::set<u32> chunkNodes;  // scene node indexes that are terrain chunks
static bool ownsDrawing = false;

bool createShader(const char* source, SHADER_TYPE type, const char* name, RefCntAutoPtr<IShader>& out) {
    ShaderCreateInfo shaderCI;
    shaderCI.SourceLanguage = SHADER_SOURCE_LANGUAGE_HLSL;
    shaderCI.Desc.ShaderType = type;
    shaderCI.EntryPoint = "main";
    shaderCI.Desc.Name = name;
    shaderCI.Source = source;
    device->CreateShader(shaderCI, &out);
    if (!out) {
        utils::warn("terrain: %s shader compile failed", name);
        return false;
    }
    return true;
}

bool terrainStartDiligent(void) {
    if (!device) {
        utils::warn("terrain: renderer not initialized");
        return false;
    }

    if (!createShader(kTerrainVS, SHADER_TYPE_VERTEX, "terrain splat VS", vertexShader) ||
        !createShader(kTerrainPS, SHADER_TYPE_PIXEL, "terrain splat PS", pixelShader)) {
        return false;
    }
    shadersReady = true;

    // samplers: splat clamp, styles repeat
    SamplerDesc splatDesc;
    splatDesc.AddressU = TEXTURE_ADDRESS_CLAMP;
    splatDesc.AddressV = TEXTURE_ADDRESS_CLAMP;
    splatDesc.AddressW = TEXTURE_ADDRESS_CLAMP;
    splatDesc.MinFilter = FILTER_TYPE_LINEAR;
    splatDesc.MagFilter = FILTER_TYPE_LINEAR;
    splatDesc.MipFilter = FILTER_TYPE_LINEAR;
    device->CreateSampler(splatDesc, &splatSampler);

    SamplerDesc styleDesc = splatDesc;
    styleDesc.AddressU = TEXTURE_ADDRESS_WRAP;
    styleDesc.AddressV = TEXTURE_ADDRESS_WRAP;
    styleDesc.AddressW = TEXTURE_ADDRESS_WRAP;
    device->CreateSampler(styleDesc, &styleSampler);

    CreateUniformBuffer(device, sizeof(float4x4), "terrain frame VS constants", &frameVSBuffer);
    CreateUniformBuffer(device, 3 * sizeof(float4) + sizeof(float), "terrain frame PS constants", &framePSBuffer);
    CreateUniformBuffer(device, sizeof(TerrainMaterialCpu), "terrain material constants", &materialBuffer);
    if (!frameVSBuffer || !framePSBuffer || !materialBuffer) {
        utils::warn("terrain: constant buffer creation failed");
        return false;
    }
    return true;
}

bool terrainArrayDiligent(TerrainArrayKind kind, TerrainDecodedArray& array) {
    if (array.layers.empty() || !device) {
        return false;
    }

    const size_t layerCount = array.layers.size();
    const size_t levelCount = array.layers[0].size();

    TextureDesc texDesc;
    texDesc.Name = "terrain splat array";
    texDesc.Type = RESOURCE_DIM_TEX_2D_ARRAY;
    texDesc.Usage = USAGE_IMMUTABLE;
    texDesc.BindFlags = BIND_SHADER_RESOURCE;
    texDesc.Format = array.srgb ? TEX_FORMAT_BC7_UNORM_SRGB : TEX_FORMAT_BC7_UNORM;
    texDesc.Width = array.layers[0][0].width;
    texDesc.Height = array.layers[0][0].height;
    texDesc.MipLevels = (Uint32)levelCount;
    texDesc.ArraySize = (Uint32)layerCount;

    // subresource = mip + layer * mipLevels; BC7 stride = one row of blocks
    std::vector<TextureSubResData> subresources(layerCount * levelCount);
    for (size_t layer = 0; layer < layerCount; layer++) {
        for (size_t mip = 0; mip < levelCount; mip++) {
            const TerrainLevelBlocks& level = array.layers[layer][mip];
            const size_t blockCols = (level.width + 3) / 4;
            subresources[mip + layer * levelCount] =
                    TextureSubResData(level.blocks, blockCols * 16);
        }
    }
    TextureData initData(subresources.data(), (Uint32)subresources.size());

    RefCntAutoPtr<ITexture> texture;
    device->CreateTexture(texDesc, &initData, &texture);
    freeDecodedArray(array);
    if (!texture) {
        utils::warn("terrain: texture array upload failed");
        return false;
    }

    switch (kind) {
        case TerrainArrayKind::SplatTiles: splatTilesTex = texture; break;
        case TerrainArrayKind::StyleAlbedo: styleAlbedoTex = texture; break;
        case TerrainArrayKind::StyleNormal: styleNormalTex = texture; break;
        case TerrainArrayKind::DefaultAlbedo: defaultAlbedoTex = texture; break;
        case TerrainArrayKind::DefaultNormal: defaultNormalTex = texture; break;
    }
    return true;
}

bool terrainFinishDiligent(const TerrainParams& params) {
    if (!splatTilesTex || !styleAlbedoTex || !styleNormalTex || !defaultAlbedoTex ||
        !defaultNormalTex) {
        return false;
    }

    // material constants (written once); the struct mirrors the GPU cbuffer's
    // 16-byte int array strides — see TerrainMaterialCpu
    {
        TerrainMaterialCpu material{};
        for (int g = 0; g < TerrainParams::kMaxGroups; g++) {
            for (int t = 0; t < TerrainParams::kMaxTiles; t++) {
                material.tileLayer[g][t].v = params.tileLayer[g][t];
            }
        }
        for (int i = 0; i < 12; i++) {
            material.styleRemap[i].v = params.styleRemap[i];
        }
        material.sandHeight = params.sandHeight;
        material.sandFade = params.sandFade;
        material.snowHeight = params.snowHeight;
        material.snowFade = params.snowFade;
        material.cliffSlope = params.cliffSlope;
        material.cliffFade = params.cliffFade;
        material.styleTiling = params.styleTiling;

        void* mapped = nullptr;
        context->MapBuffer(materialBuffer, MAP_WRITE, MAP_FLAG_NONE, mapped);
        if (mapped) {
            memcpy(mapped, &material, sizeof(material));
            context->UnmapBuffer(materialBuffer, MAP_WRITE);
        } else {
            utils::warn("terrain: material constants map failed");
            return false;
        }
    }

    // PSO for the swapchain pass
    GraphicsPipelineStateCreateInfo psoCI;
    PipelineStateDesc& psoDesc = psoCI.PSODesc;
    psoDesc.Name = "terrain splat PSO";
    psoDesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
    psoCI.GraphicsPipeline.NumRenderTargets = 1;
    psoCI.GraphicsPipeline.RTVFormats[0] = swapChain->GetDesc().ColorBufferFormat;
    psoCI.GraphicsPipeline.DSVFormat = swapChain->GetDesc().DepthBufferFormat;
    psoCI.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // glTF front faces are CCW; the PBR path (GltfDiligent) sets
    // FrontCounterClockwise = true and the splat path must match it. With the
    // winding flipped the folded heightfield culls its own front faces:
    // a see-through pentagon in top-down, back-faces visible from below.
    // ENGINE_DEBUG_TERRAIN=cull|front|depth|none overrides for diagnosis.
    CULL_MODE cull = CULL_MODE_BACK;
    bool frontCCW = True;
    bool depthEnable = True;
    if (const char* dbg = getenv("ENGINE_DEBUG_TERRAIN")) {
        if (strcmp(dbg, "cull") == 0) cull = CULL_MODE_NONE;
        else if (strcmp(dbg, "front") == 0) frontCCW = True;
        else if (strcmp(dbg, "depth") == 0) depthEnable = False;
        else if (strcmp(dbg, "none") == 0) { cull = CULL_MODE_NONE; depthEnable = False; }
    }
    psoCI.GraphicsPipeline.RasterizerDesc.CullMode = cull;
    psoCI.GraphicsPipeline.RasterizerDesc.FrontCounterClockwise = frontCCW;
    psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable = depthEnable;
    psoCI.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = True;
    psoCI.pVS = vertexShader;
    psoCI.pPS = pixelShader;

    // vertex layout matches GLTF::DefaultVertexAttributes for the attributes
    // the terrain model carries (pos/norm/uv in buffer 0, tangent in buffer 4).
    // Vulkan uses Diligent's cross-platform ATTRIBn semantics.
    InputLayoutDescX inputLayout;
    inputLayout
        .Add(0u, 0u, 3u, VT_FLOAT32, False)        // ATTRIB0: POSITION  @ buffer 0 (32B)
        .Add(1u, 0u, 3u, VT_FLOAT32, False, 12u)   // ATTRIB1: NORMAL
        .Add(2u, 0u, 2u, VT_FLOAT32, False, 24u)   // ATTRIB2: TEXCOORD
        .Add(3u, 4u, 3u, VT_FLOAT32, False);       // ATTRIB3: TANGENT    @ buffer 4 (12B)
    psoCI.GraphicsPipeline.InputLayout = inputLayout;

    device->CreateGraphicsPipelineState(psoCI, &terrainPSO);
    if (!terrainPSO) {
        utils::warn("terrain: PSO creation failed");
        return false;
    }

    // all shader resources are static (single resource each), so assign them
    // on the pipeline state BEFORE the SRB is created: CreateShaderResourceBinding
    // (InitStaticResources=true) snapshots the PSO's static cache into the SRB
    // at creation time, and anything set afterwards is invisible to the SRB
    // ("No resource is assigned to static shader variable" at commit)
    auto setStatic = [&](const char* name, SHADER_TYPE type, IDeviceObject* resource) {
        IShaderResourceVariable* var = terrainPSO->GetStaticVariableByName(type, name);
        if (!var) {
            utils::warn("terrain: shader variable %s missing", name);
            return false;
        }
        var->Set(resource);
        return true;
    };
    auto setStaticTexture = [&](const char* name, ITexture* texture) {
        return setStatic(name, SHADER_TYPE_PIXEL, texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    };
    if (!setStatic("PerFrameVS", SHADER_TYPE_VERTEX, frameVSBuffer) ||
        !setStatic("PerFramePS", SHADER_TYPE_PIXEL, framePSBuffer) ||
        !setStatic("MaterialPS", SHADER_TYPE_PIXEL, materialBuffer) ||
        !setStaticTexture("g_splatTiles", splatTilesTex) ||
        !setStaticTexture("g_styleAlbedo", styleAlbedoTex) ||
        !setStaticTexture("g_styleNormal", styleNormalTex) ||
        !setStaticTexture("g_defaultAlbedo", defaultAlbedoTex) ||
        !setStaticTexture("g_defaultNormal", defaultNormalTex) ||
        !setStatic("g_splatSampler", SHADER_TYPE_PIXEL, splatSampler) ||
        !setStatic("g_styleSampler", SHADER_TYPE_PIXEL, styleSampler)) {
        return false;
    }

    terrainPSO->CreateShaderResourceBinding(&terrainSRB, true);
    if (!terrainSRB) {
        utils::warn("terrain: SRB creation failed");
        return false;
    }

    resourcesReady = true;
    return true;
}

void terrainApplyDiligent(void) {
    chunkNodes.clear();
    ownsDrawing = false;
    if (!resourcesReady) {
        return;
    }

    u64 chunks[TerrainParams::kMaxTiles];
    size_t found = gltf::gltfEntitiesNamed("terrain_chunk_", chunks, TerrainParams::kMaxTiles);
    for (size_t i = 0; i < found && i < TerrainParams::kMaxTiles; i++) {
        chunkNodes.insert((u32)chunks[i]);
    }

    // the splat path draws the whole model only when it is entirely terrain
    // (the current data is 100/100 chunks); mixed models stay on GLTF_PBR
    size_t meshNodes = gltf::gltfDiligentMeshNodeCount();
    ownsDrawing = !chunkNodes.empty() && chunkNodes.size() >= meshNodes;
    utils::info("terrain: %zu/%zu mesh nodes are chunks (%s)", chunkNodes.size(), meshNodes,
            ownsDrawing ? "splat path" : "pbr path");
}

bool terrainDiligentOwnsDrawing(void) {
    return ownsDrawing && resourcesReady;
}

void terrainDiligentDrawWorld(IDeviceContext* ctx, const Diligent::GLTF::Model& model,
        const Diligent::GLTF::ModelTransforms& transforms) {
    if (!ownsDrawing || !resourcesReady || model.Scenes.empty()) {
        return;
    }

    if (getenv("ENGINE_DEBUG_CAM")) {
        static int dbg = 0;
        if (dbg++ < 3) {
            const float4x4 viewProj =
                    engine::renderer::diligent::diligentFrameView() *
                    engine::renderer::diligent::diligentFrameProj();
            // corner of the model bounds through the actual game matrices
            const float pts[4][4] = {{-3704.62f, 0.74f, -3737.88f, 1.0f}, {3728.75f, 1202.50f, 3753.62f, 1.0f}};
            const float m[4][4] = {{viewProj._11, viewProj._12, viewProj._13, viewProj._14},
                    {viewProj._21, viewProj._22, viewProj._23, viewProj._24},
                    {viewProj._31, viewProj._32, viewProj._33, viewProj._34},
                    {viewProj._41, viewProj._42, viewProj._43, viewProj._44}};
            for (int i = 0; i < 2; i++) {
                float c[4];
                for (int j = 0; j < 4; j++) {
                    c[j] = pts[i][0] * m[0][j] + pts[i][1] * m[1][j] + pts[i][2] * m[2][j] + pts[i][3] * m[3][j];
                }
                utils::warn("terrain: corner %d clip = %.4f %.4f %.4f w=%.4f z_ndc=%.4f", i, c[0], c[1], c[2], c[3], c[2] / c[3]);
            }
            utils::warn("terrain: drawWorld running (nodes=%zu)", model.Scenes[0].LinearNodes.size());
        }
    }

    // per-frame constants
    {
        const float4x4 viewProj =
                engine::renderer::diligent::diligentFrameView() *
                engine::renderer::diligent::diligentFrameProj();
        MapHelper<float4x4> frameVS(ctx, frameVSBuffer, MAP_WRITE, MAP_FLAG_DISCARD);
        *frameVS = viewProj;
    }
    {
        struct FramePS {
            float4 sunDir;
            float4 sunColor;
            float4 ambient;
            float diag;
        };
        const f32* sunDir = engine::renderer::diligent::diligentSunDirection();
        const f32* sunColor = engine::renderer::diligent::diligentSunColor();
        const f32* ambient = engine::renderer::diligent::diligentAmbientColor();
        f32 sunIntensity = engine::renderer::diligent::diligentSunIntensity();
        f32 ambientIntensity = engine::renderer::diligent::diligentAmbientIntensity();

        float diag = 0.0f;
        if (const char* dbg = getenv("ENGINE_DEBUG_TERRAIN")) {
            if (strcmp(dbg, "diag") == 0) diag = 1.0f;      // height
            else if (strcmp(dbg, "albedo") == 0) diag = 2.0f; // raw baseColor
            else if (strcmp(dbg, "wsum") == 0) diag = 3.0f;   // splat coverage
            else if (strcmp(dbg, "norm") == 0) diag = 4.0f;   // final normal
            else if (strcmp(dbg, "geomn") == 0) diag = 5.0f;  // geometry normal
        }

        MapHelper<FramePS> framePS(ctx, framePSBuffer, MAP_WRITE, MAP_FLAG_DISCARD);
        float3 dir = normalize(float3{sunDir[0], sunDir[1], sunDir[2]});
        framePS->sunDir = float4{dir, 0.0f};
        framePS->sunColor = float4{sunColor[0], sunColor[1], sunColor[2], sunIntensity};
        framePS->ambient = float4{ambient[0], ambient[1], ambient[2], ambientIntensity};
        framePS->diag = diag;
    }

    // vertex + index buffers (same buffers GLTF_PBR_Renderer would use)
    const Uint32 numVBs = (Uint32)model.GetVertexBufferCount();
    IBuffer* vertexBuffers[8] = {};
    Uint32 numUsedVBs = 0;
    for (Uint32 i = 0; i < numVBs && i < 8; i++) {
        vertexBuffers[i] = model.GetVertexBuffer(i);
        numUsedVBs = i + 1;
    }
    ctx->SetVertexBuffers(0, numUsedVBs, vertexBuffers, nullptr,
            RESOURCE_STATE_TRANSITION_MODE_TRANSITION, SET_VERTEX_BUFFERS_FLAG_RESET);
    IBuffer* indexBuffer = model.GetIndexBuffer();
    if (indexBuffer) {
        ctx->SetIndexBuffer(indexBuffer, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }

    ctx->SetPipelineState(terrainPSO);
    ctx->CommitShaderResources(terrainSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    const GLTF::Scene& scene = model.Scenes[0];
    const Uint32 baseVertex = model.GetBaseVertex();
    const Uint32 firstIndex = model.GetFirstIndexLocation();
    // the loader converts all indices to uint32 (GLTF_PBR_Renderer does the same)
    const VALUE_TYPE indexType = VT_UINT32;

    for (const GLTF::Node* node : scene.LinearNodes) {
        if (!node->pMesh) {
            continue;
        }
        for (const GLTF::Primitive& primitive : node->pMesh->Primitives) {
            if (primitive.HasIndices() && indexBuffer) {
                DrawIndexedAttribs draw{primitive.IndexCount, indexType, DRAW_FLAG_VERIFY_ALL};
                draw.FirstIndexLocation = firstIndex + primitive.FirstIndex;
                draw.BaseVertex = baseVertex + primitive.FirstVertex;
                ctx->DrawIndexed(draw);
            } else {
                DrawAttribs draw{primitive.VertexCount, DRAW_FLAG_VERIFY_ALL};
                draw.StartVertexLocation = baseVertex + primitive.FirstVertex;
                ctx->Draw(draw);
            }
        }
    }
}

void terrainDestroyDiligent(void) {
    ownsDrawing = false;
    resourcesReady = false;
    shadersReady = false;
    chunkNodes.clear();

    splatTilesTex.Release();
    styleAlbedoTex.Release();
    styleNormalTex.Release();
    defaultAlbedoTex.Release();
    defaultNormalTex.Release();
    terrainSRB.Release();
    terrainPSO.Release();
    materialBuffer.Release();
    framePSBuffer.Release();
    frameVSBuffer.Release();
    styleSampler.Release();
    splatSampler.Release();
    pixelShader.Release();
    vertexShader.Release();
}
}  // namespace engine::terrain
