// RmlUi GUI fragment shader (diligent backend).
//
// Straight multiply of the vertex colour by the texture (the old engine's
// rmlui fragment.frag, minus its GL-specific gamma branch). UI textures are
// plain RGBA8_UNORM and the output goes raw into the sRGB swapchain — the
// same convention as the in-tree ImGui pass (raw 0..1 UI colours into an
// sRGB framebuffer, no linearization); alpha blends with
// (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) in the PSO.
//
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
