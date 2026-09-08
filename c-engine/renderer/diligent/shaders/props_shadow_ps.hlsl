// Azgaar props SHADOW pixel shader (diligent backend, DiligentFX CSM).
// Cutout stage for the depth-only shadow pipeline: without a pixel stage
// the alpha-tested cards (flag bit 0) and flower discs (flag bit 2) wrote
// their full square into the cascade atlas. Mirrors the lit props_ps.hlsl
// discards (hard 0.5 alpha test, 0.30-unit flower disc radius) so the
// shadow silhouette matches the lit one. Procedural (non-textured)
// variants bind the white 1x1 fallback (alpha 1, no flags) and pass
// through. Colour output is ignored (NumRenderTargets = 0) — only the
// discard matters.

Texture2D g_BaseTex;
SamplerState g_BaseSampler;

struct PSPropsShadowIn
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
    nointerpolation uint Flags : TEXCOORD1;
};

float4 main(in PSPropsShadowIn In) : SV_Target0
{
    if ((In.Flags & 1u) != 0u)
    {
        float4 tex = g_BaseTex.Sample(g_BaseSampler, In.UV);
        if (tex.a < 0.5) discard;
    }
    if ((In.Flags & 4u) != 0u && length(In.UV - 0.5) > 0.30) discard;
    return float4(0.0, 0.0, 0.0, 1.0);
}
