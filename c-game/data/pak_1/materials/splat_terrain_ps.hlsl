// Splat terrain pixel shader (diligent backend, runtime-compiled HLSL —
// the rmlui_ui_* pattern). Must match the VS output struct (shaders compile
// separately — no shared header on this path; keep the two in sync).
//
// Splat blend (SplatGroup chain from the Blender Splat_1 graph):
//     c = mix(c, alpha, 1.0 - w.a);
// over the group's 4 detail sets at the world-tiled detail uv (weights stay
// at the tile-local uv — see SPLAT_DETAIL_* below). The group weights are
// the UDIM layer (row * 10 + col, standard UDIM: file 1001 is the bottom-left
// tile, row 0 = the smallest-v band) of a 1024^2 x 100 TEXTURE2DARRAY;
// unshipped layers are the Noop weight (0,0,0,1) so the alpha detail
// (1 - w.a) leaves the base untouched there. The two groups chain over a
// black base (base normal: flat (0,0,1) in tangent space) in REVERSE
// splatInfo order: roads1 is the base group, grass1 the top — the loader
// stores groups in splatInfo order (grass1 first), so g_Weights0 /
// g_Detail[0..3] / g_DetailN[0..3] bind the first group and g_Weights1 /
// g_Detail[4..7] / g_DetailN[4..7] the last. Old-engine parity (terrain.frag):
// the painted chain is then blended OVER a base detail set (g_BaseAlbedo /
// g_BaseNormal, the first detail set of the first group) with
// splatInfluence = clamp(rawTotalWeight * 2, 0, 1), so Noop-weight regions
// (the weight data covers only ~17/100 UDIM tiles) fall back to the base
// texture instead of black. Albedo and normal maps run the
// SAME chain; normals blend in tangent space, then transform once to world.
//
// Lighting: full PBR parity with the GLTF_PBR_Renderer pass (RenderPBR.psh +
// PBR_Shading.fxh, DiligentFX sources) — Cook-Torrance GGX sun (the frame's
// directional light: Smith-GGX visibility * NdotH distribution * Schlick,
// energy-conserving diffuse (1 - F) * albedo / PI) with the CSM shadow
// receiver (3x3 fixed PCF, the Witness method, on the shared cascade depth
// atlas), plus IBL: diffuse irradiance cube + prefiltered spec env through
// the preintegrated GGX LUT with the Fdez-Aguera multiple-scattering terms,
// scaled by the frame's IBLScale. Same cbuffer the PBR pass fills
// (HLSL::PBRFrameAttribs mirrored flat below, matrices TRANSPOSED for this
// runtime-glslang convention, row-vector muls) — sun, cascade data and IBL
// params come out identical. Output is LINEAR HDR (the PBR pass skips tone
// mapping — the TAA chain grades later).
//
// Outputs: the world pass renders into the TAA offscreen chain (linear
// RGBA16F + RG16F motion + RGBA16F normal + D32) — write all three like the
// PBR pass (GltfDiligent.cpp GetPSMainSource footer) or TAA/SSAO misbehave:
// Color, CustomData = motion vectors (static terrain: zero), WorldNormal
// = float4(world normal, 1.0).

struct PSSplatIn
{
    float4 Position    : SV_Position;
    float3 AnchoredPos : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float4 Tangent     : TEXCOORD2;
    float2 UdimUv      : TEXCOORD3;
    float4 PrevClip    : TEXCOORD4;
    float  ViewZ       : TEXCOORD5;  // camera view-space depth (unjittered)
};

