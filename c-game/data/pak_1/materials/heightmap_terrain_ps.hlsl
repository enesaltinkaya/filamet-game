// Heightmap terrain pixel shader (diligent backend).
//
// Full port of the look in materials/terrain.mat (phase 5) onto the
// DiligentFX PBR pipeline, so the terrain reads as one scene with the
// glTF PBR path: same ApplyPunctualLight/ApplyIBL/ResolveLighting math,
// same frame attribs (view/proj/camera/sun/tone-map), same tone map
// (Uncharted2 with the gltf PBR frame's AverageLogLum/MiddleGray/
// WhitePoint). Lighting inputs come from:
//   - cbFrameAttribs: PBRFrameAttribs + one PBRLightAttribs (directional
//     sun), filled exactly like GltfDiligent.cpp fillFrameAttribs; the
//     per-world look values ride in Camera.f4ExtraData (mapBounds /
//     climateParams / maxLandHeight+debugView — see the look constants
//     block below; a second cbuffer bound ambiguously on this pipeline,
//     see docs/lessons.md, the diligent-terrain entry)
//   - the 9 look/tiling Texture2Ds + 2 constant IBL cubes + the PBR
//     preintegrated GGX LUT (SRV from GLTF_PBR_Renderer, see
//     GetPreintegratedGGX_SRV).
//
// The look is a port of the old engine's heightmap_terrain.frag, same as
// terrain.mat:
//   - default grass texture, world-space tiling + tangent normal map
//   - per-world biome tint (biomeColor texture, registered at world load)
//   - dry-turf colour variation (coarse world-anchored value noise)
//   - waterline: beach sand band + wet-sand strip at sea level
//   - slope-based triplanar cliff + altitude rock band
//   - climate snow line (temperature + Glacier biome, climate texture)
//   - micro-band normal perturbation (32/16/8/4 m value noise) with
//     derivative-based octave fade and grazing gain
// The geometry band (wavelengths >= 64 m) is baked into the lattice
// heights; only 32/16/8/4 m perturb the shading normal here, so the
// rendered surface stays exactly the CPU/physics bilinear surface.
//
// Debug views (terrain.mat parity): 0 = off, 1 = 256 m periodic height
// ramp, 2 = raw biome-colour texture. Drawn as a flat matte surface.
//
// One shared SRB serves every tile: all pipeline resources are STATIC
// (bound once through the PRS); only the per-draw VBO varies.

#include "BasicStructures.fxh"
#include "PBR_Shading.fxh"
#include "RenderPBR_Structures.fxh"

// Tone map the same as the glTF PBR path (PBR_Renderer DefineMacros).
#ifndef TONE_MAPPING_MODE
#   define TONE_MAPPING_MODE TONE_MAPPING_MODE_UNCHARTED2
#endif
#include "ToneMapping.fxh"

// ── Terrain look constants (identical to terrain.mat) ──────────────────────

// World-space tiling frequency for the grass texture (repeats per metre).
#define AZGAAR_GRASS_TILE (2048.0 / 7000.0)
#define AZGAAR_CLIFF_DETAIL_TILE 32.0
#define CLIFF_TRIPLANAR_SCALE (AZGAAR_CLIFF_DETAIL_TILE / 4096.0)
#define SPLAT_NORMAL_STRENGTH 2.0

// Climate texture R channel encoding: byte = temperature (deg C) + bias
// (kept monotonic so bilinear filtering cannot ring across a sign change).
#define AZGAAR_CLIMATE_TEMP_BIAS 64.0
// Biome id (FMG): 11 = Glacier.
#define AZGAAR_BIOME_GLACIER 11.0

#define MICRO_NOISE_STRENGTH 0.45

// ── Frame / look cbuffers ──────────────────────────────────────────────────
// cbFrameAttribs: PBRFrameAttribs (camera + PBRRendererShaderParameters)
// followed immediately by one PBRLightAttribs (the directional sun) — the
// gltf path's "layout owned by the renderer" pairing. Both are filled with
// the exact values GltfDiligent.cpp fillFrameAttribs uses, so sun/ambient/
// tone-map read identically on terrain and model.
cbuffer cbFrameAttribs
{
    PBRFrameAttribs   g_Frame;
    PBRLightAttribs   g_Sun;
};

