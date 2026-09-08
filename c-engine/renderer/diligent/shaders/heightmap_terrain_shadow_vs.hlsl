// Heightmap terrain SHADOW vertex shader (diligent backend, DiligentFX CSM).
// Depth-only transform of the CPU lattice corners into one cascade's light
// view-projection. Same clip-space conventions as the lit terrain VS
// (heightmap_terrain_vs.hlsl): the anchor split (f32 high + sub-mm
// residual) is subtracted before the light transform, so the shadow map is
// built in the same render space the lit pass samples it in (the camera
// eye is the render-space origin; the cascade distribution runs on the
// rotation-only view). The cbuffer is the shadow pass' shared
// cbShadowPass (filled per cascade by ShadowDiligent.cpp — mLightViewProj
// transposed, the runtime glslang convention).

cbuffer cbShadowPass
{
    float4x4 mLightViewProj; // world(render space) -> light projection
    float4   f4AnchorHi;     // f32(anchor)
    float4   f4AnchorLo;     // anchor - f32(anchor)
    float4   f4Wind;         // unused here (props shadow VS mirrors the lit sway)
    float4   f4Phase;
    float4   f4Player;
};

struct VSTerrainShadowIn
{
    float3 Position : ATTRIB0; // world metres
    float3 Normal   : ATTRIB1; // unused (depth-only)
};

float4 main(in VSTerrainShadowIn In) : SV_Position
{
    float3 rel = In.Position - f4AnchorHi.xyz - f4AnchorLo.xyz;
    return mul(float4(rel, 1.0), mLightViewProj);
}
