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
    float4   f4ShadowFade;         // x = tier shadow distance in m, y = mode, z = EVSM far-pad scale (1.0 = none)
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
SamplerState g_HeightSampler;
SamplerState g_LinearClampSampler;
SamplerComparisonState g_ShadowMap_sampler;
SamplerState g_ShadowMapLinearSampler;
SamplerState g_ShadowMapNearestSampler;

struct PSOutput
{
    float4 Color        : SV_Target0;
    float2 MotionVector : SV_Target1;
    float4 WorldNormal  : SV_Target2;
};

const float SPLAT_PI        = 3.14159265359;

// Old-engine terrain.frag parity: tangent-normal xy strength, and roughness
// comes from the albedo alpha (bake: albedo.a = roughness), AO from the
// normal blue (bake: normal.b = AO, .a = height).
#define SPLAT_NORMAL_STRENGTH 2.0

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

// ── Parallax occlusion mapping (ported from the old engine's pom.shader —
// pak_0_engine/shaders/includes/pom.shader, exact parity) ── the heightfield
// lives in the ALPHA channel of each detail's normal map (the bake puts the
// 8-bit height there; grass/cliff/sand carry a real histogram, snow's normal
// is 3-component flat so its band keeps the blended detail field — real
// terrain under snow). SPLAT_POM_DEPTH is the relief depth in world meters
// (old POM_DEPTH_SPLAT), converted to tiled-uv units at the call site
// (× tile / span, matching old hScaleSplat). The march samples a 5-tap
// cross-blurred height (old samplePOMHeightBlurred: 8-bit heights + TAA
// jitter otherwise make the intersection jump between frames), and the
// view ray is transformed by the surface-aligned TBN (old buildTerrainTBN),
// so relief leans correctly on slopes instead of assuming a flat Y-up uv
// frame. Fade ramp and step counts are the old engine's 20/30 m and 6–8.
#ifndef SPLAT_POM
#define SPLAT_POM 1
#endif
#ifndef SPLAT_POM_DEPTH
#define SPLAT_POM_DEPTH 0.3
#endif
#ifndef SPLAT_POM_FADE_START
#define SPLAT_POM_FADE_START 20.0
#endif
#ifndef SPLAT_POM_FADE_END
#define SPLAT_POM_FADE_END 30.0
#endif
#ifndef SPLAT_POM_MIN_STEPS
#define SPLAT_POM_MIN_STEPS 6
#endif
#ifndef SPLAT_POM_MAX_STEPS
#define SPLAT_POM_MAX_STEPS 8
#endif
#ifndef SPLAT_POM_BINARY_STEPS
#define SPLAT_POM_BINARY_STEPS 4
#endif
#ifndef SPLAT_POM_BLUR
#define SPLAT_POM_BLUR 0
#endif

float dotSat(float3 x, float3 y) { return max(dot(x, y), 0.0); }

// The SplatGroup chain for one group, written as a + (b - a) * t — this
// glslang HLSL build does not resolve mix(float3, float3, float) (the rmlui
// shaders avoid it), and the expanded form is the identical linear blend.
// Albedo samples the sRGB textures raw (decode on sample) and carries the
// ALPHA through as roughness (old layerRoughness = albedoSample.a) — one
// fetch per detail serves both. The details sample
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
void splatAlbedoMix(inout float3 c, inout float rough, float4 w,
        Texture2D red, Texture2D green, Texture2D blue, Texture2D alpha,
        SamplerState detailSampler, float2 uv, float2 du, float2 dv)
{
    float4 s = red.SampleGrad(detailSampler, uv, du, dv);
    c = c + (s.rgb - c) * w.r;
    rough = rough + (s.a - rough) * w.r;
    s = green.SampleGrad(detailSampler, uv, du, dv);
    c = c + (s.rgb - c) * w.g;
    rough = rough + (s.a - rough) * w.g;
    s = blue.SampleGrad(detailSampler, uv, du, dv);
    c = c + (s.rgb - c) * w.b;
    rough = rough + (s.a - rough) * w.b;
    s = alpha.SampleGrad(detailSampler, uv, du, dv);
    c = c + (s.rgb - c) * (1.0 - w.a);
    rough = rough + (s.a - rough) * (1.0 - w.a);
}

