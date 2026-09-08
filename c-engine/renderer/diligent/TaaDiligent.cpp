// Diligent half of temporal anti-aliasing (see TaaDiligent.h for the
// pipeline shape). The heavy lifting is DiligentFX's PostFXContext +
// TemporalAntiAliasing (prebuilt libDiligentFX.a); this module owns the
// offscreen chain, the jittered projection, the camera-attribs constant
// buffer and the final blit into the sRGB swapchain backbuffer.
// Motion-vector convention (must match the world passes' output): NDC-space
// per-pixel delta (currNDC − currJitter) − (prevNDC − prevJitter), the
// same quantity RenderPBR.psh's GetMotionVector produces for the glTF pass.
// Integration reference: DiligentSamples Tutorial27_PostProcessing
// (PrepareResources every frame, Execute with camera CB + curr/prev depth +
// motion vectors, GetAccumulatedFrameSRV feeds the final pass).

#include "renderer/diligent/TaaDiligent.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsTools/interface/MapHelper.hpp"
#include "Graphics/GraphicsEngine/interface/Shader.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"
#include "PostProcess/Common/interface/PostFXContext.hpp"
#include "PostProcess/TemporalAntiAliasing/interface/TemporalAntiAliasing.hpp"
#include "Utils.h"
#include "renderer/RenderBackend.h"
#include "renderer/diligent/DiligentRenderer.h"
#include "renderer/diligent/SsaoDiligent.h"
#include "renderer/diligent/BloomDiligent.h"
#include "Graphics/GraphicsTools/interface/ScopedDebugGroup.hpp"
#include "stb/git/stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

// The HLSL shared structs (CameraAttribs, TemporalAntiAliasingAttribs) as
// the passes' convention: include the .fxh inside namespace Diligent::HLSL.
// Order matters for the TAA structures (ShaderDefinitions must come first).
namespace Diligent {
namespace HLSL {
#include "Shaders/Common/public/BasicStructures.fxh"
#include "Shaders/Common/public/ShaderDefinitions.fxh"
#include "Shaders/PostProcess/TemporalAntiAliasing/public/TemporalAntiAliasingStructures.fxh"
}
}

namespace engine::renderer::diligent {

using namespace Diligent;

static std::unique_ptr<PostFXContext> postFXContext;
static std::unique_ptr<TemporalAntiAliasing> taa;

static RefCntAutoPtr<IBuffer> cameraCB;

// Offscreen chain (swapped to backbuffer by the blit).
static RefCntAutoPtr<ITexture> sceneColorTex;  // RGBA16F linear
static RefCntAutoPtr<ITexture> motionTex;      // RG16F, NDC deltas
static RefCntAutoPtr<ITexture> normalTex;
static RefCntAutoPtr<ITexture> aoCompositeTex; // RGBA16F, AO-applied world color
static RefCntAutoPtr<ITexture> depthTex[2];    // D32, double-buffered

static u32 frameIdx = 0;
static u32 targetWidth = 0;
static u32 targetHeight = 0;

static bool taaOn = false;
static float taaWeight = 0.9f; // settings.taaWeight → TemporalStabilityFactor

// [0] = this frame, [1] = previous frame (raw row-major storage; the
// PostFX shaders get them through a PackMatrixRowMajor PostFXContext).
static HLSL::CameraAttribs camAttribs[2] = {};
static float2 currJitter{0.0f, 0.0f};

// ENGINE_MV_DUMP=path: one-shot motion-buffer dump (frame mvDumpFrame) —
// RGB image of the world passes' TAA motion vectors (R = mv.x, G = mv.y in
// F3NDC, B = |mv|; 1.0 == 0.5 NDC == half the screen).  Used to verify the
// MVs actually written by the world passes against the expected camera
// motion.  The per-frame camera translation (lastDEyeMag, metres) is logged
// alongside for the comparison.
static const char* mvDumpPath = nullptr;
static bool mvDumpDone = false;
static u32 mvDumpFrame = 60;
static f32 lastDEyeMag = 0.0f;

// Last frame's world anchor (= the camera eye; f64, see diligentWorldAnchor).
// Feeds the prev-camera anchor correction below; reset on (re-)init so a
// stale eye cannot skew the first frame's motion vectors.
static f64 prevEye[3] = {0.0, 0.0, 0.0};
static bool havePrevEye = false;
static f32 lastDEye[3] = {0.0f, 0.0f, 0.0f};

static RefCntAutoPtr<IShader> blitVS;
static RefCntAutoPtr<IShader> blitPS;
static RefCntAutoPtr<IPipelineState> blitPSO;
static RefCntAutoPtr<ISampler> blitSampler;

// One SRB per source texture (the 2026-09-05 rmlui lesson: in-place SRV
// updates invalidate the command buffer — build one SRB per texture and
// cache it; the sources are few: scene color + the two TAA accumulators).
static std::unordered_map<ITexture*, RefCntAutoPtr<IShaderResourceBinding>> blitSrbs;

// AMD FidelityFX RCAS — the old engine's "cas" (its rcas.shader was a GLSL
// recast of FsrRcasFilterF with FSR_RCAS_DENOISE on, the exact config the
// FSR3 upscaler's rcas pass used). Stateless 3x3 cross-tap pass over the
// linear HDR composite: here it runs after the TAA resolve (or over the raw
// scene color), before the sRGB encode-on-store into the backbuffer — the
// same placement class as the old engine's final pass (exposed HDR, before
// its LPM tone curve).
// g_Cas.Sharpness = exp2(2 * casStrength - 2) (the FSR3 host-side remap the
// old final pass used; casStrength 0..1.5, 0 = off, 1.0 = AMD reference
// max). The lobe re-clamp to RCAS_LIMIT after the multiply is the old
// engine's extension: it lets >1.0 amplify the lobe without flipping the
// resolve denominator (inverted / NaN pixels).

static float casStrength = 0.0f;
static float renderScale = 1.0f;

struct CasAttribs {
    float2 texel;      // 1 / source size, per axis
    float sharpness;
    float pad;
};

static RefCntAutoPtr<IShader> downPS;
static RefCntAutoPtr<IShader> casPS;
static RefCntAutoPtr<IPipelineState> casPSO;
static RefCntAutoPtr<ISampler> casSampler;
static RefCntAutoPtr<IBuffer> casCB;
// One SRB per source texture (the 2026-09-05 rmlui lesson: in-place SRV
// updates invalidate the command buffer — cache by texture, like the blit).
static std::unordered_map<ITexture*, RefCntAutoPtr<IShaderResourceBinding>> casSrbs;

// renderScale > 1 downsample: the final CAS/blit runs at backbuffer
// resolution with a single tap per output pixel — at a 2x source that keeps
// 1 of every 4 texels, so the downsample itself re-aliases everything the
// world rendered (temporal crawl whenever the camera moves; a static camera
// hides it because the sub-sample pattern is stable). TAA resolves at the
// offscreen resolution, so its output dies in the same undersampled
// downsample — the box pass below first averages into a backbuffer-sized
// intermediate; CAS/blit then read THAT.
struct DownsampleAttribs {
    float2 footprint;  // one output pixel expressed in source UV
    float2 pad;
};
static RefCntAutoPtr<IPipelineState> downPSO;
static RefCntAutoPtr<ITexture> downTex;
static RefCntAutoPtr<IBuffer> downCB;
static std::unordered_map<ITexture*, RefCntAutoPtr<IShaderResourceBinding>> downSrbs;

// Defined below with the blit: binds the backbuffer RT + viewport/scissor,
// commits the pass' SRB and draws the fullscreen triangle.
static void drawFullscreenToRtv(IDeviceContext* ctx, IPipelineState* pso,
        IShaderResourceBinding* srb, ITextureView* rtv, u32 w, u32 h);
static void drawFullscreenToBackbuffer(IDeviceContext* ctx, IPipelineState* pso,
        IShaderResourceBinding* srb, ITextureView* backRTV);

static constexpr char kBlitVS[] = R"(
struct VSOut
{
    float4 Pos : SV_Position;
    float2 UV : UV;
};

VSOut main(in uint VertId : SV_VertexID)
{
    VSOut Out;
    float2 xy = float2(VertId == 1 ? 3.0 : -1.0, VertId == 2 ? 3.0 : -1.0);
    Out.Pos = float4(xy, 0.0, 1.0);
    Out.UV  = float2((xy.x + 1.0) * 0.5, 1.0 - (xy.y + 1.0) * 0.5);
    return Out;
}
)";