// Flat mirror of HLSL::PBRFrameAttribs (BasicStructures / PBR_Structures /
// RenderPBR_Structures.fxh, PBR_MAX_LIGHTS=1) + the splat anchor — the
// ShadowMaps block is NOT mirrored: the CSM cascade data arrives through the
// separate cbSplatShadow cbuffer below. Field order must stay byte-identical
// to the C++ staging struct (SplatTerrainDiligent.cpp). float4 groups are
// consecutive C++ scalars.
cbuffer cbSplatFrame
{
    float4   cCamPosition;    // camera world position (render/anchored space)
    float4   cCamViewport;
    float4   cCamClip;        // nearZ farZ nearDepth farDepth
    float4   cCamScene;       // scene near/far Z/depth
    float4   cCamHand;        // handness frameIndex pad pad
    float4   cCamFocus;       // focus fstop focalLength sensorW
    float4   cCamSensor;     // sensorH exposure jitter.x jitter.y
    float4x4 cCamView;
    float4x4 cCamProj;
    float4x4 cCamViewProj;
    float4x4 cCamViewInv;
    float4x4 cCamProjInv;
    float4x4 cCamViewProjInv;
    float4   cCamExtra[5];
    float4   pCamPosition;
    float4   pCamViewport;
    float4   pCamClip;
    float4   pCamScene;
    float4   pCamHand;
    float4   pCamFocus;
    float4   pCamSensor;
    float4x4 pCamView;
    float4x4 pCamProj;
    float4x4 pCamViewProj;
    float4x4 pCamViewInv;
    float4x4 pCamProjInv;
    float4x4 pCamViewProjInv;
    float4   pCamExtra[5];
    float4   rCam;            // AvgLogLum MiddleGray WhitePoint PrefilteredCubeLastMip
    float4   rIBLScale;
    float4   rEnvRot;         // EnvironmentRotation (cos, sin) + pad
    float4   rMisc;           // OcclusionStrength EmissionScale PointSize MipBias
    float4   rLight;          // LightCount Time DebugView pad
    float4   rUnshaded;
    float4   rHighlight;
    float4   rLoadA;
    float4   rLoadB;
    float4   rLoadC;
    float4   lTypePos;        // Light[0]: Type PosX PosY PosZ
    float4   lDir;            // DirectionX/Y/Z ShadowMapIndex (-1 = no shadow)
    float4   lInt;            // IntensityR/G/B Range4
    float4   lSpot;           // SpotAngleScale SpotAngleOffset pad pad
    float4   g_Anchor;
    // CSM shadow receive (merged into cbSplatFrame — on this runtime path a
    // second cbuffer in the same PSO bound ambiguously, lessons.md 2026-09-05):
    // byte-identical mirror of Diligent::ShadowMapAttribs (BasicStructures.fxh,
    // the non-C++ branch — f4CascadeCamSpaceZEnd as float4[]; the same bytes
    // shadowDiligentLightAttribs() carries in LightAttribs.ShadowAttribs) +
    // f4ShadowFade. Matrices TRANSPOSED (the runtime glslang convention,
    // row-vector muls). Fixed size, always present — shadow on/off is the
    // ShadowIndex gate + the g_ShadowMap SRV, never a cbuffer shape change.
    float4x4 mWorldToLightView;
    float4   cascadeAttribs[32];   // per cascade i: [4i]=f4LightSpaceScale [4i+1]=f4LightSpaceScaledBias [4i+2]=f4StartEndZ [4i+3]=f4MarginProjSpace
    float4   mWorldToShadowMapUVDepth[32];
    float4   f4CascadeCamSpaceZEnd[2];
    float4   f4ShadowMapDim;
    float4   sNumCascades;         // iNumCascades fNumCascades bVisualizeCascades bVisualizeShadowing
    float4   sBiasParams;          // fReceiverPlaneDepthBiasClamp fFixedDepthBias fCascadeTransitionRegion iMaxAnisotropy
    float4   sVSMParams;           // fVSMBias fVSMLightBleedingReduction fEVSMPositiveExponent fEVSMNegativeExponent
    // sTail: the C++ ShadowMapAttribs tail is a mix of int/BOOL and float.
    // Declare each with its real type so the memcpy'd bit patterns read back
    // correctly (a float4 mirror turns the int/BOOL fields into denormals).
    int      bIs32BitEVSM;         // sTail.x  (BOOL: 1 = 32-bit EVSM atlas)
    int      iFixedFilterSize;     // sTail.y  (int: poisson tap count)
    float    fFilterWorldSize;     // sTail.z  (float)
    float    fShadowDebugMode;     // sTail.w  (fDummy: raw f32 debug gate, 0 = off)
    float4   f4ShadowFade;         // x = tier shadow distance in m (0 = no fade)
};

Texture2DArray g_Weights0;   // splatInfo group 0 (grass1 — the top group)
Texture2DArray g_Weights1;   // splatInfo group 1 (roads1 — the base group)
Texture2D g_BaseAlbedo;       // base material albedo (tiled uv, under the splat chain)
Texture2D g_BaseNormal;       // base material normal  (tiled uv, under the splat chain)
Texture2D g_Detail0;         // group 0 red albedo
Texture2D g_Detail1;        // group 0 green albedo
Texture2D g_Detail2;        // group 0 blue albedo
Texture2D g_Detail3;        // group 0 alpha albedo
Texture2D g_Detail4;        // group 1 red albedo
Texture2D g_Detail5;        // group 1 green albedo
Texture2D g_Detail6;        // group 1 blue albedo
Texture2D g_Detail7;        // group 1 alpha albedo
Texture2D g_DetailN0;       // group 0 red normal
Texture2D g_DetailN1;       // group 0 green normal
Texture2D g_DetailN2;       // group 0 blue normal
Texture2D g_DetailN3;       // group 0 alpha normal
Texture2D g_DetailN4;       // group 1 red normal
Texture2D g_DetailN5;       // group 1 green normal
Texture2D g_DetailN6;       // group 1 blue normal
Texture2D g_DetailN7;       // group 1 alpha normal
Texture2D g_SnowAlbedo;
Texture2D g_SnowNormal;
Texture2D g_SandAlbedo;
Texture2D g_SandNormal;
Texture2D g_CliffAlbedo;
Texture2D g_CliffNormal;
TextureCube g_IrradianceMap;
TextureCube g_PrefilteredEnvMap;
Texture2D g_PreintegratedGGX;
Texture2DArray g_ShadowMap;
Texture2DArray g_ShadowMapLinear;
SamplerState g_Sampler;
SamplerState g_DetailSampler;
SamplerState g_LinearClampSampler;
SamplerComparisonState g_ShadowMap_sampler;
SamplerState g_ShadowMapLinearSampler;