// NOTE: the look params (map bounds, climate thresholds, debug view) are NOT
// in a separate cbuffer — they ride in g_Frame.Camera.f4ExtraData
// (filled in fillFrameAttribs). A second cbuffer here bound ambiguously on
// this pipeline (the PS read the frame attribs instead, rendering the
// debug-white path); see docs/lessons.md, the diligent-terrain entry.
// ── Look constants (carried in g_Frame.Camera.f4ExtraData — see the note
// above; filled by fillFrameAttribs) ─────────────────────────────────────
#define mapBounds      (g_Frame.Camera.f4ExtraData[0])
#define climateParams  (g_Frame.Camera.f4ExtraData[1])
#define maxLandHeight  (g_Frame.Camera.f4ExtraData[2].x)
#define debugView      (g_Frame.Camera.f4ExtraData[2].y)

// ── SRV slots ──────────────────────────────────────────────────────────────
// Default terrain textures (engine pak, KTX2/BC7; albedos created as
// BC7_UNORM_SRGB so the sRGB decode matches the Filament pass).
Texture2D g_GrassAlbedo;
Texture2D g_GrassNormal;
Texture2D g_CliffAlbedo;
Texture2D g_CliffNormal;
Texture2D g_SnowAlbedo;
Texture2D g_SandAlbedo;
// Per-world look textures (registered at world load; map-space UV).
Texture2D g_BiomeColor;
Texture2D g_Climate;
// Same texture as g_Climate, NEAREST-sampled: its A channel carries
// discrete biome ids, which bilinear filtering would invent bogus
// intermediate biomes from.
Texture2D g_ClimateNearest;
// Constant-environment IBL (1x1x6 RGBA8 cubes, gltfIblUpdateDiligent
// formula) + the PBR preintegrated GGX split-sum LUT.
TextureCube g_IblIrradiance;
TextureCube g_IblPrefiltered;
Texture2D   g_PreintegratedGGX;

// ── Sampler slots (docs/lessons.md: no plain-LINEAR tiling samplers) ───────
// Exactly three sampler states (shared across slots):
//   g_TilingSampler: min LINEAR_MIPMAP_LINEAR / mag LINEAR / REPEAT /
//     anisotropy 16 (world-tiling terrain textures, 11 mip levels) — used
//     for the six default terrain textures
//   g_ClampSampler: min/mag LINEAR / CLAMP — look maps, IBL cubes, BRDF LUT
//   g_ClampNearestSampler: NEAREST / CLAMP — biome-id channel
// The ApplyIBL cube/LUT samplers are passed as g_ClampSampler (DiligentFX
// declares no implicit companion samplers — every sampler is explicit).
//
// SRB resource list (bind by name; slots follow declaration order):
//   cbuffer 0 cbFrameAttribs (PBRFrameAttribs + PBRLightAttribs, 1 buffer)
//   SRV     g_GrassAlbedo, g_GrassNormal, g_CliffAlbedo, g_CliffNormal,
//           g_SnowAlbedo, g_SandAlbedo, g_BiomeColor, g_Climate,
//           g_ClimateNearest, g_IblIrradiance, g_IblPrefiltered,
//           g_PreintegratedGGX
//   sampler g_TilingSampler, g_ClampSampler, g_ClampNearestSampler
SamplerState g_TilingSampler;       // min LINEAR_MIPMAP_LINEAR / mag LINEAR / REPEAT
SamplerState g_ClampSampler;        // min/mag LINEAR / CLAMP
SamplerState g_ClampNearestSampler; // NEAREST / CLAMP

// ── Input (from heightmap_terrain_vs.hlsl) / output ───────────────────────
struct PSTerrainIn
{
    float4 Position : SV_Position;
    float3 WorldPos : WORLD_POS;
    float3 Normal   : NORMAL;
};

struct PSTerrainOut
{
    float4 Color : SV_Target0;
};

// ── Look math (verbatim ports of the terrain.mat fragment helpers) ─────────

// fract-based cell hash (the old engine's utils.shader hash, kept so the
// procedural noise fields read identically on both backends).
float hash21(float2 p)
{
    p = frac(p * 0.3183099 + float2(0.1, 0.3));
    p *= 17.0;
    return frac(p.x * p.y * (p.x + p.y));
}

