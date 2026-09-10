cbuffer cbSplatShadowCaster
{
    float4x4 cLightViewProj;
    float4   g_Anchor;
};

struct VSSplatShadowIn
{
    float3 Position : ATTRIB0;
    float3 Normal   : ATTRIB1;
    float4 Tangent  : ATTRIB2;
    float2 TexCoord : ATTRIB3;
};

struct PSSplatShadowIn
{
    float4 Position : SV_Position;
};

PSSplatShadowIn main(in VSSplatShadowIn In)
{
    PSSplatShadowIn Out;
    float3 p = In.Position - g_Anchor.xyz;
    Out.Position = mul(float4(p, 1.0), cLightViewProj);
    return Out;
}