struct PSOutput
{
    float4 Color        : SV_Target0;
    float2 MotionVector : SV_Target1;
    float4 WorldNormal  : SV_Target2;
};

const float SPLAT_PI        = 3.14159265359;
const float SPLAT_ROUGHNESS = 0.9;  // dielectric outdoor terrain

// Detail tiling, old-engine parity (terrain.frag): a fixed reference span,
// NOT the terrain AABB, so the pattern stays anchored across world rebuilds
// — tiledUV spans one 1024-px detail repeat per 7000 reference meters
// (~6.84 m/repeat). ENGINE_SPLAT_DETAIL_METERS (SplatTerrainDiligent.cpp,
// prepended #define) overrides the span for tuning without a repak.
#define SPLAT_DETAIL_TILE 1024.0
#ifndef SPLAT_DETAIL_METERS
#define SPLAT_DETAIL_METERS 7000.0
#endif

#ifndef SPLAT_SAND_LO
#define SPLAT_SAND_LO -1.5
#endif
#ifndef SPLAT_SAND_HI
#define SPLAT_SAND_HI 4.0
#endif
#ifndef SPLAT_CLIFF_LO
#define SPLAT_CLIFF_LO 0.1
#endif
#ifndef SPLAT_CLIFF_HI
#define SPLAT_CLIFF_HI 0.4
#endif
#ifndef SPLAT_CLIFF_METERS
#define SPLAT_CLIFF_METERS 128.0
#endif
#ifndef SPLAT_SNOW_LO
#define SPLAT_SNOW_LO 800.0
#endif
#ifndef SPLAT_SNOW_HI
#define SPLAT_SNOW_HI 1100.0
#endif

float dotSat(float3 x, float3 y) { return max(dot(x, y), 0.0); }

// The SplatGroup chain for one group, written as a + (b - a) * t — this
// glslang HLSL build does not resolve mix(float3, float3, float) (the rmlui
// shaders avoid it), and the expanded form is the identical linear blend.
// Albedo samples the sRGB textures raw (decode on sample); the base is the
// lower group's output (black for the base group). The details sample
// through the REPEAT/aniso g_DetailSampler at the world-tiled uv — the
// WEIGHTS stay on g_Sampler at the tile-local uv (clamp keeps UDIM tiles
// from bleeding into each other).
//
// The details use EXPLICIT gradients (SampleGrad), not implicit LOD: the
// tiled uv magnitude is ~±500..19000 (absolute world meters × scale), and
// the sampler's implicit quad gradient overflows/saturates at that
// magnitude on this hardware — every implicit-lod detail sample returned
// the 1x1 LAST mip (the texture average, uv-independent — verified live:
// 6.84 m vs 200 m scales rendered byte-identical until the switch). ddx/ddy
// themselves are correct here (measured ~0.2/px), so passing them explicitly
// restores real mip selection. Old-engine parity: terrain.frag fed the tiled
// uv through sampleMaterialTextureGrad(dUVdx, dUVdy) for the same reason.
// (Aniso filtering needs the hardware quad, so it may not engage on
// explicit-grad samples — mips still filter; acceptable per the fallback.)
float3 splatAlbedoMix(float3 base, float4 w,
        Texture2D red, Texture2D green, Texture2D blue, Texture2D alpha,
        SamplerState detailSampler, float2 uv, float2 du, float2 dv)
{
    float3 c = base;
    c = c + (red.SampleGrad(detailSampler, uv, du, dv).rgb - c) * w.r;
    c = c + (green.SampleGrad(detailSampler, uv, du, dv).rgb - c) * w.g;
    c = c + (blue.SampleGrad(detailSampler, uv, du, dv).rgb - c) * w.b;
    c = c + (alpha.SampleGrad(detailSampler, uv, du, dv).rgb - c) * (1.0 - w.a);
    return c;
}

// Same chain over the tangent-space normals (decode 2*s - 1 first; the
// base-group base is the flat tangent normal (0, 0, 1) — the Noop weight
// (0,0,0,1) keeps it untouched, matching the albedo chain's black base).
float3 splatNormalMix(float3 base, float4 w,
        Texture2D red, Texture2D green, Texture2D blue, Texture2D alpha,
        SamplerState detailSampler, float2 uv, float2 du, float2 dv)
{
    float3 c = base;
    c = c + (red.SampleGrad(detailSampler, uv, du, dv).xyz * 2.0 - 1.0 - c) * w.r;
    c = c + (green.SampleGrad(detailSampler, uv, du, dv).xyz * 2.0 - 1.0 - c) * w.g;
    c = c + (blue.SampleGrad(detailSampler, uv, du, dv).xyz * 2.0 - 1.0 - c) * w.b;
    c = c + (alpha.SampleGrad(detailSampler, uv, du, dv).xyz * 2.0 - 1.0 - c) * (1.0 - w.a);
    return c;
}