float microValueNoise(float2 p)
{
    float2 ip = floor(p);
    float2 fp = p - ip;
    float2 u  = fp * fp * (3.0 - 2.0 * fp);
    float a = hash21(ip);
    float b = hash21(ip + float2(1.0, 0.0));
    float c = hash21(ip + float2(0.0, 1.0));
    float d = hash21(ip + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y) * 2.0 - 1.0;
}

// Grazing-aware octave fade: an octave of wavelength `lambda` is only kept
// while it spans a few pixels in BOTH screen directions, keyed on the
// screen-space derivatives of the world-XZ noise input (the true on-screen
// sampling rate, including grazing-angle depth compression).
float octaveFade(float lambda, float dpx, float dpy)
{
    float pxX = lambda / max(dpx, 1e-5);
    float pxY = lambda / max(dpy, 1e-5);
    return smoothstep(2.0, 8.0, min(pxX, pxY));
}

// Synthetic "height" of the micro band (metres), used only for its slope.
// Each octave is derivative-faded so sub-pixel octaves do not alias into
// specular smudges at grazing angles.
float microBumpFaded(float2 p, float dpx, float dpy)
{
    return 0.5 * microValueNoise(p / 32.0 + 11.17) * octaveFade(32.0, dpx, dpy) +
           0.25 * microValueNoise(p / 16.0 + 5.93) * octaveFade(16.0, dpx, dpy) +
           0.5 * microValueNoise(p / 8.0 + 7.31) * octaveFade(8.0, dpx, dpy) +
           0.25 * microValueNoise(p / 4.0 + 3.7) * octaveFade(4.0, dpx, dpy);
}

float3 safeNormalize(float3 v, float3 fallback)
{
    float len2 = dot(v, v);
    if (len2 <= 1e-8) return fallback;
    return v / sqrt(len2);
}

float3 triplanarWeights(float3 worldNormal, float sharpness)
{
    float3 w = abs(worldNormal);
    w        = pow(w, float3(sharpness));
    return w / (w.x + w.y + w.z + 1e-6);
}

// Tangent frame built from a world normal (same construction the CPU
// lattice packs into the per-vertex tangent quaternion, so normal maps
// land on the same frame on both backends).
float3x3 buildTerrainTBN(float3 geomNormal)
{
    float3 N = safeNormalize(geomNormal, float3(0.0, 1.0, 0.0));
    float3 T = float3(1.0, 0.0, 0.0) - N * N.x;
    T        = safeNormalize(T, float3(1.0, 0.0, 0.0));
    float3 B = float3(0.0, 0.0, 1.0) - N * N.z - T * dot(T, float3(0.0, 0.0, 1.0));
    B        = safeNormalize(B, safeNormalize(cross(T, N), float3(0.0, 0.0, 1.0)));
    return float3x3(T, B, N);
}