// Packed-normal decode (old engine terrain.frag): RG = tangent xy at
// SPLAT_NORMAL_STRENGTH, Z reconstructed from the constraint, B carries AO,
// A height. The .xyz*2-1 decode this shader used before packed the AO byte
// into normal-Z (verified against the files: B spans 0.44..1.0 with xy
// within ±0.45 — impossible for a reconstructed z).
float3 splatNormalDecode(float2 rg)
{
    float2 nxy = (rg * 2.0 - 1.0) * SPLAT_NORMAL_STRENGTH;
    return float3(nxy, sqrt(max(1.0 - dot(nxy, nxy), 0.0)));
}

// Same chain over the tangent-space normals (decode first; the
// base-group base is the flat tangent normal (0, 0, 1) — the Noop weight
// (0,0,0,1) keeps it untouched, matching the albedo chain's black base),
// carrying the normal map's BLUE through as AO (old layerAo = normalSample.b)
// off the same fetch.
void splatNormalMix(inout float3 c, inout float ao, float4 w,
        Texture2D red, Texture2D green, Texture2D blue, Texture2D alpha,
        SamplerState detailSampler, float2 uv, float2 du, float2 dv)
{
    float4 s = red.SampleGrad(detailSampler, uv, du, dv);
    float3 n = splatNormalDecode(s.rg);
    c = c + (n - c) * w.r;
    ao = ao + (s.b - ao) * w.r;
    s = green.SampleGrad(detailSampler, uv, du, dv);
    n = splatNormalDecode(s.rg);
    c = c + (n - c) * w.g;
    ao = ao + (s.b - ao) * w.g;
    s = blue.SampleGrad(detailSampler, uv, du, dv);
    n = splatNormalDecode(s.rg);
    c = c + (n - c) * w.b;
    ao = ao + (s.b - ao) * w.b;
    s = alpha.SampleGrad(detailSampler, uv, du, dv);
    n = splatNormalDecode(s.rg);
    c = c + (n - c) * (1.0 - w.a);
    ao = ao + (s.b - ao) * (1.0 - w.a);
}

// ── POM height field ── the same chain as the albedo/normal mixes but over
// the normal maps' ALPHA (the baked height): chain base is the 0.5 surface
// level, the base material's own height folds back with the splat influence
// exactly like baseAlbedo/baseN. One blended field serves ALL chains — a
// dominant-material single-texture march would need dynamically selected
// texture handles (unsupported on this runtime-glslang path), and
// per-material offsets would tear the chains apart at weight boundaries.
float splatHeightMix(float base, float4 w,
        Texture2D red, Texture2D green, Texture2D blue, Texture2D alpha,
        SamplerState detailSampler, float2 uv, float2 du, float2 dv)
{
    float h = base;
    h = h + (red.SampleGrad(detailSampler, uv, du, dv).w - h) * w.r;
    h = h + (green.SampleGrad(detailSampler, uv, du, dv).w - h) * w.g;
    h = h + (blue.SampleGrad(detailSampler, uv, du, dv).w - h) * w.b;
    h = h + (alpha.SampleGrad(detailSampler, uv, du, dv).w - h) * (1.0 - w.a);
    return h;
}

float pomHeight(float2 uv, float2 du, float2 dv, float4 wBase, float4 wTop, float influence)
{
    float baseH = g_BaseNormal.SampleGrad(g_HeightSampler, uv, du, dv).w;
    float h = splatHeightMix(0.5, wBase, g_DetailN4, g_DetailN5, g_DetailN6, g_DetailN7,
            g_HeightSampler, uv, du, dv);
    h = splatHeightMix(h, wTop, g_DetailN0, g_DetailN1, g_DetailN2, g_DetailN3,
            g_HeightSampler, uv, du, dv);
    return baseH + (h - baseH) * influence;
}

