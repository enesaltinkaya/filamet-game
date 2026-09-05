// RmlUi GUI vertex shader (diligent backend).
//
// The crmlui wrapper's RmlVertex is a 20-byte packed struct (float2 position,
// 4-byte Colourb, float2 texcoord, 4-byte aligned) and maps 1:1 onto this
// input layout: the colour's RGBA8 bit pattern is read through a float
// element (asuint) and unpacked in the shader — no CPU conversion.
//
// Positions arrive with the per-geometry translation already baked on the
// CPU (the wrapper's RenderGeometry translationX/Y); the per-frame document
// transform (usually identity or a uiScale) and the ortho projection come
// from the frame cbuffer. The matrices are stored TRANSPOSED — the runtime-
// compiled HLSL path (glslang) consumes cbuffer matrices transposed
// relative to Diligent's row-major math (docs/lessons.md, the 2026-09-05
// transpose entry), and the transform is applied row-vector style like
// Diligent's: clip = mul(mul(p, transform), proj).
//
// Input layout (PSO): one VBO, 20-byte stride
//   ATTRIB0 float2 @0   (position + translation, RmlUi units)
//   ATTRIB1 float  @8   (packed RGBA8 colour bit pattern)
//   ATTRIB2 float2 @12  (texcoord)
// Indices: u32, bound with SetIndexBuffer.

cbuffer cbRmluiFrame
{
    float4x4 g_OrthoProj;  // y-down ortho (Diligent NDC), clip.z constant
    float4x4 g_Transform;  // per-frame document transform (row-vector)
};

struct VSRmluiIn
{
    float2 Position : ATTRIB0;
    float  Colour   : ATTRIB1;
    float2 TexCoord : ATTRIB2;
};

struct PSRmluiIn
{
    float4 Position : SV_Position;
    float4 Colour   : TEXCOORD0;
    float2 TexCoord : TEXCOORD1;
};

PSRmluiIn main(in VSRmluiIn In)
{
    PSRmluiIn Out;
    float4 p = mul(float4(In.Position, 0.0, 1.0), g_Transform);
    Out.Position = mul(p, g_OrthoProj);

    // Colourb is RGBA (little-endian): r = byte 0, a = byte 3.
    uint c = asuint(In.Colour);
    Out.Colour = float4(
            float(c & 0xFFu) / 255.0,
            float((c >> 8) & 0xFFu) / 255.0,
            float((c >> 16) & 0xFFu) / 255.0,
            float((c >> 24) & 0xFFu) / 255.0);
    Out.TexCoord = In.TexCoord;
    return Out;
}