// The world renders LINEAR values (today's shaders write linear into the
// sRGB-encoded swapchain, which encodes on store). Sampling the linear
// offscreen and storing the same value into the sRGB backbuffer is the
// identical encode-on-write — no extra conversion here.
static constexpr char kBlitPS[] = R"(
Texture2D<float4> g_Source;
SamplerState g_Source_sampler;

struct PSIn
{
    float4 Pos : SV_Position;
    float2 UV : UV;
};

float4 main(in PSIn In) : SV_Target
{
    return g_Source.Sample(g_Source_sampler, In.UV);
}
)";

// Box downsample: 4 bilinear taps at the footprint quarters approximate a
// 4x4 box — exact for the 2x downscale (renderScale is clamped to 2.0).
static constexpr char kDownsamplePS[] = R"(
Texture2D<float4> g_Source;
SamplerState g_Source_sampler;

struct DownsampleAttribs
{
    float2 footprint;
    float2 pad;
};
cbuffer cbDownsampleAttribs
{
    DownsampleAttribs g_Downsample;
}

struct PSIn
{
    float4 Pos : SV_Position;
    float2 UV : UV;
};

float4 main(in PSIn In) : SV_Target
{
    float2 o = g_Downsample.footprint * 0.25;
    float4 s = g_Source.Sample(g_Source_sampler, In.UV + float2(-o.x, -o.y));
    s += g_Source.Sample(g_Source_sampler, In.UV + float2(o.x, -o.y));
    s += g_Source.Sample(g_Source_sampler, In.UV + float2(-o.x, o.y));
    s += g_Source.Sample(g_Source_sampler, In.UV + float2(o.x, o.y));
    return s * 0.25;
}
)";

// ENGINE_TAA_DEBUG_MV variant: encode motion as mv + 0.5 so zero motion is
// mid-gray (0.5, 0.5) — survives JPEG quantization for exact readings.
// Magnitude: (value − 0.5) NDC per frame; 0.01 NDC ≈ 10 px at 1080p ≈ 128/255.
static constexpr char kBlitMvPS[] = R"(
Texture2D<float4> g_Source;
SamplerState g_Source_sampler;

struct PSIn
{
    float4 Pos : SV_Position;
    float2 UV : UV;
};

float4 main(in PSIn In) : SV_Target
{
    float2 mv = g_Source.Sample(g_Source_sampler, In.UV).xy;
    return float4(mv + 0.5, 0.0, 1.0);
}
)";

static void createBlitPSO(void);