// samplePOMHeightBlurred (old engine): 5-tap cross blur (center ×2, ÷6) at
// ±1 texel in the uv gradient frame. The 256 discrete height levels make a
// sub-pixel uv shift (TAA jitter) jump the ray-march intersection between
// frames; the blur smooths the field so the hit moves continuously.
float pomHeightBlurred(float2 uv, float2 du, float2 dv, float4 wBase, float4 wTop, float influence)
{
#if SPLAT_POM_BLUR
    float2 sx = float2(du.x, 0.0);
    float2 sy = float2(0.0, dv.y);
    float h = pomHeight(uv, du, dv, wBase, wTop, influence) * 2.0;
    h += pomHeight(uv + sx, du, dv, wBase, wTop, influence);
    h += pomHeight(uv - sx, du, dv, wBase, wTop, influence);
    h += pomHeight(uv + sy, du, dv, wBase, wTop, influence);
    h += pomHeight(uv - sy, du, dv, wBase, wTop, influence);
    return h / 6.0;
#else
    return pomHeight(uv, du * 2.0, dv * 2.0, wBase, wTop, influence);
#endif
}

// parallaxOcclusionMap (old engine): linear ray-march through the height
// field until the view ray dives under it, then a binary refinement —
// blurred samples during the march, center-biased first (uv += slope/2) so
// height 0.5 is the geometric surface. Steps adapt with the view angle
// (6 head-on, 8 grazing). Returns the offset uv; the caller's chains sample
// at it while the original du/dv gradients stay valid (the offset is
// sub-texel).
float2 parallaxOcclusionUV(float2 uv, float3 vTS, float heightScale, float fadeFactor,
        float2 du, float2 dv, float4 wBase, float4 wTop, float influence)
{
    if (fadeFactor < 0.001 || heightScale <= 0.0) {
        return uv;
    }
    float3 V           = normalize(vTS);
    float  angleFactor = 1.0 - abs(V.z);
    int    numSteps    = (int)((float)SPLAT_POM_MIN_STEPS +
            ((float)SPLAT_POM_MAX_STEPS - (float)SPLAT_POM_MIN_STEPS) * angleFactor);
    float  stepH = 1.0 / (float)numSteps;
    float2 slope = (V.xy / max(abs(V.z), 0.001)) * heightScale;

    float2 currentUV      = uv + slope * 0.5;
    float  currentHeight  = 1.0;
    float  sampledHeight  = pomHeightBlurred(currentUV, du, dv, wBase, wTop, influence);
    [loop]
    for (int i = 0; i < numSteps; i++)
    {
        if (currentHeight <= sampledHeight) { break; }
        currentUV     -= slope * stepH;
        currentHeight -= stepH;
        sampledHeight  = pomHeightBlurred(currentUV, du, dv, wBase, wTop, influence);
    }
    float2 prevUV     = currentUV + slope * stepH;
    float  prevHeight = currentHeight + stepH;
    [loop]
    for (int i = 0; i < SPLAT_POM_BINARY_STEPS; i++)
    {
        float2 midUV      = 0.5 * (prevUV + currentUV);
        float  midHeight  = 0.5 * (prevHeight + currentHeight);
        float  midSampled = pomHeightBlurred(midUV, du, dv, wBase, wTop, influence);
        if (midHeight > midSampled) {
            prevUV = midUV;
            prevHeight = midHeight;
        } else {
            currentUV = midUV;
            currentHeight = midHeight;
        }
    }
    return uv + (currentUV - uv) * fadeFactor;
}

float3 safeNormalize3(float3 v, float3 fallback)
{
    float len2 = dot(v, v);
    return (len2 > 1e-8) ? (v * rsqrt(len2)) : fallback;
}

// buildTerrainTBN (old engine): X-tangent and Z-bitangent Gram-Schmidt'd
// against the geometric normal — surface-aligned, so on slopes the parallax
// ray leans with the geometry instead of assuming a flat Y-up uv frame.
// float3x3(T, B, N) fills rows, so mul(M, V) = V.x*T + V.y*B + V.z*N, the
// exact old transpose(mat3(T, B, N)) * V.
float3 pomViewDirTS(float3 geomNormal, float3 V)
{
    float3 N = safeNormalize3(geomNormal, float3(0.0, 1.0, 0.0));
    float3 T = safeNormalize3(float3(1.0, 0.0, 0.0) - N * N.x, float3(1.0, 0.0, 0.0));
    float3 B = float3(0.0, 0.0, 1.0) - N * N.z - T * dot(T, float3(0.0, 0.0, 1.0));
    B        = safeNormalize3(B, safeNormalize3(cross(T, N), float3(0.0, 0.0, 1.0)));
    return normalize(mul(float3x3(T, B, N), V));
}

