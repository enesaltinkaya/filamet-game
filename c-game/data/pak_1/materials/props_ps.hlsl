// Azgaar props pixel shader (diligent backend).
// The DiligentFX-PBR lighting port of the terrain PS (same frame attribs,
// same sun fill, same constant IBL cubes + preintegrated GGX LUT + same
// Uncharted2 tone map), so props read as one scene with the terrain and
// the glTF model. Props are matte dielectrics (roughness 0.9, metallic 0)
// at vegetation scale — no shadows/SSAO inputs on this path (parity with
// the terrain pass, which runs without them too).
// Albedo rules (ported from the old engine's azgaar_props.frag):
//   - per-part tint mask: a white part colour marks the part tintable
//     (leaves / canopy) and receives the per-instance biome tint; a baked
//     non-white part colour (trunk) stays its own colour. Textured parts
//     (grass cards, flag bit 3): non-tintable parts keep the base texture,
//     tintable parts get tint * texture.
//   - flag bit 0: alpha-test the base texture at 0.5 (cutout grass cards)
//   - flag bit 2: radial flower-disc alpha test (unit-UV quads)
//   - flag bit 1 (thin double-sided vegetation): rendered double-sided
//     (the PSO culls nothing), but BOTH faces light with the unflipped
//     normal — a blade's back face faces the sky like its front. Closed
//     solids flip the normal on back faces so slab undersides stay unsunlit.

#include "BasicStructures.fxh"
#include "PBR_Shading.fxh"
#include "RenderPBR_Structures.fxh"

// CSM shadow sampling (DiligentFX Shadows.fxh): macros provided by the
// pass at shader creation — see heightmap_terrain_ps.hlsl.
#ifndef ENABLE_CSM_SHADOWS
#   define ENABLE_CSM_SHADOWS 0
#endif
#if ENABLE_CSM_SHADOWS
#include "Shadows.fxh"
#endif

// Tone map the same as the glTF PBR path (PBR_Renderer DefineMacros).
#ifndef TONE_MAPPING_MODE
#   define TONE_MAPPING_MODE TONE_MAPPING_MODE_UNCHARTED2
#endif
#include "ToneMapping.fxh"

cbuffer cbFrameAttribs
{
    PBRFrameAttribs g_Frame;
    PBRLightAttribs g_Sun;
    LightAttribs    g_Light;   // DiligentFX CSM attribs (ShadowDiligent.cpp)
    float4          g_ShadowFade;  // CSM: receiver fade (x = lit-by view z m, y = fade start m)
};

#if ENABLE_CSM_SHADOWS
#if SHADOW_MODE == SHADOW_MODE_PCF
    Texture2DArray<float>  g_tex2DShadowMap;
    SamplerComparisonState g_tex2DShadowMap_sampler;
#else
    Texture2DArray<float4> g_tex2DFilterableShadowMap;
    SamplerState           g_tex2DFilterableShadowMap_sampler;
#endif
#endif

Texture2D   g_BaseTex;         // per-variant base colour (white 1x1 fallback)
TextureCube g_IblIrradiance;   // constant-environment cubes (terrain pass parity)
TextureCube g_IblPrefiltered;
Texture2D   g_PreintegratedGGX;

SamplerState g_ClampSampler;  // IBL cubes + BRDF LUT
SamplerState g_BaseSampler;   // min LINEAR_MIPMAP_LINEAR / mag LINEAR / WRAP

struct PSPropsIn
{
    float4 Position  : SV_Position;
    float3 WorldPos  : WORLD_POS;
    float3 Normal    : NORMAL;
    float3 Tint      : TINT;
    float2 UV        : UV;
    float3 PartColor : PART_COLOR;
    nointerpolation uint Flags : FLAGS;
    bool FrontFacing : SV_IsFrontFace; // system-generated (glslang HLSL has no gl_FrontFacing)
    float4 PrevClipPos : PREV_CLIP_POS; // TAA: prev-frame clip position
    float3 PosInLightViewSpace : LIGHT_SPACE_POS; // CSM sampling
};

struct PSPropsOut
{
    float4 Color : SV_Target0;
    float4 Motion : SV_Target1; // TAA motion vectors (xy = NDC delta)
    float4 Normal : SV_Target2; // world-space normal (SSAO input)
};

// TAA motion vectors — the terrain PS' convention (RenderPBR.psh
// GetMotionVector: unjittered curr NDC minus unjittered prev NDC; NDC from
// SV_Position via Diligent's TexUVToNormalizedDeviceXY).
float2 texUVToNormalizedDeviceXY(float2 uv)
{
    return (uv - float2(0.5, 0.5)) * float2(2.0, -2.0);
}

float2 taaMotionVector(float4 clipPos, float4 prevClipPos)
{
    float2 ndc     = texUVToNormalizedDeviceXY(clipPos.xy * g_Frame.Camera.f4ViewportSize.zw);
    float2 prevNdc = prevClipPos.xy / prevClipPos.w;
    return (ndc - g_Frame.Camera.f2Jitter) - (prevNdc - g_Frame.PrevCamera.f2Jitter);
}

