// RmlUi GUI fragment shader (diligent backend).
// Straight multiply of the vertex colour by the texture (the old engine's
// rmlui fragment.frag). File-loaded UI textures are RGBA8_UNORM_SRGB —
// sampling decodes sRGB→linear and the sRGB swapchain (RGBA8_UNORM_SRGB,
// auto-encode on write) re-encodes, so image colours round-trip exactly
// like the old engine's BC7_SRGB path (createRgba8's srgb flag in
// RmluiDiligent.cpp). A plain UNORM texture here double-encodes on the
// sRGB swapchain: washed-out logo, stored 13 → displayed 62.
// The white fallback + font atlases stay UNORM (white texels are 1.0 in
// either space); vertex colours stay raw (old-engine behaviour). Alpha
// blends with (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) in the PSO.
// Texture/sampler are bound per draw: the texture SRV is a SETTABLE pipeline
// resource (one per texture batch — consecutive RmlUi geometry with the
// same texture merges into one draw), the linear clamp sampler is static.

// Must match the VS output struct (shaders compile separately — no shared
// header on this path; keep the two in sync).
struct PSRmluiIn
{
    float4 Position : SV_Position;
    float4 Colour   : TEXCOORD0;
    float2 TexCoord : TEXCOORD1;
};

Texture2D<float4> g_UiTex;
SamplerState      g_UiSampler;

float4 main(PSRmluiIn In) : SV_Target
{
    return In.Colour * g_UiTex.Sample(g_UiSampler, In.TexCoord);
}