// ── Pixel main ─────────────────────────────────────────────────────────────
// Output is linear HDR after the tone map: the swapchain RTV is
// RGBA8_UNORM_SRGB, so the driver does the final sRGB encode — exactly
// what the glTF PBR path relies on.
PSTerrainOut main(in PSTerrainIn vs)
{
    float3 worldPos   = vs.WorldPos;
    float3 V          = normalize(g_Frame.Camera.f4Position.xyz - worldPos);
    float3 geomNormal = normalize(vs.Normal);

    // World-space tiled UV for the grass texture (stable across tiles).
    float2 grassUV = worldPos.xz * AZGAAR_GRASS_TILE;

    float3 baseColor;
    float  roughness = 0.9;
    float3 N         = geomNormal;

    // Default grass albedo (rgb) + roughness (a).
    float4 a = g_GrassAlbedo.Sample(g_TilingSampler, grassUV);
    baseColor = a.rgb;
    roughness = clamp(a.a, 0.04, 1.0);

    // Tangent-space normal mapping (frame rebuilt from the world normal;
    // the terrain lattice has no UVs).
    {
        float4 n = g_GrassNormal.Sample(g_TilingSampler, grassUV);
        float2 nxy = (n.rg * 2.0 - 1.0) * SPLAT_NORMAL_STRENGTH;
        float nz = sqrt(max(1.0 - dot(nxy, nxy), 0.0));
        float3x3 TBN = buildTerrainTBN(geomNormal);
        N        = safeNormalize(mul(TBN, float3(nxy, nz)), geomNormal);
    }

    // ── Azgaar climate fields ──────────────────────────────────────────────
    // Static per-world textures sampled through the map-space UV derived
    // from the terrain bounds set at world load. Map +X is world -X and map
    // +Y is world -Z (azgaarMapToWorld), hence the mirrored formulation.
    float2 mapExtent  = mapBounds.zw - mapBounds.xy;
    bool   climateOn  = climateParams.w > 0.5 && mapExtent.x > 1.0 &&
                        mapExtent.y > 1.0;
    float4 climate    = float4(0.0);
    float  landMask   = smoothstep(0.0, 0.2, worldPos.y);  // sea level = 0 m
    float  snowT      = 0.0;
    float  beachT     = 0.0;

    // Map-space UV is always available (the bounds are valid even when the
    // per-world climate fields are off) — the debug views use it too.
    float2 mapUV = float2(0.0);
    bool   mapUVValid = mapExtent.x > 1.0 && mapExtent.y > 1.0;
    if (mapUVValid)
    {
        mapUV = clamp(float2((mapBounds.z - worldPos.x) / mapExtent.x,
                             (mapBounds.w - worldPos.z) / mapExtent.y),
                      0.0, 1.0);
    }

    if (climateOn)
    {
        // 1) Biome tint over the grass base — soft multiply keeps the grass
        // texture detail while the world reads as FMG's authored biome map.
        float3 biomeT = g_BiomeColor.Sample(g_ClampSampler, mapUV).rgb;
        baseColor    = lerp(baseColor, baseColor * (biomeT * 2.0), 0.55);

        climate = g_Climate.Sample(g_ClampSampler, mapUV);
    }

    // ── Turf colour variation (dry-grass patches) ─────────────────────────
    // Coarse world-anchored value noise (12 m / 48 m) splits the ground into
    // patches of dry, sun-bleached straw and surviving green turf. Inserted
    // before the beach/cliff/snow swaps so only the grass base is affected.
    {
        float n = 0.6 * microValueNoise(worldPos.xz / 12.0 + 31.7) +
                  0.4 * microValueNoise(worldPos.xz / 48.0 + 71.3);  // [-1,1]
        float dryMask = smoothstep(0.32, 0.62, 0.5 + 0.5 * n);
        float3 dryColor = float3(0.58, 0.50, 0.32);
        float  wobble   = 0.82 + 0.36 * (0.5 + 0.5 * microValueNoise(worldPos.xz / 4.0 + 13.9));
        baseColor      = lerp(baseColor, dryColor, dryMask) * wobble;
    }

    // 2) Beach band: low land near sea level becomes sand, with a darker
    // wet-sand strip right at the waterline. Driven by the fragment's own
    // height (metres), not the coarse climate grid.
    {
        float beachH = climateParams.z;
        if (beachH > 0.0)
        {
            // The sand band extends ~1.5 m below the waterline: the shallow
            // submerged shelf would otherwise keep the grass tint and read
            // as a green ring seen through the water.
            float beachMask = smoothstep(-1.5, 0.2, worldPos.y);
            beachT = (1.0 - smoothstep(beachH * 0.24, beachH, worldPos.y)) * beachMask;
            float  wetT = (1.0 - smoothstep(0.1, 1.2, worldPos.y)) * landMask;
            float3 sandColor = g_SandAlbedo.Sample(g_TilingSampler, grassUV).rgb;
            baseColor = lerp(baseColor, sandColor, beachT);
            baseColor = lerp(baseColor, sandColor * 0.55, wetT);
        }
    }

    // Slope-based cliff texturing + altitude rock band. The rock weight is
    // the max of the slope blend and a highland band (55%..85% of the
    // world's max land height), so exposed rock appears both on steep faces
    // and on high plateaus above the tree line.
    float rockT = 0.0;
    {
        float  slope      = 1.0 - max(dot(geomNormal, float3(0.0, 1.0, 0.0)), 0.0);
        float  cliffBlend = smoothstep(0.1, 0.4, slope);
        float  rockAlt    = maxLandHeight > 1.0
                                ? smoothstep(0.55, 0.85, worldPos.y / maxLandHeight) * landMask
                                : 0.0;
        rockT            = max(cliffBlend, rockAlt);

        if (rockT > 0.01)
        {
            // Triplanar sampling for the cliff texture.
            float3 w = triplanarWeights(geomNormal, 4.0);
            float2 uvX = worldPos.zy * CLIFF_TRIPLANAR_SCALE;
            float2 uvY = worldPos.xz * CLIFF_TRIPLANAR_SCALE;
            float2 uvZ = worldPos.xy * CLIFF_TRIPLANAR_SCALE;

            float4 cliffAlbedoSample = g_CliffAlbedo.Sample(g_TilingSampler, uvX) * w.x +
                                       g_CliffAlbedo.Sample(g_TilingSampler, uvY) * w.y +
                                       g_CliffAlbedo.Sample(g_TilingSampler, uvZ) * w.z;

            float3 cliffBase             = cliffAlbedoSample.rgb;
            float  cliffRoughnessSample  = clamp(cliffAlbedoSample.a, 0.04, 1.0);

            baseColor = lerp(baseColor, cliffBase, rockT);
            roughness = lerp(roughness, cliffRoughnessSample, rockT);

            float4 cliffNormalSample = g_CliffNormal.Sample(g_TilingSampler, uvX) * w.x +
                                       g_CliffNormal.Sample(g_TilingSampler, uvY) * w.y +
                                       g_CliffNormal.Sample(g_TilingSampler, uvZ) * w.z;

            float2 nxy = cliffNormalSample.rg * 2.0 - 1.0;
            nxy *= SPLAT_NORMAL_STRENGTH;
            float nz = sqrt(max(1.0 - dot(nxy, nxy), 0.0));
            float3 cliffTangentNormal = normalize(float3(nxy, nz));

            float3x3 TBN = buildTerrainTBN(geomNormal);
            float3 cliffWorldNormal = normalize(mul(TBN, cliffTangentNormal));

            N = normalize(lerp(N, cliffWorldNormal, rockT));
        }
    }

    // 3) Snow line — last so the peaks stay white over rock. FMG's cell
    // temperature already falls with altitude, so the isotherm band gives a
    // natural altitude snow line; the Glacier biome is snow
    // unconditionally. A value-noise breakup keeps the line from reading as
    // a contour.
    if (climateOn)
    {
        // The A channel carries discrete biome ids (nearest-cell values),
        // so it must be read through a NEAREST sampler.
        float4 climateNearest = g_ClimateNearest.Sample(g_ClampNearestSampler, mapUV);
        float   tempC   = climate.r * 255.0 - AZGAAR_CLIMATE_TEMP_BIAS;
        float   biomeId = floor(climateNearest.a * 255.0 + 0.5);
        float   snowLo  = climateParams.x;
        float   snowHi  = climateParams.y;

        snowT = 1.0 - smoothstep(snowLo, snowHi, tempC);
        snowT = max(snowT,
                    (biomeId > AZGAAR_BIOME_GLACIER - 0.5 &&
                     biomeId < AZGAAR_BIOME_GLACIER + 0.5) ? 1.0 : 0.0);
        snowT *= landMask;
        snowT *= 0.75 + 0.25 * (0.5 + 0.5 * microValueNoise(worldPos.xz * 0.02));

        if (snowT > 0.004)
        {
            float3 snowColor = g_SnowAlbedo.Sample(g_TilingSampler, grassUV).rgb;
            baseColor = lerp(baseColor, snowColor, snowT);
            // Snow normal stays flat (the micro-band noise below still
            // applies, which reads well on snow).
            N = normalize(lerp(N, geomNormal, snowT));
        }
    }

    // Roughness follows the same material chain (snow smooth, sand rough).
    roughness = lerp(roughness, 0.6, snowT);
    roughness = lerp(roughness, 0.85, beachT);

    // Micro-band roughness: perturb the shading normal with the 4-32 m value
    // noise (finite differences, 0.5 m step). Does not move geometry. Two
    // guards keep the GGX lobe from smudging:
    //   1) octaveFade (screen-space derivatives) fades sub-pixel octaves out;
    //   2) grazing gain: the tilt is only useful where the detail reads
    //      (steep view), so it attenuates to zero as the view goes grazing
    //      (keyed on the stable pre-perturbation normal).
    {
        const float e = 0.5;
        float2 p     = worldPos.xz;
        float dpx = length(ddx(p));
        float dpy = length(ddy(p));
        float hC = microBumpFaded(p, dpx, dpy);
        float hX = microBumpFaded(p + float2(e, 0.0), dpx, dpy);
        float hZ = microBumpFaded(p + float2(0.0, e), dpx, dpy);
        float3 microN = float3((hC - hX) / e, 0.0, (hC - hZ) / e);

        float   geomNdotV     = max(dot(geomNormal, V), 0.0);
        float   grazingGain    = smoothstep(0.05, 0.30, geomNdotV);
        N                      = safeNormalize(N + microN * (MICRO_NOISE_STRENGTH * grazingGain), N);
    }

    // ── Debug views (terrain.mat parity; flat matte, not emissive) ────────
    bool   dbgOn    = debugView > 0.5;
    float3 dbgColor = float3(1.0);
    if (dbgOn)
    {
        if (debugView < 1.5)
        {
            dbgColor = 0.5 + 0.5 * cos(6.2831853 * (worldPos.y * (1.0 / 256.0) + float3(0.0, 0.33, 0.67)));
        }
        else if (mapUVValid)
        {
            dbgColor = g_BiomeColor.Sample(g_ClampSampler, mapUV).rgb;
        }
    }

    // ── DiligentFX PBR lighting (same pipeline as the glTF path) ─────────
    // Dielectric (metallic 0) like terrain.mat's `lit` + prepareMaterial.
    float3  baseColorF = dbgOn ? dbgColor : baseColor;
    float   roughnessF = dbgOn ? 1.0 : roughness;
    float3  Nf         = dbgOn ? geomNormal : N;

    SurfaceReflectanceInfo srf = GetSurfaceReflectanceMR(baseColorF, 0.0, roughnessF);

    BaseLayerShadingInfo base;
    base.Metallic = 0.0;
    base.Srf      = srf;
    base.Normal   = Nf;
    base.NdotV    = dot_sat(Nf, V);

    SurfaceShadingInfo shading;
    shading.Pos       = worldPos;
    shading.View      = V;
    shading.Occlusion = 1.0;
    shading.Emissive  = float3(0.0);
    shading.BaseLayer = base;
    shading.IBLScale  = g_Frame.Renderer.IBLScale.rgb;

    SurfaceLightingInfo lighting = GetDefaultSurfaceLightingInfo();

    // Directional sun (the light attribs pair right after the frame
    // attribs, filled like GltfDiligent.cpp fillFrameAttribs).
    ApplyPunctualLight(shading, g_Sun, lighting);

    // Constant-environment IBL: 1x1x6 irradiance + prefiltered cubes from
    // the engine ambient, split-sum against the shared preintegrated GGX
    // LUT. PrefilteredCubeLastMip = 0 for the single-mip constant env.
    ApplyIBL(shading,
             g_Frame.Renderer.PrefilteredCubeLastMip,
             g_Frame.Renderer.EnvironmentRotation,
             g_PreintegratedGGX, g_ClampSampler,
             g_IblIrradiance, g_ClampSampler,
             g_IblPrefiltered, g_ClampSampler,
             lighting);

    float3 color = ResolveLighting(shading, lighting);

    // Same tone map as the glTF PBR frame (Uncharted2, fixed exposure).
    ToneMappingAttribs tm;
    tm.iToneMappingMode     = TONE_MAPPING_MODE_UNCHARTED2;
    tm.bAutoExposure        = false;
    tm.fMiddleGray          = g_Frame.Renderer.MiddleGray;
    tm.bLightAdaptation     = false;
    tm.fWhitePoint          = g_Frame.Renderer.WhitePoint;
    tm.fLuminanceSaturation = 1.0;
    color = ToneMap(color, tm, g_Frame.Renderer.AverageLogLum);

    PSTerrainOut result;
    result.Color = float4(color, 1.0);
    return result;
}