PSPropsOut main(in PSPropsIn In)
{
    bool   textured = (In.Flags & 8u) != 0u;
    float  tintable = step(0.99, min(min(In.PartColor.r, In.PartColor.g), In.PartColor.b));
    float3 albedo;

    if (textured)
    {
        float4 tex = g_BaseTex.Sample(g_BaseSampler, In.UV);
        // Cutout alpha test (grass cards): the hard 0.5 test keeps the edge
        // pinned to the world point (no per-frame stochastic flip).
        if ((In.Flags & 1u) != 0u && tex.a < 0.5) discard;
        // Flower dot: keep only a small central disc of the unit-UV quad.
        if ((In.Flags & 4u) != 0u && length(In.UV - 0.5) > 0.30) discard;
        float3 tint = lerp(float3(1.0, 1.0, 1.0), In.Tint, tintable);
        albedo = tint * tex.rgb;
    }
    else if (g_Frame.Camera.f4ExtraData[2].x > 0.5)
    {
        albedo = In.Tint;
    }
    else
    {
        // Procedural: tintable parts get the biome tint, non-tintable parts
        // keep their baked part colour.
        albedo = lerp(In.PartColor, In.Tint, tintable);
    }

    float3 N0     = normalize(In.Normal);
    // Back faces store the outward normal of the visible surface.
    float3 N      = In.FrontFacing ? N0 : -N0;
    // Thin vegetation lights with the unflipped normal on both sides.
    float3 Nlight = (In.Flags & 2u) != 0u ? N0 : N;

    // The camera sits at the anchor origin (rotation-only view), so the view
    // direction comes from the ANCHOR-RELATIVE position (the same split the
    // VS subtracted — the absolute one would point speculars at the world
    // origin, 39 km away).
    float3 rel = In.WorldPos - g_Frame.Camera.f4ExtraData[3].xyz
                          - g_Frame.Camera.f4ExtraData[4].xyz;
    float3 V   = normalize(g_Frame.Camera.f4Position.xyz - rel);

    SurfaceReflectanceInfo srf = GetSurfaceReflectanceMR(albedo, 0.0, 0.9);

    BaseLayerShadingInfo base;
    base.Metallic = 0.0;
    base.Srf      = srf;
    base.Normal   = Nlight;
    base.NdotV    = dot_sat(Nlight, V);

    SurfaceShadingInfo shading;
    shading.Pos       = In.WorldPos;
    shading.View      = V;
    shading.Occlusion = 1.0;
    shading.Emissive  = float3(0.0);
    shading.BaseLayer = base;
    shading.IBLScale  = g_Frame.Renderer.IBLScale.rgb;

    SurfaceLightingInfo lighting = GetDefaultSurfaceLightingInfo();

    // Direct sun + IBL both run on the lighting normal: thin vegetation
    // (flag bit 1) catches the sun on BOTH faces of a blade/card — a card's
    // back face faces the sky like its front, so the flipped back-face
    // normal must not be fed to the light (it would NdotL == 0 and render
    // half of every tuft near-black; the port of the old engine's `Nlight`
    // computed it but never used it).
#if ENABLE_CSM_SHADOWS
    // CSM factor scales the SUN only; the IBL fill below keeps shading the
    // shadowed vegetation (thin-leaf translucency look).
    PBRLightAttribs sun = g_Sun;
    {
        FilteredShadow sh;
        float cameraSpaceZ = In.Position.w;
#   if SHADOW_MODE == SHADOW_MODE_PCF
        sh = FilterShadowMap(g_Light.ShadowAttribs, g_tex2DShadowMap, g_tex2DShadowMap_sampler,
                             In.PosInLightViewSpace, ddx(In.PosInLightViewSpace), ddy(In.PosInLightViewSpace),
                             cameraSpaceZ);
#   else
        sh = SampleFilterableShadowMap(g_Light.ShadowAttribs, g_tex2DFilterableShadowMap, g_tex2DFilterableShadowMap_sampler,
                                       In.PosInLightViewSpace, ddx(In.PosInLightViewSpace), ddy(In.PosInLightViewSpace),
                                       cameraSpaceZ);
#   endif
        // Receiver distance fade: the cascade box (StabilizeExtents bounding
        // sphere of the caster-padded frustum) reaches well past the tier's
        // shadow distance — fade the contribution to lit over the last
        // quarter of it (the sun only; the IBL fill below keeps the
        // shadowed vegetation shaded).
        if (g_ShadowFade.x > 0.0)
            sh.fLightAmount = lerp(1.0, sh.fLightAmount,
                                   1.0 - smoothstep(g_ShadowFade.y, g_ShadowFade.x, cameraSpaceZ));
        sun.IntensityR *= sh.fLightAmount;
        sun.IntensityG *= sh.fLightAmount;
        sun.IntensityB *= sh.fLightAmount;
    }
    ApplyPunctualLight(shading, sun, lighting);
#else
    ApplyPunctualLight(shading, g_Sun, lighting);
#endif

    ApplyIBL(shading,
             g_Frame.Renderer.PrefilteredCubeLastMip,
             g_Frame.Renderer.EnvironmentRotation,
             g_PreintegratedGGX, g_ClampSampler,
             g_IblIrradiance, g_ClampSampler,
             g_IblPrefiltered, g_ClampSampler,
             lighting);

    float3 color = ResolveLighting(shading, lighting);

    ToneMappingAttribs tm;
    tm.iToneMappingMode     = TONE_MAPPING_MODE_UNCHARTED2;
    tm.bAutoExposure        = false;
    tm.fMiddleGray          = g_Frame.Renderer.MiddleGray;
    tm.bLightAdaptation     = false;
    tm.fWhitePoint          = g_Frame.Renderer.WhitePoint;
    tm.fLuminanceSaturation = 1.0;
    color = ToneMap(color, tm, g_Frame.Renderer.AverageLogLum);

    PSPropsOut result;
    result.Color = float4(color, 1.0);
    result.Motion = float4(taaMotionVector(In.Position, In.PrevClipPos), 0.0, 0.0);
    result.Normal = float4(N, 1.0);
    return result;
}