float3 triplanarSample(Texture2D tex, SamplerState smp, float3 pos, float3 w)
{
    float3 c = float3(0.0, 0.0, 0.0);
    c = c + tex.Sample(smp, pos.zy).rgb * w.x;
    c = c + tex.Sample(smp, pos.xz).rgb * w.y;
    c = c + tex.Sample(smp, pos.xy).rgb * w.z;
    return c;
}

// ── PBR lighting (ported from DiligentFX PBR_Shading.fxh / PCF.fxh) ────────

float ggxNormalDistribution(float NdotH, float AlphaRoughness)
{
    AlphaRoughness = max(AlphaRoughness, 1e-3);
    float a2  = AlphaRoughness * AlphaRoughness;
    float nh2 = NdotH * NdotH;
    float f   = nh2 * a2 + (1.0 - nh2);
    return a2 / max(SPLAT_PI * f * f, 1e-9);
}

float ggxVisibilityCorrelated(float NdotL, float NdotV, float AlphaRoughness)
{
    float a2 = AlphaRoughness * AlphaRoughness;
    float GGXV = NdotL * sqrt(max(NdotV * NdotV * (1.0 - a2) + a2, 1e-7));
    float GGXL = NdotV * sqrt(max(NdotL * NdotL * (1.0 - a2) + a2, 1e-7));
    return 0.5 / (GGXV + GGXL);
}

