// Azgaar props SHADOW vertex shader (diligent backend, DiligentFX CSM).
// Depth-only instanced transform that MIRRORS the lit props VS
// (props_vs.hlsl): same per-instance texel fetch, yaw rotation, wind sway
// and player-reaction push, so the shadow map depth matches the lit
// geometry. The differences to the lit VS: no TAA prev-clip output and the
// transform target is the cascade's light view-projection (cbShadowPass,
// filled per cascade by ShadowDiligent.cpp — mLightViewProj transposed, the
// runtime glslang convention) instead of the camera view-projection. The
// pixel stage (props_shadow_ps.hlsl) alpha-discards cutout cards / flower
// discs — this VS forwards the UV + per-instance flags for it.
// Per-instance texel layout: see props_vs.hlsl.

cbuffer cbShadowPass
{
    float4x4 mLightViewProj; // world(render space) -> light projection
    float4   f4AnchorHi;     // f32(anchor)
    float4   f4AnchorLo;     // anchor - f32(anchor)
    float4   f4Wind;         // (dir.xy, speed, gust-modulated strength)
    float4   f4Phase;        // (integrated sway phase rad, 0, 0, 0)
    float4   f4Player;       // (feet.xyz, horizontal speed m/s)
};

Texture2D<float4> g_InstanceTex;

struct VSPropsShadowIn
{
    float3 Position : ATTRIB0; // mesh local (metres, base y = 0, unit height)
    float4 Normal   : ATTRIB1; // unused (depth-only)
    float2 UV       : ATTRIB2; // forwarded to the cutout PS
    float4 PartColor: ATTRIB3; // unused
};

struct PSPropsShadowIn
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
    nointerpolation uint Flags : TEXCOORD1;
};

PSPropsShadowIn main(in VSPropsShadowIn In, uint InstId : SV_InstanceID)
{
    PSPropsShadowIn Out;
// Value check, not #ifdef: a macro added with value 0 (AddShaderMacro) is
// still "defined", so #ifdef would take the bare branch while the normal
// path stays out (or vice versa) — 't0' unknown variable, whole VS fails.
// The ENTIRE normal body sits in the #else: with the bare branch active
// its declarations are out of scope.
#if defined(PROPS_SHADOW_DEBUG_BARE) && PROPS_SHADOW_DEBUG_BARE == 1
    // Bisect: the lattice lifted 5 m — the whole map's depth must shift.
    // Flags 0 keeps the cutout PS a pure passthrough on this cross-test.
    float3 relB = In.Position - (f4AnchorHi.xyz + f4AnchorLo.xyz) + float3(0, 5, 0);
    Out.Position = mul(float4(relB, 1.0), mLightViewProj);
    Out.UV       = float2(0.0, 0.0);
    Out.Flags    = 0u;
    return Out;
#else
    uint lin = InstId * 4u;
    uint tx  = lin & 2047u;
    uint ty  = lin >> 11u;
    float4 t0 = g_InstanceTex.Load(int3(tx,     ty, 0));
    float4 t1 = g_InstanceTex.Load(int3(tx + 1, ty, 0));
    float4 t2 = g_InstanceTex.Load(int3(tx + 2, ty, 0));
    float4 t3 = g_InstanceTex.Load(int3(tx + 3, ty, 0));

    float3  iPos  = t0.xyz;
    float   yaw   = t0.w;
    float   scale = t1.x;
    float   phase = t2.x;
    float   bMinY = t2.y;
    float   bMaxY = t2.z;
    float   swayF = t2.w;
    uint    flags = asuint(t3.x);

    float cy = cos(yaw);
    float sy = sin(yaw);
    float3 local = In.Position;
    float3 rot   = float3(cy * local.x + sy * local.z, local.y,
                          -sy * local.x + cy * local.z) * scale;

    float3 rel = iPos - f4AnchorHi.xyz - f4AnchorLo.xyz;

    // Wind sway: height-weighted (h^2, base anchored), de-synced per
    // instance by the packed phase — identical math to the lit VS.
    float span  = max(bMaxY - bMinY, 1e-3);
    float hN    = clamp((In.Position.y - bMinY) / span, 0.0, 1.0);
    float swayW = hN * hN * swayF;
    float sway  = sin(f4Phase.x + phase) * f4Wind.w * swayW;

    float3 worldRel = rel + rot;
    worldRel.xz += f4Wind.xy * sway;

    // Player reaction push (the lit VS' kPlayer* constants).
    const float kPlayerReach      = 2.0f;
    const float kPlayerBase       = 0.15f;
    const float kPlayerSpeedScale = 0.05f;
    float pDist = length(iPos - f4Player.xyz);
    float pFall = 1.0 - smoothstep(0.0, kPlayerReach, pDist);
    float pAmp  = (kPlayerBase + kPlayerSpeedScale * f4Player.w) * pFall * swayW;
    float2 pDir = (iPos.xz - f4Player.xz) / max(pDist, 1e-3);
    worldRel.xz += pDir * pAmp;

    Out.Position = mul(float4(worldRel, 1.0), mLightViewProj);
    Out.UV       = In.UV;
    Out.Flags    = flags;
    return Out;
#endif
}
