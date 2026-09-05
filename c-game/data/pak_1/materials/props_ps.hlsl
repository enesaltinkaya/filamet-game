// Azgaar props pixel shader (diligent backend).
//
// The DiligentFX-PBR lighting port of the terrain PS (same frame attribs,
// same sun fill, same constant IBL cubes + preintegrated GGX LUT + same
// Uncharted2 tone map), so props read as one scene with the terrain and
// the glTF model. Props are matte dielectrics (roughness 0.9, metallic 0)
// at vegetation scale — no shadows/SSAO inputs on this path (parity with
// the terrain pass, which runs without them too).
//
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

// Tone map the same as the glTF PBR path (PBR_Renderer DefineMacros).
#ifndef TONE_MAPPING_MODE
#   define TONE_MAPPING_MODE TONE_MAPPING_MODE_UNCHARTED2
#endif
#include "ToneMapping.fxh"

cbuffer cbFrameAttribs
{
    PBRFrameAttribs g_Frame;
    PBRLightAttribs g_Sun;
};

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
};

struct PSPropsOut
{
    float4 Color : SV_Target0;
};

PSPropsOut main(in PSPropsIn In)
{
    // ── Albedo (per-instance tint x per-part mask x base texture) ─────────
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

    // ── Normals: double-sided rules ────────────────────────────────────────
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

    // ── DiligentFX PBR lighting (terrain PS parity) ────────────────────────
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
    ApplyPunctualLight(shading, g_Sun, lighting);

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
    return result;
}