float3 schlickReflection(float VdotH, float3 Reflectance0, float3 Reflectance90)
{
    return Reflectance0 + (Reflectance90 - Reflectance0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

// The Witness 3x3 fixed PCF (PCF.fxh FilterShadowMapFixedPCF,
// PCF_FILTER_SIZE 3) over the cascade depth array.
float filterShadowPCF3(float2 uv, float slice, float lightDepth)
{
    float4 dims;
    g_ShadowMap.GetDimensions(dims.x, dims.y, dims.z);
    float2 px     = uv * dims.xy;
    float2 baseUV = floor(px + 0.5) - 0.5;
    float  s      = (px.x + 0.5) - (baseUV.x + 0.5);
    float  t      = (px.y + 0.5) - (baseUV.y + 0.5);
    float2 texel  = 1.0 / dims.xy;
    baseUV *= texel;

    float uw0 = (3.0 - 2.0 * s);
    float uw1 = (1.0 + 2.0 * s);
    float u0  = (2.0 - s) / uw0 - 1.0;
    float u1  = s / uw1 + 1.0;
    float vw0 = (3.0 - 2.0 * t);
    float vw1 = (1.0 + 2.0 * t);
    float v0  = (2.0 - t) / vw0 - 1.0;
    float v1  = t / vw1 + 1.0;

    const float DepthClamp = 1e-8;
    float sum = 0.0;
    sum += uw0 * vw0 * g_ShadowMap.SampleCmpLevelZero(g_ShadowMap_sampler, float3(baseUV + float2(u0, v0) * texel, slice), max(lightDepth, DepthClamp));
    sum += uw1 * vw0 * g_ShadowMap.SampleCmpLevelZero(g_ShadowMap_sampler, float3(baseUV + float2(u1, v0) * texel, slice), max(lightDepth, DepthClamp));
    sum += uw0 * vw1 * g_ShadowMap.SampleCmpLevelZero(g_ShadowMap_sampler, float3(baseUV + float2(u0, v1) * texel, slice), max(lightDepth, DepthClamp));
    sum += uw1 * vw1 * g_ShadowMap.SampleCmpLevelZero(g_ShadowMap_sampler, float3(baseUV + float2(u1, v1) * texel, slice), max(lightDepth, DepthClamp));
    return sum / 16.0;
}

// The VSM (mode 2) receive: the filterable atlas stores (R = mean depth,
// G = mean depth^2) pre-filtered over the tier's radius (ShadowConversions.fx
// VSMHorzPS/VertBlurPS). Per tap the one-tailed Chebyshev bound (Shadows.fxh
// ChebyshevUpperBound, VSM's fMinVariance floor = fVSMBias, the cbuffer's
// sVSMParams.x): in front of the occluder mean -> lit, behind ->
// Var / (Var + d^2), which falls off over ~2 sqrt(Var) and needs no separate
// depth bias (fFixedDepthBias would double-bias and peter-pan). 8 Poisson
// taps (unit radius) around the texel, averaged like the PCF3 kernel; the
// clamp keeps the taps inside the cascade tile.
float filterShadowVSM(float2 uv, float slice, float lightDepth)
{
    float4 dims;
    g_ShadowMapLinear.GetDimensions(dims.x, dims.y, dims.z);
    float2 px     = uv * dims.xy;
    float2 baseUV = floor(px + 0.5) - 0.5;
    float  s      = (px.x + 0.5) - (baseUV.x + 0.5);
    float  t      = (px.y + 0.5) - (baseUV.y + 0.5);
    float2 texel  = 1.0 / dims.xy;

    const float2 poisson[8] =
    {
        float2( 0.9362,  0.5513),
        float2(-0.4554,  0.9537),
        float2( 0.6568, -0.8001),
        float2( 0.8798, -0.5202),
        float2(-0.8181,  0.2479),
        float2(-0.1008, -0.8778),
        float2(-0.2477,  0.3971),
        float2( 0.8349,  0.4302)
    };

    lightDepth = max(lightDepth, 0.0);
    float sum = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float2 tUV = (baseUV + float2(s, t) + poisson[i]) * texel;
        tUV        = clamp(tUV, 0.0, 1.0);
        float2 m   = g_ShadowMapLinear.Sample(g_ShadowMapLinearSampler, float3(tUV, slice)).xy;
        float  variance = max(m.y - m.x * m.x, sVSMParams.x);
        float  d        = lightDepth - m.x;
        float  p        = (d < 0.0) ? 1.0 : min(variance / (variance + d * d), 1.0);
        if (sVSMParams.y > 0.0)
            p = saturate((p - sVSMParams.y) / (1.0 - sVSMParams.y));
        sum += p;
    }
    return sum / 8.0;
}

// EVSM (modes 3/4) receive. The filterable atlas stores warped-depth moments
// (ShadowConversions.fx EVSMHorzPS): R,G = (mean w1, mean w1^2) with
// w1 = +exp(+posExp * (2d - 1)) and, for EVSM4 only (RGBA32; the EVSM2 atlas is
// RG32 so .zw read back as 0), B,A = (mean w2, mean w2^2) with
// w2 = -exp(-negExp * (2d - 1)). The receiver un-warps its RAW cascade z with
// the same mirrored exponents (Shadows.fxh WarpDepthEVSM, clamped to 42 for
// 32-bit per GetEVSMExponents) and runs the same one-tailed Chebyshev test in
// the warped domain — but the min-variance floor is per-dimension and scales
// with the warp itself: (fVSMBias * exponent * warpedDepth)^2 (Shadows.fxh
// SampleEVSM), which is the mode's clamp offset (the exponential warp is the
// depth bias; no fFixedDepthBias). EVSM2 tests only the positive dimension;
// EVSM4 additionally tests the negative one and takes the min of both.
float chebyshevUpperBound(float2 moments, float mean, float minVariance)
{
    float variance = max(moments.y - moments.x * moments.x, minVariance);
    float d        = mean - moments.x;
    float pMax     = variance / (variance + d * d);
    if (sVSMParams.y > 0.0)
        pMax = saturate((pMax - sVSMParams.y) / (1.0 - sVSMParams.y));
    return (mean <= moments.x) ? 1.0 : min(pMax, 1.0);
}

float2 warpDepthEVSM(float depth)
{
    // bIs32BitEVSM is a BOOL (int) in the C++ ShadowMapAttribs (bit 0x1 = true);
    // the mirror must read it as an int, not a float (float read = denormal ~0).
    float  maxExp = (bIs32BitEVSM > 0) ? 42.0 : 5.54;
    float2 ex     = min(sVSMParams.zw, float2(maxExp, maxExp));
    float  d      = 2.0 * depth - 1.0;
    return float2(exp(ex.x * d), -exp(-ex.y * d));
}

float filterShadowEVSM(float2 uv, float slice, float lightDepth, bool evsm4)
{
    float4 dims;
    g_ShadowMapLinear.GetDimensions(dims.x, dims.y, dims.z);
    float2 px     = uv * dims.xy;
    float2 baseUV = floor(px + 0.5) - 0.5;
    float  s      = (px.x + 0.5) - (baseUV.x + 0.5);
    float  t      = (px.y + 0.5) - (baseUV.y + 0.5);
    float2 texel  = 1.0 / dims.xy;

    const float2 poisson[8] =
    {
        float2( 0.9362,  0.5513),
        float2(-0.4554,  0.9537),
        float2( 0.6568, -0.8001),
        float2( 0.8798, -0.5202),
        float2(-0.8181,  0.2479),
        float2(-0.1008, -0.8778),
        float2(-0.2477,  0.3971),
        float2( 0.8349,  0.4302)
    };

    lightDepth = max(lightDepth, 0.0);
    float2 w      = warpDepthEVSM(lightDepth);
    float2 minVar = sVSMParams.x * sVSMParams.zw * w;

    float sum = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float2 tUV = (baseUV + float2(s, t) + poisson[i]) * texel;
        tUV        = clamp(tUV, 0.0, 1.0);
        float4 m  = g_ShadowMapLinear.Sample(g_ShadowMapLinearSampler, float3(tUV, slice));
        float  p  = chebyshevUpperBound(m.xy, w.x, minVar.x * minVar.x);
        if (evsm4)
            p = min(p, chebyshevUpperBound(m.zw, w.y, minVar.y * minVar.y));
        sum += p;
    }
    return sum / 8.0;
}