static void createDownPSO(void) {
    if (downPSO) {
        return;
    }
    if (!blitVS) {
        createBlitPSO();
    }

    ShaderCreateInfo shaderCI;
    shaderCI.SourceLanguage = SHADER_SOURCE_LANGUAGE_HLSL;
    shaderCI.Desc.ShaderType = SHADER_TYPE_PIXEL;
    shaderCI.EntryPoint = "main";
    shaderCI.Desc.Name = "taaDownsamplePS";
    shaderCI.Source = kDownsamplePS;
    device->CreateShader(shaderCI, &downPS);
    if (!downPS) {
        utils::warn("taa: downsample PS failed");
        return;
    }

    GraphicsPipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name = "taaDownsample";
    psoCI.pVS = blitVS;
    psoCI.pPS = downPS;
    GraphicsPipelineDesc& gp = psoCI.GraphicsPipeline;
    gp.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    gp.RasterizerDesc.CullMode = CULL_MODE_NONE;
    gp.DepthStencilDesc.DepthEnable = False;
    gp.NumRenderTargets = 1;
    gp.RTVFormats[0] = TEX_FORMAT_RGBA16_FLOAT;
    PipelineResourceLayoutDesc& layout = psoCI.PSODesc.ResourceLayout;
    ShaderResourceVariableDesc vars[3] = {
            {SHADER_TYPE_PIXEL, "g_Source", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
            {SHADER_TYPE_PIXEL, "g_Source_sampler", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "cbDownsampleAttribs", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    layout.Variables = vars;
    layout.NumVariables = 3;

    device->CreateGraphicsPipelineState(psoCI, &downPSO);
    if (!downPSO) {
        utils::warn("taa: downsample PSO failed");
        return;
    }

    if (IShaderResourceVariable* v = downPSO->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_Source_sampler")) {
        v->Set(blitSampler);
    }
    if (IShaderResourceVariable* v = downPSO->GetStaticVariableByName(SHADER_TYPE_PIXEL, "cbDownsampleAttribs")) {
        v->Set(downCB, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    }
}

static void createBlitPSO(void) {
    ShaderCreateInfo shaderCI;
    shaderCI.SourceLanguage = SHADER_SOURCE_LANGUAGE_HLSL;
    shaderCI.Desc.ShaderType = SHADER_TYPE_VERTEX;
    shaderCI.EntryPoint = "main";
    shaderCI.Desc.Name = "taaBlitVS";
    shaderCI.Source = kBlitVS;
    device->CreateShader(shaderCI, &blitVS);
    if (!blitVS) {
        utils::warn("taa: blit VS failed");
        return;
    }

    shaderCI.Desc.ShaderType = SHADER_TYPE_PIXEL;
    shaderCI.Desc.Name = "taaBlitPS";
    shaderCI.Source = getenv("ENGINE_TAA_DEBUG_MV") != nullptr ? kBlitMvPS : kBlitPS;
    device->CreateShader(shaderCI, &blitPS);
    if (!blitPS) {
        utils::warn("taa: blit PS failed");
        return;
    }

    GraphicsPipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name = "taaBlit";
    psoCI.pVS = blitVS;
    psoCI.pPS = blitPS;
    GraphicsPipelineDesc& gp = psoCI.GraphicsPipeline;
    gp.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    gp.RasterizerDesc.CullMode = CULL_MODE_NONE;
    gp.DepthStencilDesc.DepthEnable = False;
    gp.NumRenderTargets = 1;
    gp.RTVFormats[0] = swapChain->GetDesc().ColorBufferFormat;
    // glslang splits the HLSL combined sampler: g_Source (texture, dynamic —
    // alternates between scene color and the TAA accumulators) +
    // g_Source_sampler (sampler, static — assigned once below).
    PipelineResourceLayoutDesc& layout = psoCI.PSODesc.ResourceLayout;
    ShaderResourceVariableDesc vars[2] = {
            {SHADER_TYPE_PIXEL, "g_Source", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
            {SHADER_TYPE_PIXEL, "g_Source_sampler", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    layout.Variables = vars;
    layout.NumVariables = 2;

    device->CreateGraphicsPipelineState(psoCI, &blitPSO);
    if (!blitPSO) {
        utils::warn("taa: blit PSO failed");
        return;
    }

    SamplerDesc sampDesc;
    sampDesc.MagFilter = FILTER_TYPE_LINEAR;
    sampDesc.MinFilter = FILTER_TYPE_LINEAR;
    sampDesc.AddressU = TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = TEXTURE_ADDRESS_CLAMP;
    device->CreateSampler(sampDesc, &blitSampler);
    if (IShaderResourceVariable* v = blitPSO->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_Source_sampler")) {
        v->Set(blitSampler);
    }
}

static constexpr char kCasPS[] = R"(
Texture2D<float4> g_Source;
SamplerState g_Source_sampler;

struct CasAttribs
{
    float2 Texel;
    float Sharpness;
    float Pad;
};
cbuffer cbCasAttribs
{
    CasAttribs g_Cas;
}

struct PSIn
{
    float4 Pos : SV_Position;
    float2 UV : UV;
};

static const float RCAS_LIMIT = 0.25 - 1.0 / 16.0;

/* Approximate reciprocal with fp16-ish precision (AMD
 * ffxApproximateReciprocalMedium). The resolve deliberately uses this to
 * avoid visible tonality changes. */
float rcasRcpMed(float x) { return exp2(-log2(x)); }

/* Sharpening algorithm uses a minimal 3x3 neighborhood (cross taps):
 *    b
 *  d e f
 *    h
 *
 * Ported from the old engine's rcas.shader — FsrRcasFilterF (f32) with
 * FSR_RCAS_DENOISE on; the math is unchanged. */
float3 rcasFilter(float3 b, float3 d, float3 e, float3 f, float3 h, float s) {
    // Luma times 2.
    float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
    float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
    float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
    float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
    float hL = h.b * 0.5 + (h.r * 0.5 + h.g);

    // Noise detection.
    float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
    nz = saturate(abs(nz) * rcasRcpMed(max(max(max(bL, dL), eL), max(fL, hL)) -
                                       min(min(min(bL, dL), eL), min(fL, hL))));
    nz = -0.5 * nz + 1.0;

    // Min and max of ring.
    float mn4R = min(min(min(b.r, d.r), f.r), h.r);
    float mn4G = min(min(min(b.g, d.g), f.g), h.g);
    float mn4B = min(min(min(b.b, d.b), f.b), h.b);
    float mx4R = max(max(max(b.r, d.r), f.r), h.r);
    float mx4G = max(max(max(b.g, d.g), f.g), h.g);
    float mx4B = max(max(max(b.b, d.b), f.b), h.b);

    // Limiters (high precision division).
    float hitMinR = mn4R / (4.0 * mx4R);
    float hitMinG = mn4G / (4.0 * mx4G);
    float hitMinB = mn4B / (4.0 * mx4B);
    float hitMaxR = (1.0 - mx4R) / (4.0 * mn4R - 4.0);
    float hitMaxG = (1.0 - mx4G) / (4.0 * mn4G - 4.0);
    float hitMaxB = (1.0 - mx4B) / (4.0 * mn4B - 4.0);
    float lobeR   = max(-hitMinR, hitMaxR);
    float lobeG   = max(-hitMinG, hitMaxG);
    float lobeB   = max(-hitMinB, hitMaxB);
    float lobe =
        max(-RCAS_LIMIT, min(max(max(lobeR, lobeG), lobeB), 0.0)) * s;

    /* Engine extension (old engine's rcas.shader): s may exceed 1.0 (the
     * slider goes to 150%). AMD only ever uses multipliers in [0.25, 1.0],
     * where the resolve denominator 1 + 4*lobe is guaranteed >= 0.25. Above
     * 1.0 an unclamped lobe could reach -0.375 and flip the denominator
     * negative (inverted / NaN pixels). Re-clamp after the multiply: >1.0
     * amplifies the adaptive lobe but never past the kernel's hard bound. */
    lobe = max(-RCAS_LIMIT, lobe);

    // Apply noise removal.
    lobe *= nz;

    // Resolve.
    float rcpL = rcasRcpMed(4.0 * lobe + 1.0);
    return (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
}

float4 main(in PSIn In) : SV_Target
{
    const float2 t = g_Cas.Texel;
    float3 b = g_Source.Sample(g_Source_sampler, In.UV + float2(0.0, -t.y)).rgb;
    float3 d = g_Source.Sample(g_Source_sampler, In.UV + float2(-t.x, 0.0)).rgb;
    float4 e4 = g_Source.Sample(g_Source_sampler, In.UV);
    float3 e = e4.rgb;
    float3 f = g_Source.Sample(g_Source_sampler, In.UV + float2(t.x, 0.0)).rgb;
    float3 h = g_Source.Sample(g_Source_sampler, In.UV + float2(0.0, t.y)).rgb;
    return float4(rcasFilter(b, d, e, f, h, g_Cas.Sharpness), e4.a);
}
)";

static void createCasPSO(void) {
    if (casPSO) {
        return;
    }
    if (!blitVS) {  // the RCAS pass rides the blit's fullscreen-triangle VS
        createBlitPSO();
    }

    ShaderCreateInfo shaderCI;
    shaderCI.SourceLanguage = SHADER_SOURCE_LANGUAGE_HLSL;
    shaderCI.Desc.ShaderType = SHADER_TYPE_PIXEL;
    shaderCI.EntryPoint = "main";
    shaderCI.Desc.Name = "taaCasPS";
    shaderCI.Source = kCasPS;
    device->CreateShader(shaderCI, &casPS);
    if (!casPS) {
        utils::warn("taa: cas PS failed");
        return;
    }

    GraphicsPipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name = "taaCas";
    psoCI.pVS = blitVS;
    psoCI.pPS = casPS;
    GraphicsPipelineDesc& gp = psoCI.GraphicsPipeline;
    gp.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    gp.RasterizerDesc.CullMode = CULL_MODE_NONE;
    gp.DepthStencilDesc.DepthEnable = False;
    gp.NumRenderTargets = 1;
    gp.RTVFormats[0] = swapChain->GetDesc().ColorBufferFormat;
    PipelineResourceLayoutDesc& layout = psoCI.PSODesc.ResourceLayout;
    ShaderResourceVariableDesc vars[3] = {
            {SHADER_TYPE_PIXEL, "g_Source", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
            {SHADER_TYPE_PIXEL, "g_Source_sampler", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "cbCasAttribs", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    layout.Variables = vars;
    layout.NumVariables = 3;

    device->CreateGraphicsPipelineState(psoCI, &casPSO);
    if (!casPSO) {
        utils::warn("taa: cas PSO failed");
        return;
    }

    // POINT: the taps land exactly on the four ring texels (a LINEAR sampler
    // would average the center into them).
    SamplerDesc sampDesc;
    sampDesc.MagFilter = FILTER_TYPE_POINT;
    sampDesc.MinFilter = FILTER_TYPE_POINT;
    sampDesc.AddressU = TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = TEXTURE_ADDRESS_CLAMP;
    device->CreateSampler(sampDesc, &casSampler);
    if (IShaderResourceVariable* v = casPSO->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_Source_sampler")) {
        v->Set(casSampler);
    }
    // Static cbuffer: set once on the PSO (the blit's g_Source_sampler
    // pattern); the contents are re-mapped per frame (MapHelper) so the
    // ALLOW_OVERWRITE flag keeps it live without SRB churn.
    if (IShaderResourceVariable* v = casPSO->GetStaticVariableByName(SHADER_TYPE_PIXEL, "cbCasAttribs")) {
        v->Set(casCB, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    }
}

static IShaderResourceBinding* casSrbFor(ITextureView* src) {
    auto it = casSrbs.find(src->GetTexture());
    if (it != casSrbs.end()) {
        return it->second;
    }
    RefCntAutoPtr<IShaderResourceBinding> srb;
    casPSO->CreateShaderResourceBinding(&srb, true);
    if (!srb) {
        utils::warn("taa: cas SRB creation failed");
        return nullptr;
    }
    if (IShaderResourceVariable* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, "g_Source")) {
        v->Set(src);
    }
    auto inserted = casSrbs.emplace(src->GetTexture(), std::move(srb));
    return inserted.first->second;  // NOT the (moved-from) local
}

static IShaderResourceBinding* downSrbFor(ITextureView* src) {
    auto it = downSrbs.find(src->GetTexture());
    if (it != downSrbs.end()) {
        return it->second;
    }
    RefCntAutoPtr<IShaderResourceBinding> srb;
    downPSO->CreateShaderResourceBinding(&srb, true);
    if (!srb) {
        utils::warn("taa: downsample SRB creation failed");
        return nullptr;
    }
    if (IShaderResourceVariable* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, "g_Source")) {
        v->Set(src);
    }
    auto inserted = downSrbs.emplace(src->GetTexture(), std::move(srb));
    return inserted.first->second;
}

// Box-filters src (renderScale× the backbuffer) into the backbuffer-sized
// downTex; returns its SRV, or null when the pass is unavailable (caller
// falls back to the undersampled direct path).
static ITextureView* downsampleToIntermediate(IDeviceContext* ctx, ITextureView* src) {
    createDownPSO();
    if (!downPSO || !downTex || !downCB) {
        return nullptr;
    }
    {
        MapHelper<DownsampleAttribs> cb(ctx, downCB, MAP_WRITE, MAP_FLAG_DISCARD);
        cb[0].footprint = float2(1.0f / (float)downTex->GetDesc().Width,
                1.0f / (float)downTex->GetDesc().Height);
        cb[0].pad = float2{0.0f, 0.0f};
    }
    IShaderResourceBinding* srb = downSrbFor(src);
    if (!srb) {
        return nullptr;
    }
    ITextureView* rtv = downTex->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    drawFullscreenToRtv(ctx, downPSO, srb, rtv,
            downTex->GetDesc().Width, downTex->GetDesc().Height);
    return downTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
}

// Returns false if the PSO could not be built (caller falls back to the
// plain blit).
static bool casToBackbuffer(IDeviceContext* ctx, ITextureView* src, ITextureView* backRTV) {
    createCasPSO();
    if (!casPSO || !casCB) {
        return false;
    }

    // Per-frame cbuffer: exclusive dynamic-heap region via MapHelper — NOT
    // UpdateBuffer (the blind heap-offset-0 collision, docs/lessons.md
    // 2026-09-05). The map must land before the SRB commit below (the
    // descriptor's dynamic offset is written at commit time). Texel follows
    // the ACTUAL input (after the box downsample it is the intermediate, not
    // the offscreen chain).
    {
        const TextureDesc& srcDesc = src->GetTexture()->GetDesc();
        MapHelper<CasAttribs> cb(ctx, casCB, MAP_WRITE, MAP_FLAG_DISCARD);
        cb[0].texel = float2(1.0f / (float)srcDesc.Width, 1.0f / (float)srcDesc.Height);
        cb[0].sharpness = std::exp2(2.0f * casStrength - 2.0f);
        cb[0].pad = 0.0f;
    }

    IShaderResourceBinding* srb = casSrbFor(src);
    if (!srb) {
        return false;
    }
    drawFullscreenToBackbuffer(ctx, casPSO, srb, backRTV);
    return true;
}

// SSAO application: 1:1 point-tap multiply of the full-res AO map into the
// resolved world color. The AO map is a 1:1 screen-space map of this frame
// (SSAO resolves at the offscreen size), so the same UV lands on the same
// pixel in both textures; a POINT sampler keeps the 1:1 composite from
// averaging neighbours. strength = 1.0 is the unaltered AO, 0.0 a no-op.
static constexpr char kAoCompositePS[] = R"(
Texture2D<float4> g_Source;
Texture2D<float> g_AO;
SamplerState g_Source_sampler;

struct AoCompositeAttribs
{
    float Strength;
    float Pad;
};
cbuffer cbAoCompositeAttribs
{
    AoCompositeAttribs g_AoComposite;
}

struct PSIn
{
    float4 Pos : SV_Position;
    float2 UV : UV;
};

float4 main(in PSIn In) : SV_Target
{
    float4 c = g_Source.Sample(g_Source_sampler, In.UV);
    float ao = g_AO.Sample(g_Source_sampler, In.UV);
    c.rgb *= 1.0 - (1.0 - ao) * g_AoComposite.Strength;
    return c;
}
)";

struct AoCompositeAttribs {
    float strength;
    float pad;
};

static RefCntAutoPtr<IShader> aoPS;
static RefCntAutoPtr<IPipelineState> aoPSO;
static RefCntAutoPtr<ISampler> aoSampler;
static RefCntAutoPtr<IBuffer> aoCB;
static std::unordered_map<ITexture*, RefCntAutoPtr<IShaderResourceBinding>> aoSrbs;

static void createAoPSO(void) {
    if (aoPSO) {
        return;
    }
    if (!blitVS) {
        createBlitPSO();
    }

    ShaderCreateInfo shaderCI;
    shaderCI.SourceLanguage = SHADER_SOURCE_LANGUAGE_HLSL;
    shaderCI.Desc.ShaderType = SHADER_TYPE_PIXEL;
    shaderCI.EntryPoint = "main";
    shaderCI.Desc.Name = "aoCompositePS";
    shaderCI.Source = kAoCompositePS;
    device->CreateShader(shaderCI, &aoPS);
    if (!aoPS) {
        utils::warn("taa: ao composite PS failed");
        return;
    }

    GraphicsPipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name = "aoComposite";
    psoCI.pVS = blitVS;
    psoCI.pPS = aoPS;
    GraphicsPipelineDesc& gp = psoCI.GraphicsPipeline;
    gp.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    gp.RasterizerDesc.CullMode = CULL_MODE_NONE;
    gp.DepthStencilDesc.DepthEnable = False;
    gp.NumRenderTargets = 1;
    gp.RTVFormats[0] = TEX_FORMAT_RGBA16_FLOAT;
    PipelineResourceLayoutDesc& layout = psoCI.PSODesc.ResourceLayout;
    ShaderResourceVariableDesc vars[4] = {
            {SHADER_TYPE_PIXEL, "g_Source", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
            {SHADER_TYPE_PIXEL, "g_AO", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
            {SHADER_TYPE_PIXEL, "g_Source_sampler", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "cbAoCompositeAttribs", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    layout.Variables = vars;
    layout.NumVariables = 4;

    device->CreateGraphicsPipelineState(psoCI, &aoPSO);
    if (!aoPSO) {
        utils::warn("taa: ao composite PSO failed");
        return;
    }

    SamplerDesc sampDesc;
    sampDesc.MagFilter = FILTER_TYPE_POINT;
    sampDesc.MinFilter = FILTER_TYPE_POINT;
    sampDesc.AddressU = TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = TEXTURE_ADDRESS_CLAMP;
    device->CreateSampler(sampDesc, &aoSampler);
    if (IShaderResourceVariable* v = aoPSO->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_Source_sampler")) {
        v->Set(aoSampler);
    }
    if (IShaderResourceVariable* v = aoPSO->GetStaticVariableByName(SHADER_TYPE_PIXEL, "cbAoCompositeAttribs")) {
        v->Set(aoCB, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    }
}

static IShaderResourceBinding* aoSrbFor(ITextureView* src, ITextureView* ao) {
    auto it = aoSrbs.find(src->GetTexture());
    if (it != aoSrbs.end()) {
        return it->second;
    }
    RefCntAutoPtr<IShaderResourceBinding> srb;
    aoPSO->CreateShaderResourceBinding(&srb, true);
    if (!srb) {
        utils::warn("taa: ao composite SRB creation failed");
        return nullptr;
    }
    if (IShaderResourceVariable* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, "g_Source")) {
        v->Set(src);
    }
    if (IShaderResourceVariable* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, "g_AO")) {
        v->Set(ao);
    }
    auto inserted = aoSrbs.emplace(src->GetTexture(), std::move(srb));
    return inserted.first->second;
}

// Multiplies src's color by the AO map into aoCompositeTex; returns its SRV
// for the downsample/CAS/blit, or nullptr when the pass is unavailable (the
// caller keeps the plain source).
static ITextureView* aoCompositeApply(IDeviceContext* ctx, ITextureView* src, ITextureView* ao) {
    if (!aoCompositeTex || !aoCB || !src || !ao) {
        return nullptr;
    }
    createAoPSO();
    if (!aoPSO) {
        return nullptr;
    }
    {
        MapHelper<AoCompositeAttribs> cb(ctx, aoCB, MAP_WRITE, MAP_FLAG_DISCARD);
        cb[0].strength = ssaoIntensity();
        cb[0].pad = 0.0f;
    }
    IShaderResourceBinding* srb = aoSrbFor(src, ao);
    if (!srb) {
        return nullptr;
    }
    ITextureView* rtv = aoCompositeTex->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    drawFullscreenToRtv(ctx, aoPSO, srb, rtv,
            aoCompositeTex->GetDesc().Width, aoCompositeTex->GetDesc().Height);
    return aoCompositeTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
}

static IShaderResourceBinding* blitSrbFor(ITextureView* src) {
    auto it = blitSrbs.find(src->GetTexture());
    if (it != blitSrbs.end()) {
        return it->second;
    }
    RefCntAutoPtr<IShaderResourceBinding> srb;
    blitPSO->CreateShaderResourceBinding(&srb, true);
    if (!srb) {
        utils::warn("taa: blit SRB creation failed");
        return nullptr;
    }
    if (IShaderResourceVariable* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, "g_Source")) {
        v->Set(src);
    }
    auto inserted = blitSrbs.emplace(src->GetTexture(), std::move(srb));
    return inserted.first->second;  // NOT the (moved-from) local
}

// Binds rtv at w×h, commits the pass' SRB and draws the fullscreen triangle.
static void drawFullscreenToRtv(IDeviceContext* ctx, IPipelineState* pso,
        IShaderResourceBinding* srb, ITextureView* rtv, u32 w, u32 h) {
    ctx->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport vp(0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f);
    ctx->SetViewports(1, &vp, 0, 0);
    Rect scissor(0, 0, (i32)w, (i32)h);
    ctx->SetScissorRects(1, &scissor, 0, 0);

    ctx->SetPipelineState(pso);
    if (srb) {
        ctx->CommitShaderResources(srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
    DrawAttribs drawAttrs{3, DRAW_FLAG_VERIFY_ALL};
    ctx->Draw(drawAttrs);
    ctx->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
}

static void drawFullscreenToBackbuffer(IDeviceContext* ctx, IPipelineState* pso,
        IShaderResourceBinding* srb, ITextureView* backRTV) {
    const SwapChainDesc& scDesc = swapChain->GetDesc();
    drawFullscreenToRtv(ctx, pso, srb, backRTV, scDesc.Width, scDesc.Height);
}

static void blitToBackbuffer(IDeviceContext* ctx, ITextureView* src, ITextureView* backRTV) {
    if (!blitPSO) {
        createBlitPSO();
        if (!blitPSO) {
            return;
        }
    }

    drawFullscreenToBackbuffer(ctx, blitPSO, blitSrbFor(src), backRTV);
}

static void destroyTargets(void) {
    sceneColorTex.Release();
    motionTex.Release();
    normalTex.Release();
    aoCompositeTex.Release();
    depthTex[0].Release();
    depthTex[1].Release();
    downTex.Release();
    targetWidth = 0;
    targetHeight = 0;
    // The blit/cas SRBs reference the old views' textures — drop the caches.
    blitSrbs.clear();
    casSrbs.clear();
    downSrbs.clear();
    aoSrbs.clear();
}

static void createTargets(u32 width, u32 height) {
    destroyTargets();

    TextureDesc desc;
    desc.Type = RESOURCE_DIM_TEX_2D;
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;

    desc.Name = "taaSceneColor";
    desc.Format = TEX_FORMAT_RGBA16_FLOAT;  // linear HDR (TAA accumulates here)
    desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
    desc.ClearValue.Format = desc.Format;
    desc.ClearValue.Color[0] = kClearColor[0];
    desc.ClearValue.Color[1] = kClearColor[1];
    desc.ClearValue.Color[2] = kClearColor[2];
    desc.ClearValue.Color[3] = kClearColor[3];
    device->CreateTexture(desc, nullptr, &sceneColorTex);

    desc.Name = "taaMotionVectors";
    desc.Format = TEX_FORMAT_RG16_FLOAT;
    desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
    device->CreateTexture(desc, nullptr, &motionTex);

    desc.Name = "taaWorldNormal";
    desc.Format = TEX_FORMAT_RGBA16_FLOAT;
    desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
    device->CreateTexture(desc, nullptr, &normalTex);

    desc.Name = "aoComposite";
    desc.Format = TEX_FORMAT_RGBA16_FLOAT;
    desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
    device->CreateTexture(desc, nullptr, &aoCompositeTex);
    if (!aoCompositeTex) {
        utils::warn("taa: ao composite target failed");
    }

    // Downsample intermediate: backbuffer-sized (the box pass' output, the
    // CAS/blit input at renderScale > 1). Recreated with the swapchain here,
    // since createTargets fires on every resize via the target-size check.
    // Local desc copy — the depth textures below must keep the TARGET size.
    {
        TextureDesc dDesc = desc;
        const SwapChainDesc& scDesc = swapChain->GetDesc();
        dDesc.Name = "taaDownIntermediate";
        dDesc.Width = scDesc.Width;
        dDesc.Height = scDesc.Height;
        dDesc.Format = TEX_FORMAT_RGBA16_FLOAT;
        device->CreateTexture(dDesc, nullptr, &downTex);
        if (!downTex) {
            utils::warn("taa: downsample intermediate failed");
        }
    }

    // Double-buffered depth: the TAA reprojection compares this frame's
    // depth against LAST frame's (disocclusion detection).
    desc.Format = TEX_FORMAT_D32_FLOAT;
    desc.BindFlags = BIND_DEPTH_STENCIL | BIND_SHADER_RESOURCE;
    for (int i = 0; i < 2; i++) {
        desc.Name = i == 0 ? "taaDepthCurr" : "taaDepthPrev";
        device->CreateTexture(desc, nullptr, &depthTex[i]);
    }

    if (!sceneColorTex || !motionTex || !normalTex || !depthTex[0] || !depthTex[1]) {
        utils::warn("taa: offscreen target creation failed");
        destroyTargets();
        return;
    }
    targetWidth = width;
    targetHeight = height;
    utils::info("taa: targets %ux%u (rgba16f + rg16f motion + d32 x2)", width, height);
}

void taaInit(void) {
    if (!device) {
        return;
    }
    havePrevEye = false;
    mvDumpPath = getenv("ENGINE_MV_DUMP");
    if (const char* s = getenv("ENGINE_MV_DUMP_FRAME")) {
        mvDumpFrame = (u32)atoi(s);
    }
    PostFXContext::CreateInfo ci;
    ci.EnableAsyncCreation = false;
    ci.PackMatrixRowMajor = true;  // CameraAttribs filled raw (Tutorial27 style)
    postFXContext = std::make_unique<PostFXContext>(device, ci);

    TemporalAntiAliasing::CreateInfo taaCI;
    taa = std::make_unique<TemporalAntiAliasing>(device, taaCI);

    CreateUniformBuffer(device, 2 * sizeof(HLSL::CameraAttribs),
            "taa camera attribs", &cameraCB);
    if (!cameraCB) {
        utils::warn("taa: camera attribs buffer failed");
    }

    CreateUniformBuffer(device, sizeof(CasAttribs), "taa cas attribs", &casCB);
    if (!casCB) {
        utils::warn("taa: cas attribs buffer failed");
    }

    CreateUniformBuffer(device, sizeof(DownsampleAttribs), "taa downsample attribs", &downCB);
    if (!downCB) {
        utils::warn("taa: downsample attribs buffer failed");
    }

    CreateUniformBuffer(device, sizeof(AoCompositeAttribs), "taa ao composite attribs", &aoCB);
    if (!aoCB) {
        utils::warn("taa: ao composite attribs buffer failed");
    }

    frameIdx = 0;
}

void taaDestroy(void) {
    blitSrbs.clear();
    casSrbs.clear();
    downSrbs.clear();
    aoSrbs.clear();
    blitPSO.Release();;
    blitVS.Release();
    blitPS.Release();
    blitSampler.Release();
    casPSO.Release();
    casPS.Release();
    casSampler.Release();
    casCB.Release();
    downPSO.Release();
    downPS.Release();
    downCB.Release();
    aoPSO.Release();
    aoPS.Release();
    aoSampler.Release();
    aoCB.Release();
    destroyTargets();
    cameraCB.Release();
    taa.reset();
    postFXContext.reset();
}

void taaSettingsApply(bool enabled, float stabilityFactor, float cas, float scale) {
    taaOn = enabled;
    taaWeight = stabilityFactor;
    casStrength = cas;
    // Offscreen chain size changes on the next frame (the targetWidth/Height
    // comparison in taaFrameBegin recreates the targets); TAA history drops
    // with them and the frame-index continuity check resets the accumulator.
    renderScale = scale < 0.5f ? 0.5f : (scale > 2.0f ? 2.0f : scale);
    // History resets automatically: while disabled Execute() is skipped, so
    // on re-enable TAA sees CurrentFrameIdx != LastFrameIdx + 1 and treats
    // the stale history as invalid.
}

bool taaEnabled(void) {
    return taaOn;
}

void taaOnResized(void) {
    destroyTargets();
}

void taaFrameBegin(IDeviceContext* ctx, const float4x4& view, float4x4& proj) {
    if (!postFXContext || !taa || !cameraCB) {
        return;
    }

    frameIdx++;

    // Resolution scale: the world renders into the offscreen chain at
    // scSize * renderScale; the post-world blit (or CAS) upsamples it to the
    // backbuffer. The UI passes keep drawing at full swapchain resolution.
    const SwapChainDesc& scDesc = swapChain->GetDesc();
    const u32 tw = (u32)std::max(1.0f, (float)scDesc.Width * renderScale + 0.5f);
    const u32 th = (u32)std::max(1.0f, (float)scDesc.Height * renderScale + 0.5f);
    if (targetWidth != tw || targetHeight != th) {
        createTargets(tw, th);
    }
    if (!sceneColorTex) {
        return;
    }

    // Shared post-FX intermediates (closest-motion-vector pass input, blue
    // noise, ...) — every frame, like Tutorial27.
    PostFXContext::FrameDesc frameDesc;
    frameDesc.Index = frameIdx;
    frameDesc.Width = targetWidth;
    frameDesc.Height = targetHeight;
    frameDesc.OutputWidth = targetWidth;
    frameDesc.OutputHeight = targetHeight;
    postFXContext->PrepareResources(device, frameDesc, PostFXContext::FEATURE_FLAG_NONE);

    // Prepare the accumulators even while disabled so the frame-index
    // continuity check inside TAA resets the history on re-enable, and so
    // GetJitterOffset is live on the first enabled frame.
    const TemporalAntiAliasing::FEATURE_FLAGS taaFlags =
            TemporalAntiAliasing::FEATURE_FLAG_GAUSSIAN_WEIGHTING |
            TemporalAntiAliasing::FEATURE_FLAG_BICUBIC_FILTER |
            TemporalAntiAliasing::FEATURE_FLAG_YCOCG_COLOR_SPACE;
    taa->PrepareResources(device, ctx, postFXContext.get(), taaFlags);
    ssaoFrameBegin(ctx);
    bloomFrameBegin(ctx);

    // Jitter this frame's projection (TAA picks the Halton phase for the
    // CURRENT frame — PrepareResources above stamped the frame index).
    currJitter = taaOn ? taa->GetJitterOffset() : float2{0.0f, 0.0f};
    proj = TemporalAntiAliasing::GetJitteredProjMatrix(proj, currJitter);

    // Camera attribs (curr = [0], prev = [1]).
    camAttribs[1] = camAttribs[0];

    /* Prev-camera anchor correction (the ground-smearing fix).
     *
     * The world passes are camera-anchored: the VS subtracts the CURRENT
     * frame's anchor (the camera eye, f32-split in f4ExtraData[3..4]) and
     * feeds the rotation-only view matrices.  Their PrevClipPos is
     * PrevCamera.mViewProj * (P - anchorCurr), but the true previous-frame
     * projection is PrevCamera.mViewProj * (P - anchorPrev).  The missing
     * term is the per-frame camera translation dEye = (eyeCurr - eyePrev):
     * without it the motion field is rotation-only — TAA gets no
     * translation flow, the history stays screen-static while the world
     * flows, and the near ground smears into vertical streaks whenever the
     * character runs (walking flow is sub-threshold).
     *
     * Absorb the term so that
     *
     *     (rel + dEye, 1) consumed by M_prev = the true previous clip
     *
     * The two consumer families use OPPOSITE matrix conventions, so
     * taaFrameBegin keeps camAttribs[1] PURE and exposes the delta via
     * taaPrevAnchorDelta(); each fill site folds it in its own convention:
     *   - glTF (raw upload, row-vector consumed):  t_row * M_prev
     *   - terrain/props (transposed upload, column consumed):
     *       t_row * (M_prev ^ T)
     * with t_row = identity carrying dEye on its bottom row.  Both forms
     * apply exactly (rel + dEye, 1) through M_prev.
     *
     * Every consumer of taaPrevCameraAttribs (terrain/props/gltf MV) picks
     * this up without a shader change; the DiligentFX TAA shader only reads
     * g_PrevCamera.mProj (disocclusion test), which is untouched. */
    {
        f64 currEye[3];
        diligentWorldAnchor(currEye);
        if (havePrevEye) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                utils::info("taa: prev-camera anchor correction active");
            }
            const f32 dEye[3] = { (f32)(currEye[0] - prevEye[0]),
                                  (f32)(currEye[1] - prevEye[1]),
                                  (f32)(currEye[2] - prevEye[2]) };
            lastDEyeMag = std::sqrt(dEye[0] * dEye[0] + dEye[1] * dEye[1] +
                                    dEye[2] * dEye[2]);
            lastDEye[0] = dEye[0];
            lastDEye[1] = dEye[1];
            lastDEye[2] = dEye[2];
        }
        prevEye[0] = currEye[0];
        prevEye[1] = currEye[1];
        prevEye[2] = currEye[2];
        havePrevEye = true;

        // ENGINE_TAA_MV_PROBE=frame: log the motion vector the terrain PS
        // should produce for the pixel at (0.5W, 0.75H) for a world point
        // 4 m ahead of the camera, 1.5 m below the eye — the exact GPU-side
        // formula (curr/prev NDC from the anchor-relative clip positions).
        if (const char* e = getenv("ENGINE_TAA_MV_PROBE"); e && frameIdx == (u32)atoi(e)) {
            float4 fwd = float4(view._13, view._23, view._33, 0);
            float3 P{float(currEye[0]) + fwd.x * 4.0f,
                     float(currEye[1]) + fwd.y * 4.0f - 1.5f,
                     float(currEye[2]) + fwd.z * 4.0f};
            auto rowClip = [](const float4x4& M, const float3& r) {
                float4 v{r.x, r.y, r.z, 1.0f}, out{0, 0, 0, 0};
                out.x = v.x * M._11 + v.y * M._21 + v.z * M._31 + v.w * M._41;
                out.y = v.x * M._12 + v.y * M._22 + v.z * M._32 + v.w * M._42;
                out.z = v.x * M._13 + v.y * M._23 + v.z * M._33 + v.w * M._43;
                out.w = v.x * M._14 + v.y * M._24 + v.z * M._34 + v.w * M._44;
                return out;
            };
            float3 rel{P.x - (float)currEye[0],
                       P.y - (float)currEye[1],
                       P.z - (float)currEye[2]};
            float4 cc = rowClip(camAttribs[0].mViewProj, rel);
            float3 relPrev{rel.x + lastDEye[0], rel.y + lastDEye[1], rel.z + lastDEye[2]};
            float4 cp = rowClip(camAttribs[1].mViewProj, relPrev);
            float2 nCurr{cc.x / cc.w, cc.y / cc.w};
            float2 nPrev{cp.x / cp.w, cp.y / cp.w};
            float2 mv = (nCurr - camAttribs[0].f2Jitter) - (nPrev - camAttribs[1].f2Jitter);
            utils::info("taa: MVPROBE ndcCurr=(%+.4f,%+.4f) ndcPrev=(%+.4f,%+.4f) mv=(%+.5f,%+.5f) NDC",
                    nCurr.x, nCurr.y, nPrev.x, nPrev.y, mv.x, mv.y);
        }
    }

    HLSL::CameraAttribs& cam = camAttribs[0];
    cam = HLSL::CameraAttribs{};
    cam.mView = view;
    cam.mProj = proj;
    cam.mViewProj = view * proj;
    cam.mViewInv = view.Inverse();
    cam.mProjInv = proj.Inverse();
    cam.mViewProjInv = cam.mViewProj.Inverse();
    cam.f4Position = float4(float3::MakeVector(cam.mViewInv[3]), 1.0f);
    cam.f4ViewportSize = float4{(float)targetWidth, (float)targetHeight,
            1.0f / (float)targetWidth, 1.0f / (float)targetHeight};
    cam.SetClipPlanes(engine::renderer::kCameraNear, engine::renderer::kCameraFar);
    cam.fHandness = view.Determinant() > 0 ? 1.0f : -1.0f;
    cam.f2Jitter = currJitter;

    {
        MapHelper<HLSL::CameraAttribs> cb(ctx, cameraCB, MAP_WRITE, MAP_FLAG_DISCARD);
        cb[0] = camAttribs[0];
        cb[1] = camAttribs[1];
    }
}

float taaCurrentJitterX(void) {
    return currJitter.x;
}

float taaCurrentJitterY(void) {
    return currJitter.y;
}

void taaPrevAnchorDelta(f32 out[3]) {
    out[0] = lastDEye[0];
    out[1] = lastDEye[1];
    out[2] = lastDEye[2];
}

const void* taaPrevCameraAttribs(void) {
    return &camAttribs[1];
}

const void* taaCurrCameraAttribs(void) {
    return &camAttribs[0];
}

ITextureView* taaColorRTV(void) {
    return sceneColorTex ? sceneColorTex->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET) : nullptr;
}

void taaTargetSize(u32* w, u32* h) {
    *w = targetWidth;
    *h = targetHeight;
}

ITextureView* taaMotionRTV(void) {
    return motionTex ? motionTex->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET) : nullptr;
}

ITextureView* taaNormalRTV(void) {
    return normalTex ? normalTex->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET) : nullptr;
}

ITextureView* taaNormalSRV(void) {
    return normalTex ? normalTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

ITextureView* taaDepthDSV(void) {
    // curr parity must match taaWorldResolve's prev selection: this frame's
    // depth is buffer (frameIdx & 1), last frame's is the other one.
    return depthTex[frameIdx & 1] ? depthTex[frameIdx & 1]->GetDefaultView(TEXTURE_VIEW_DEPTH_STENCIL)
                                  : nullptr;
}

ITextureView* taaDepthSRV(int idx) {
    return depthTex[idx] ? depthTex[idx]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

PostFXContext* taaPostFXContext(void) {
    return postFXContext.get();
}

// IEEE half → float (the motion buffer is RG16F).
static inline float f16tof32(u16 h) {
    const u32 sign = (u32)(h & 0x8000u) << 16;
    u32 exp = (h >> 10) & 0x1Fu;
    u32 mant = h & 0x3FFu;
    u32 f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign;
        } else {
            while (!(mant & 0x400u)) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x3FFu;
            f = sign | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 0x1Fu) {
        f = sign | 0x7F800000u | (mant << 13);
    } else {
        f = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &f, sizeof(f));
    return out;
}

static void taaDumpMotionVectors(IDeviceContext* ctx) {
    // The motion texture is GPU-local (render target earlier this frame):
    // transition to COPY_SOURCE, copy into a one-shot staging texture and
    // map that.
    RefCntAutoPtr<ITexture> staging;
    {
        TextureDesc desc = motionTex->GetDesc();
        desc.Name = "taaMvDumpStaging";
        desc.Usage = USAGE_STAGING;
        desc.BindFlags = BIND_NONE;
        desc.CPUAccessFlags = CPU_ACCESS_READ;
        device->CreateTexture(desc, nullptr, &staging);
        if (!staging) {
            mvDumpDone = true;
            utils::warn("taa: motion dump failed (staging texture)");
            return;
        }
    }
    {
        CopyTextureAttribs copy{motionTex, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                staging, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
        ctx->CopyTexture(copy);
        // The Vulkan backend never waits for the GPU when mapping staging
        // reads — force completion or the map sees pre-copy contents.
        ctx->WaitForIdle();
    }

    MappedTextureSubresource mapped;
    ctx->MapTextureSubresource(staging, 0, 0,
            MAP_READ, MAP_FLAG_NONE, nullptr, mapped);
    if (!mapped.pData) {
        mvDumpDone = true;
        utils::warn("taa: motion dump failed (MapTextureSubresource)");
        return;
    }

    const u32 w = targetWidth, h = targetHeight;
    const u32 bpp = 4;  // RG16F: 2 halfs
    std::vector<u8> rgba((size_t)w * h * 4);
    const float kGain = 32.0f;  // signed: zero = mid-gray 128, ±1/32 NDC full scale
    for (u32 y = 0; y < h; y++) {
        const u8* src = (const u8*)mapped.pData + (size_t)y * mapped.Stride;
        u8* row = &rgba[(size_t)y * w * 4];
        for (u32 x = 0; x < w; x++) {
            const f32 mvx = f16tof32(*(const u16*)(src + (size_t)x * bpp));
            const f32 mvy = f16tof32(*(const u16*)(src + (size_t)x * bpp + 2));
            const f32 mag = std::sqrt(mvx * mvx + mvy * mvy);
            auto cl8 = [](float v) -> u8 {
                const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                return (u8)(c * 255.0f);
            };
            u8* dst = row + (size_t)x * 4;
            dst[0] = cl8(mvx * kGain * 0.5f + 0.5f);
            dst[1] = cl8(mvy * kGain * 0.5f + 0.5f);
            dst[2] = cl8(mag * kGain);
            dst[3] = 255;
        }
    }
    ctx->UnmapTextureSubresource(staging, 0, 0);

    mvDumpDone = true;
    if (!stbi_write_png(mvDumpPath, (int)w, (int)h, 4, rgba.data(), (int)(w * 4))) {
        utils::warn("taa: cannot save motion dump to %s", mvDumpPath);
    } else {
        utils::info("taa: motion vectors dumped to %s "
                "(SIGNED: R = mv.x, G = mv.y [F3NDC: +y = screen top], "
                "mid-gray 128 == 0, full scale == ±%.4f NDC; B = |mv| * 32); "
                "this frame's camera translation: %.4f m", mvDumpPath,
                1.0 / kGain, (double)lastDEyeMag);
    }
}

void taaWorldResolve(IDeviceContext* ctx, ITextureView* backRTV) {
    if (!sceneColorTex || !backRTV) {
        return;
    }

    ITextureView* srcColorSRV = sceneColorTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    bool ssaoRan = false;

    if (taaOn && postFXContext && taa && cameraCB) {
        const u32 curr = frameIdx & 1;
        const u32 prev = (frameIdx + 1) & 1;

        PostFXContext::RenderAttributes pa;
        pa.pDevice = device;
        pa.pDeviceContext = ctx;
        pa.pCameraAttribsCB = cameraCB;
        pa.pCurrDepthBufferSRV = depthTex[curr]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        pa.pPrevDepthBufferSRV = depthTex[prev]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        pa.pMotionVectorsSRV = motionTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        postFXContext->Execute(pa);

        if (ssaoReady() && ssaoOn()) {
            Diligent::ScopedDebugGroup ssaoGroup(ctx, "ssao");
            ssaoRan = ssaoExecute(ctx, taaDepthSRV(curr));
        }

        HLSL::TemporalAntiAliasingAttribs attribs{};
        attribs.TemporalStabilityFactor = taaWeight;
        attribs.ResetAccumulation = 0;  // frame-index continuity handles resets
        attribs.SkipRejection = 0;

        TemporalAntiAliasing::RenderAttributes ra;
        ra.pDevice = device;
        ra.pDeviceContext = ctx;
        ra.pPostFXContext = postFXContext.get();
        ra.pColorBufferSRV = srcColorSRV;
        ra.pTAAAttribs = &attribs;

        if (taa->Execute(ra) == POST_FX_EXECUTION_STATUS_READY) {
            srcColorSRV = taa->GetAccumulatedFrameSRV();
        }
    }

    if (!taaOn && postFXContext && cameraCB &&
            (ssaoReady() && ssaoOn())) {
        const u32 curr = frameIdx & 1;
        const u32 prev = (frameIdx + 1) & 1;

        PostFXContext::RenderAttributes pa;
        pa.pDevice = device;
        pa.pDeviceContext = ctx;
        pa.pCameraAttribsCB = cameraCB;
        pa.pCurrDepthBufferSRV = depthTex[curr]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        pa.pPrevDepthBufferSRV = depthTex[prev]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        pa.pMotionVectorsSRV = motionTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        postFXContext->Execute(pa);

        {
            Diligent::ScopedDebugGroup ssaoGroup(ctx, "ssao");
            ssaoRan = ssaoExecute(ctx, taaDepthSRV(curr));
        }
    }

    // ENGINE_TAA_DEBUG_MV: blit the raw motion buffer instead of the TAA
    // result (encoded mv+0.5; 128-ish sRGB ≈ zero motion). AFTER the TAA
    // block so the accumulated-frame result cannot overwrite the source.
    const bool debugMv = getenv("ENGINE_TAA_DEBUG_MV") != nullptr;
    if (debugMv) {
        srcColorSRV = motionTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    }

    // AO after the TAA accumulation, before the downsample/CAS/blit: the
    // composite is TARGET-sized 1:1, so the box downsample averages the
    // AO'd color exactly as it does the plain one. Skipped on the debug-MV
    // blit (the motion encoding must stay exact) and whenever SSAO did not
    // produce this frame's AO map (pending — the SSAO resolved texture is
    // then undefined, not a no-op AO=1).
    if (!debugMv && ssaoOn() && ssaoRan) {
        if (ITextureView* c = aoCompositeApply(ctx, srcColorSRV, ssaoAOSRV())) {
            srcColorSRV = c;
        }
    }

    if (!debugMv && bloomOn()) {
        Diligent::ScopedDebugGroup g(ctx, "bloom");
        if (bloomExecute(ctx, srcColorSRV)) {
            if (ITextureView* b = bloomSRV())
                srcColorSRV = b;
        }
    }

    // renderScale > 1: box-filter to a backbuffer-sized intermediate first —
    // the single-tap CAS/blit at backbuffer resolution would otherwise keep
    // 1 of every 4+ source texels and re-alias (the render-scale shimmer).
    const SwapChainDesc& scDesc = swapChain->GetDesc();
    if (!debugMv && targetWidth > scDesc.Width && targetHeight > scDesc.Height) {
        if (ITextureView* d = downsampleToIntermediate(ctx, srcColorSRV)) {
            srcColorSRV = d;
        }
    }

    // RCAS after the TAA resolve, before the sRGB encode-on-store — the
    // debug-MV blit bypasses it (the motion encoding must stay exact).
    if (!debugMv && casStrength > 0.0f) {
        if (!casToBackbuffer(ctx, srcColorSRV, backRTV)) {
            blitToBackbuffer(ctx, srcColorSRV, backRTV);
        }
    } else {
        blitToBackbuffer(ctx, srcColorSRV, backRTV);
    }

    // One-shot motion-buffer dump, after the TAA + blit are done reading the
    // offscreen chain so the CPU map cannot disturb GPU state.
    if (mvDumpPath && !mvDumpDone && frameIdx == mvDumpFrame) {
        taaDumpMotionVectors(ctx);
    }
}

}