float4 triplanarSample(Texture2D tex, SamplerState smp, float3 pos, float3 w)
{
    float4 c = float4(0.0, 0.0, 0.0, 0.0);
    c = c + tex.Sample(smp, pos.zy) * w.x;
    c = c + tex.Sample(smp, pos.xz) * w.y;
    c = c + tex.Sample(smp, pos.xy) * w.z;
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
// w2 = -exp(-negExp * (2d - 1)). With the caster far-pad (fFarPadS) both sides
// warp z/farPadS (atlas and receiver depths are compressed by farPadS). The
// receiver un-warps its RAW cascade z * 1/farPadS with the same mirrored exponents (Shadows.fxh WarpDepthEVSM, clamped to 42 for
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

float2 warpDepthEVSM(float depth, float farPadS, out float2 exOut)
{
    // bIs32BitEVSM is a BOOL (int) in the C++ ShadowMapAttribs (bit 0x1 = true);
    // the mirror must read it as an int, not a float (float read = denormal ~0).
    float  maxExp = (bIs32BitEVSM > 0) ? 42.0 : 5.54;
    float2 ex     = min(sVSMParams.zw, float2(maxExp, maxExp));
    float  pad    = farPadS;
    ex *= (pad > 0.0 && pad < 1.0) ? pad / (2.0 - pad) : 1.0;
    exOut = ex;
    float  d      = 2.0 * depth * (pad > 0.0 ? 1.0 / pad : 1.0) - 1.0;
    return float2(exp(ex.x * d), -exp(-ex.y * d));
}

float filterShadowEVSM(float2 uv, float slice, float lightDepth, bool evsm4, out float pPosOut, out float pNegOut, out float dbgM1, out float2 dbgTapUV)
{
    pPosOut = 1.0;
    pNegOut = 1.0;
    dbgM1 = -1.0;
    dbgTapUV = uv;
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
    float2 exWarp;
    float2 w      = warpDepthEVSM(lightDepth, f4ShadowFade.z, exWarp);
    float2 minVar = sVSMParams.x * exWarp * w;

    float sum = 0.0;
    float sumPos = 0.0;
    float sumNeg = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float2 tUV = (baseUV + float2(s, t) + poisson[i]) * texel;
        tUV        = clamp(tUV, 0.0, 1.0);
        float4 m  = g_ShadowMapLinear.Sample(g_ShadowMapNearestSampler, float3(tUV, slice));
        if (i == 0)
        {
            dbgM1 = m.x;
            dbgTapUV = tUV;
        }
        float  p  = chebyshevUpperBound(m.xy, w.x, minVar.x * minVar.x);
        if (evsm4)
        {
            float pN = chebyshevUpperBound(m.zw, w.y, minVar.y * minVar.y);
            sumPos += p;
            sumNeg += pN;
            p = min(p, pN);
        }
        sum += p;
    }
    pPosOut = sumPos / 8.0;
    pNegOut = sumNeg / 8.0;
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

    // POM: shift the tiled detail uv (only it — the triplanar cliff, the
    // weight uv and the geometric depth/motion stay untouched). The view ray
    // is transformed by the surface-aligned TBN (old buildTerrainTBN); the
    // depth is world meters converted to tiled-uv units exactly like old
    // hScaleSplat (POM_DEPTH_SPLAT * tile / reference span). The fade ramps
    // the offset back to zero over the distance window.
    float3 V = normalize(cCamPosition.xyz - In.AnchoredPos);
#if SPLAT_POM
    float  camDist    = length(cCamPosition.xyz - In.AnchoredPos);
    float  pomFade    = 1.0 - smoothstep(SPLAT_POM_FADE_START, SPLAT_POM_FADE_END, camDist);
    float  pomScaleUV = SPLAT_POM_DEPTH * SPLAT_DETAIL_TILE / SPLAT_DETAIL_METERS;
    float3 pomViewTS  = pomViewDirTS(In.WorldNormal, V);
    tiledUV = parallaxOcclusionUV(tiledUV, pomViewTS, pomScaleUV, pomFade,
            du, dv, wBase, wTop, influence);
#endif

    float4 baseA4 = g_BaseAlbedo.SampleGrad(g_DetailSampler, tiledUV, du, dv);
    float4 baseN4 = g_BaseNormal.SampleGrad(g_DetailSampler, tiledUV, du, dv);
    float3 baseAlbedo = baseA4.rgb;
    float  roughnessS = baseA4.a;
    float  aoS        = baseN4.b;
    float3 baseN      = splatNormalDecode(baseN4.rg);

    float3 albedo = baseAlbedo;
    float3 nT     = baseN;
    if (influence > 0.0)
    {
        float  splatRough = 0.5;
        float  splatAo    = 1.0;
        float3 splatAlbedo = float3(0.0, 0.0, 0.0);
        splatAlbedoMix(splatAlbedo, splatRough, wBase,
                g_Detail4, g_Detail5, g_Detail6, g_Detail7, g_DetailSampler, tiledUV, du, dv);
        splatAlbedoMix(splatAlbedo, splatRough, wTop, g_Detail0, g_Detail1, g_Detail2, g_Detail3,
                g_DetailSampler, tiledUV, du, dv);
        float3 splatN = float3(0.0, 0.0, 1.0);
        splatNormalMix(splatN, splatAo, wBase,
                g_DetailN4, g_DetailN5, g_DetailN6, g_DetailN7, g_DetailSampler, tiledUV, du, dv);
        splatNormalMix(splatN, splatAo, wTop, g_DetailN0, g_DetailN1, g_DetailN2, g_DetailN3,
                g_DetailSampler, tiledUV, du, dv);
        albedo     = baseAlbedo + (splatAlbedo - baseAlbedo) * influence;
        nT         = baseN + (splatN - baseN) * influence;
        roughnessS = roughnessS + (splatRough - roughnessS) * influence;
        aoS        = aoS + (splatAo - aoS) * influence;
    }

    float worldY = In.AnchoredPos.y + g_Anchor.y;
    float slope  = 1.0 - max(In.WorldNormal.y, 0.0);
    float wSand  = smoothstep(SPLAT_SAND_LO, SPLAT_SAND_HI, worldY) *
                   (1.0 - smoothstep(0.25 * SPLAT_SAND_HI, SPLAT_SAND_HI, worldY));
    float wCliff = smoothstep(SPLAT_CLIFF_LO, SPLAT_CLIFF_HI, slope);
    float wSnow  = smoothstep(SPLAT_SNOW_LO, SPLAT_SNOW_HI, worldY);

    float4 sandA4 = g_SandAlbedo.SampleGrad(g_DetailSampler, tiledUV, du, dv);
    albedo = albedo + (sandA4.rgb - albedo) * wSand;
    roughnessS = roughnessS + (sandA4.a - roughnessS) * wSand;
    float3 wTri   = pow(abs(normalize(In.WorldNormal)), 4.0);
    wTri         = wTri / (wTri.x + wTri.y + wTri.z + 1e-6);
    float3 cliffUV = (In.AnchoredPos + g_Anchor) * (1.0 / SPLAT_CLIFF_METERS);
    float4 cliffA4 = triplanarSample(g_CliffAlbedo, g_DetailSampler, cliffUV, wTri);
    albedo = albedo + (cliffA4.rgb - albedo) * wCliff;
    roughnessS = roughnessS + (cliffA4.a - roughnessS) * wCliff;
    float4 snowA4 = g_SnowAlbedo.SampleGrad(g_DetailSampler, tiledUV, du, dv);
    albedo = albedo + (snowA4.rgb - albedo) * wSnow;
    roughnessS = roughnessS + (snowA4.a - roughnessS) * wSnow;

    float4 sandN4 = g_SandNormal.SampleGrad(g_DetailSampler, tiledUV, du, dv);
    nT = nT + (splatNormalDecode(sandN4.rg) - nT) * wSand;
    aoS = aoS + (sandN4.b - aoS) * wSand;
    float4 cliffN4 = triplanarSample(g_CliffNormal, g_DetailSampler, cliffUV, wTri);
    nT = nT + (splatNormalDecode(cliffN4.rg) - nT) * wCliff;
    aoS = aoS + (cliffN4.b - aoS) * wCliff;
    float4 snowN4 = g_SnowNormal.SampleGrad(g_DetailSampler, tiledUV, du, dv);
    nT = nT + (splatNormalDecode(snowN4.rg) - nT) * wSnow;
    aoS = aoS + (snowN4.b - aoS) * wSnow;

    // Tangent frame (the glTF TANGENT attribute: xyz + handedness), then the
    // perturbed world normal.
    float3 N0 = normalize(In.WorldNormal);
    float3 T  = normalize(In.Tangent.xyz);
    float3 B  = cross(N0, T) * In.Tangent.w;
    float3 N  = normalize(T * nT.x + B * nT.y + N0 * nT.z);

    float  NdotV = dotSat(N, V);

    // ── IBL (ApplyIBL + GetLambertianIBL/GetSpecularIBL_GGX, PBR path) ──
    float  roughness = clamp(roughnessS, 0.04, 1.0);
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

    // Material AO (normal-map blue) darkens the ambient terms only — old
    // engine: color = (ambientDiffuse + ambientSpecular) * ao + Lo.
    float3 IBL = (DiffuseIBL + SpecularIBL) * rIBLScale.xyz * aoS;

    // ── Direct sun (ApplyPunctualLight, PBR path — the frame's light 0) ──
    float3 LightDir    = lDir.xyz;       // travel direction
    float  ShadowIndex = lDir.w;
    float3 LightIntensity = lInt.xyz;
    float  shadowMode = f4ShadowFade.y;
    float  Attenuation = 1.0;
    int    dbgCascade = -2;
    float2 dbgUV      = float2(0.0, 0.0);
    float  dbgRaw     = -1.0;
    float  dbgPos     = 1.0;
    float  dbgNeg     = 1.0;
    float  dbgZ       = -1.0;
    float  dbgM1      = -1.0;
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
                    float2 dbgTapUV2;
                    Attenuation = filterShadowEVSM(cascadeUV, float(cascade), cascadeNdc.z, false, dbgPos, dbgNeg, dbgM1, dbgTapUV2);
                    dbgZ = cascadeNdc.z;
                }
                else
                {
                    float2 dbgTapUV2;
                    Attenuation = filterShadowEVSM(cascadeUV, float(cascade), cascadeNdc.z, true, dbgPos, dbgNeg, dbgM1, dbgTapUV2);
                    dbgZ = cascadeNdc.z;
                }
            }
            // Receiver-side tier-distance fade (lessons.md 2026-09-07): the
            // padded light cube still samples past the tier distance, so fade
            // the shadow to lit over the last 25 % of it — the effective
            // cutoff.
            float tier = f4ShadowFade.x;
            if (tier > 0.0)
            {
                float fade = clamp((tier - viewZ) / (0.25 * tier), 0.0, 1.0);
                Attenuation += (1.0 - Attenuation) * (1.0 - fade);
            }
        }
    }
    float3 specDbg   = float3(0.0, 0.0, 0.0);
    float  specDbgN  = 0.0;
    if (fShadowDebugMode > 99.5)
    {
        float3 L = -LightDir;
        float NdotL = dotSat(N, L);
        float3 H = normalize(L + V);
        float  NdotH = max(dot(N, H), 0.0);
        float  VdotH = max(dot(V, H), 0.0);
        float  AlphaRoughness = roughness * roughness;
        float  D   = ggxNormalDistribution(NdotH, AlphaRoughness);
        float  Vis = ggxVisibilityCorrelated(NdotL, NdotV, AlphaRoughness);
        float3 F   = schlickReflection(VdotH, R0, R90);
        float3 SpecContrib    = F * Vis * D;
        specDbg  = SpecContrib * NdotL * (LightIntensity * 0.25);
        specDbgN = NdotH;
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

    if (fShadowDebugMode > 99.5)
    {
        if (fShadowDebugMode < 100.5)
            IBL = float3(roughness, roughness, roughness);
        else if (fShadowDebugMode < 101.5)
            IBL = specDbg;
        else if (fShadowDebugMode < 102.5)
            IBL = 0.5 + 0.5 * N;
        else
            IBL = float3(specDbgN, specDbgN, specDbgN);
    }
    else if (fShadowDebugMode > 0.5)
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
        else if (fShadowDebugMode < 4.5)
            IBL = float3(dbgPos, dbgNeg, 0.0);
        else if (fShadowDebugMode < 6.5)
            IBL = float3(dbgZ * 2.5, dbgM1 > 0.0 ? min(log10(1.0 + dbgM1) * 0.1, 1.0) : 0.0, dbgUV.y);
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