float3 rotateAroundY(float3 d, float2 rot)
{
    return float3(rot.x * d.x + rot.y * d.z, d.y, -rot.y * d.x + rot.x * d.z);
}

PSOutput main(PSSplatIn In)
{
    // Tile pick: the floor is clamped to 0..9 (chunk-boundary uvs overshoot
    // the grid by ~0.002), and the local uv comes from the UNclamped value —
    // a pre-clamp would snap the overshoot to the tile's left edge. The row
    // axis of the stored UDIM grid runs top-to-bottom, so the row index is
    // mirrored (keeps the tile-local uv upright — only the row pick flips).
    float2 tile  = clamp(floor(In.UdimUv), 0.0, 9.0);
    float2 local = clamp(In.UdimUv - tile, 0.0, 1.0);
    tile.y       = 9.0 - tile.y;
    float layer  = tile.y * 10.0 + tile.x;

    // World-space detail uv: AnchoredPos is world - g_Anchor (the VS), so the
    // absolute xz needs only the anchor add-back. Fixed reference-span tiling
    // (NOT the AABB) keeps the pattern anchored across world rebuilds.
    float2 worldXZ = In.AnchoredPos.xz + g_Anchor.xz;
    float2 tiledUV = worldXZ * (SPLAT_DETAIL_TILE / SPLAT_DETAIL_METERS);
    float2 du = ddx(tiledUV);
    float2 dv = ddy(tiledUV);

    float4 wBase = g_Weights1.Sample(g_Sampler, float3(local, layer));
    float4 wTop = g_Weights0.Sample(g_Sampler, float3(local, layer));

    // Base material under the splat chain (terrain.frag parity): the paint is
    // blended over the base detail set at the same tiled uv + explicit grads,
    // scaled by the raw total weight so Noop regions (0,0,0,1) stay base.
    float  wSum = (wBase.r + wBase.g + wBase.b + (1.0 - wBase.a)) +
                 (wTop.r + wTop.g + wTop.b + (1.0 - wTop.a));
    float  influence = clamp(wSum * 2.0, 0.0, 1.0);
    float3 baseAlbedo = g_BaseAlbedo.SampleGrad(g_DetailSampler, tiledUV, du, dv).rgb;
    float3 baseN      = g_BaseNormal.SampleGrad(g_DetailSampler, tiledUV, du, dv).xyz * 2.0 - 1.0;

    float3 albedo = baseAlbedo;
    float3 nT     = baseN;
    if (influence > 0.0)
    {
        float3 splatAlbedo = splatAlbedoMix(float3(0.0, 0.0, 0.0), wBase,
                g_Detail4, g_Detail5, g_Detail6, g_Detail7, g_DetailSampler, tiledUV, du, dv);
        splatAlbedo = splatAlbedoMix(splatAlbedo, wTop, g_Detail0, g_Detail1, g_Detail2, g_Detail3,
                g_DetailSampler, tiledUV, du, dv);
        float3 splatN = splatNormalMix(float3(0.0, 0.0, 1.0), wBase,
                g_DetailN4, g_DetailN5, g_DetailN6, g_DetailN7, g_DetailSampler, tiledUV, du, dv);
        splatN = splatNormalMix(splatN, wTop, g_DetailN0, g_DetailN1, g_DetailN2, g_DetailN3,
                g_DetailSampler, tiledUV, du, dv);
        albedo = baseAlbedo + (splatAlbedo - baseAlbedo) * influence;
        nT     = baseN + (splatN - baseN) * influence;
    }

    float worldY = In.AnchoredPos.y + g_Anchor.y;
    float slope  = 1.0 - max(In.WorldNormal.y, 0.0);
    float wSand  = smoothstep(SPLAT_SAND_LO, SPLAT_SAND_HI, worldY) *
                   (1.0 - smoothstep(0.25 * SPLAT_SAND_HI, SPLAT_SAND_HI, worldY));
    float wCliff = smoothstep(SPLAT_CLIFF_LO, SPLAT_CLIFF_HI, slope);
    float wSnow  = smoothstep(SPLAT_SNOW_LO, SPLAT_SNOW_HI, worldY);

    float3 sandAlbedo = g_SandAlbedo.SampleGrad(g_DetailSampler, tiledUV, du, dv).rgb;
    albedo = albedo + (sandAlbedo - albedo) * wSand;
    float3 wTri   = pow(abs(normalize(In.WorldNormal)), 4.0);
    wTri         = wTri / (wTri.x + wTri.y + wTri.z + 1e-6);
    float3 cliffUV = (In.AnchoredPos + g_Anchor) * (1.0 / SPLAT_CLIFF_METERS);
    float3 cliffAlbedo = triplanarSample(g_CliffAlbedo, g_DetailSampler, cliffUV, wTri);
    albedo = albedo + (cliffAlbedo - albedo) * wCliff;
    float3 snowAlbedo = g_SnowAlbedo.SampleGrad(g_DetailSampler, tiledUV, du, dv).rgb;
    albedo = albedo + (snowAlbedo - albedo) * wSnow;

    float3 sandN = g_SandNormal.SampleGrad(g_DetailSampler, tiledUV, du, dv).xyz * 2.0 - 1.0;
    nT = nT + (sandN - nT) * wSand;
    float3 cliffN = triplanarSample(g_CliffNormal, g_DetailSampler, cliffUV, wTri) * 2.0 - 1.0;
    nT = nT + (cliffN - nT) * wCliff;
    float3 snowN = g_SnowNormal.SampleGrad(g_DetailSampler, tiledUV, du, dv).xyz * 2.0 - 1.0;
    nT = nT + (snowN - nT) * wSnow;

    // Tangent frame (the glTF TANGENT attribute: xyz + handedness), then the
    // perturbed world normal.
    float3 N0 = normalize(In.WorldNormal);
    float3 T  = normalize(In.Tangent.xyz);
    float3 B  = cross(N0, T) * In.Tangent.w;
    float3 N  = normalize(T * nT.x + B * nT.y + N0 * nT.z);

    float3 V = normalize(cCamPosition.xyz - In.AnchoredPos);
    float  NdotV = dotSat(N, V);

    // ── IBL (ApplyIBL + GetLambertianIBL/GetSpecularIBL_GGX, PBR path) ──
    float  roughness = SPLAT_ROUGHNESS;
    float2 brdf = g_PreintegratedGGX.Sample(g_LinearClampSampler, float2(NdotV, roughness)).rg;
    float3 R0   = float3(0.04, 0.04, 0.04);
    float3 R90  = float3(1.0, 1.0, 1.0);  // clamp(MaxR0 * 50, 0, 1), dielectric
    float3 kS   = schlickReflection(NdotV, R0, max(float3(1.0 - roughness), R0));
    float3 FssEss = kS * brdf.x + brdf.y;
    float3 Ess    = brdf.x + brdf.y;
    float3 Ems    = 1.0 - Ess;
    float3 Favg   = R0 + (1.0 - R0) / 21.0;
    float3 Fms    = FssEss * Favg / (1.0 - Ems * Favg);
    float3 Edss   = 1.0 - (FssEss + Fms * Ems);
    float3 DiffuseColor = albedo * (1.0 - R0);  // metallic = 0

    float3 Irradiance = g_IrradianceMap.Sample(g_LinearClampSampler, rotateAroundY(N, rEnvRot.xy)).rgb;
    float3 DiffuseIBL = (Fms * Ems + DiffuseColor * Edss) * Irradiance;

    float3 Lrefl = normalize(reflect(-V, N));
    float  lod   = roughness * rCam.w;
    float3 SpecLight = g_PrefilteredEnvMap.SampleLevel(g_LinearClampSampler, rotateAroundY(Lrefl, rEnvRot.xy), lod).rgb;
    float3 SpecularIBL = SpecLight * FssEss;

    float3 IBL = (DiffuseIBL + SpecularIBL) * rIBLScale.xyz;

    // ── Direct sun (ApplyPunctualLight, PBR path — the frame's light 0) ──
    float3 LightDir    = lDir.xyz;       // travel direction
    float  ShadowIndex = lDir.w;
    float3 LightIntensity = lInt.xyz;
    float  shadowMode = f4ShadowFade.y;
    float  Attenuation = 1.0;
    int    dbgCascade = -2;
    float2 dbgUV      = float2(0.0, 0.0);
    float  dbgRaw     = -1.0;
    if (ShadowIndex >= 0.0)
    {
        // Per-pixel cascade pick (Shadows.fxh FindCascade, non-best search):
        // the camera view-space depth selects the cascade whose z range covers
        // this pixel — the terrain spans the whole shadow distance, so the
        // single-cascade CPU pick (the PBR player path) is wrong for far
        // terrain. Unjittered view z (rotation-only view) keeps the pick
        // TAA-stable. Unused slots hold +FLT_MAX, so counting all 2 float4s
        // is exact for any cascade count up to 8.
        float viewZ   = In.ViewZ;
        int cascade   = 0;
        for (int i = 0; i < 2; ++i)
        {
            float4 zEnd = f4CascadeCamSpaceZEnd[i];
            cascade += int(zEnd.x < viewZ) + int(zEnd.y < viewZ) + int(zEnd.z < viewZ) + int(zEnd.w < viewZ);
        }
        cascade = min(cascade, int(sNumCascades.y) - 1);
        if (cascade >= 0)
        {
            // Light view space (the caster's untransposed W2LView, stored
            // transposed here), then the picked cascade's scale/bias to its
            // normalized depth (the same affine the caster's
            // GetCascadeTransform projection applies — no /w: the cascade
            // projection is orthographic, w == 1).
            float3 lightViewPos = mul(float4(In.AnchoredPos, 1.0), mWorldToLightView).xyz;
            float3 cascadeNdc   = lightViewPos * cascadeAttribs[cascade * 4].xyz + cascadeAttribs[cascade * 4 + 1].xyz;
            float2 cascadeUV    = float2(0.5, 0.5) + float2(0.5, -0.5) * cascadeNdc.xy;
            float  LightDepth    = cascadeNdc.z - sBiasParams.y;  // fFixedDepthBias (cascade-z-normalized)
            dbgCascade           = cascade;
            dbgUV                = cascadeUV;
            // The shadow sampler clamps, so a receiver outside the picked
            // cascade's box would compare against unrelated atlas-edge depth
            // and flicker fully shadowed. Out-of-box pixels stay lit instead
            // of sampling — the tier fade covers the box/fade boundary.
            if (cascadeUV.x >= 0.0 && cascadeUV.x <= 1.0 &&
                cascadeUV.y >= 0.0 && cascadeUV.y <= 1.0)
            {
                if (shadowMode < 1.5)
                {
                    Attenuation = filterShadowPCF3(cascadeUV, float(cascade), LightDepth);
                    dbgRaw      = g_ShadowMap.SampleCmpLevelZero(g_ShadowMap_sampler, float3(cascadeUV, float(cascade)), max(LightDepth, 1e-8));
                }
                else if (shadowMode < 2.5)
                {
                    // Raw cascade z (no fFixedDepthBias — the variance floor is
                    // this branch's bias; LightDepth carries the PCF bias).
                    Attenuation = filterShadowVSM(cascadeUV, float(cascade), cascadeNdc.z);
                }
                else if (shadowMode < 3.5)
                {
                    Attenuation = filterShadowEVSM(cascadeUV, float(cascade), cascadeNdc.z, false);
                }
                else
                {
                    Attenuation = filterShadowEVSM(cascadeUV, float(cascade), cascadeNdc.z, true);
                }
            }
            // Receiver-side tier-distance fade (lessons.md 2026-09-07): the
            // padded light cube still samples past the tier distance, so fade
            // the shadow to lit over the last 25 % of it — the effective
            // cutoff (f4ShadowFade.x = 0 disables: ENGINE_SHADOW_FADE=0 A/B).
            float tier = f4ShadowFade.x;
            if (tier > 0.0)
            {
                float fade = clamp((tier - viewZ) / (0.25 * tier), 0.0, 1.0);
                Attenuation += (1.0 - Attenuation) * (1.0 - fade);
            }
        }
    }
    if (Attenuation > 0.0)
    {
        float3 L = -LightDir;
        float NdotL = dotSat(N, L);
        float3 Punctual = float3(0.0, 0.0, 0.0);
        if (NdotL > 0.0 || NdotV > 0.0)
        {
            float3 H = normalize(L + V);
            float  NdotH = max(dot(N, H), 0.0);
            float  VdotH = max(dot(V, H), 0.0);
            float  AlphaRoughness = roughness * roughness;
            float  D   = ggxNormalDistribution(NdotH, AlphaRoughness);
            float  Vis = ggxVisibilityCorrelated(NdotL, NdotV, AlphaRoughness);
            float3 F   = schlickReflection(VdotH, R0, R90);
            float3 DiffuseContrib = (1.0 - F) * (DiffuseColor / SPLAT_PI);
            float3 SpecContrib    = F * Vis * D;
            Punctual = (DiffuseContrib + SpecContrib) * NdotL * (LightIntensity * Attenuation);
        }
        IBL += Punctual;
    }

    if (fShadowDebugMode > 0.5)
    {
        if (ShadowIndex < 0.0)
            IBL = float3(0.1, 0.1, 0.5);
        else if (fShadowDebugMode < 1.5)
            IBL = (dbgCascade == 0) ? float3(1.0, 0.0, 0.0)
                : (dbgCascade == 1) ? float3(0.0, 1.0, 0.0)
                : float3(0.0, 0.3, 1.0);
        else if (fShadowDebugMode < 2.5)
            IBL = float3(Attenuation, Attenuation, 0.0);
        else if (fShadowDebugMode < 3.5)
            IBL = (dbgRaw < 0.0) ? float3(1.0, 0.0, 1.0) : float3(1.0 - dbgRaw, dbgRaw, 0.0);
        else
            IBL = float3(frac(dbgUV), 0.0);
    }

    PSOutput Out;
    Out.Color = float4(IBL, 1.0);
    float2 ndcCurr = In.Position.xy / In.Position.w;
    float2 ndcPrev = In.PrevClip.xy / max(In.PrevClip.w, 1e-6);
    Out.MotionVector = (ndcCurr - cCamSensor.zw) - (ndcPrev - pCamSensor.zw);
    Out.WorldNormal = float4(N, 1.0);
    return Out;
}
