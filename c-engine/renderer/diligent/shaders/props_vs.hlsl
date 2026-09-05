// Azgaar props vertex shader (diligent backend).
//
// One instanced draw per (tile, species, variant) range: the merged species
// mesh supplies ATTRIB0..3, the range's instances live in a per-tile
// RGBA32F texture fetched with SV_InstanceID (per-instance vertex
// attributes silently fed nothing on this backend's runtime-HLSL/Vulkan
// input-layout path — docs/lessons.md, the 2026-09-04 f32 entry).
//
// The per-instance texels are packed by the pass at tile-apply time, 4
// RGBA32F per instance (the width, 2048, is a multiple of 4, so an
// instance's texels never cross a row):
//   t0 = pos.xyz (absolute world metres), yaw
//   t1 = scale (target height m; the mesh is unit-height), tint.rgb
//   t2 = wind phase (rad), boundsMinY, boundsMaxY, swayFactor
//   t3 = flags (bit0 alpha-test, bit1 thin double-sided, bit2 flower disc,
//        bit3 textured — asuint), 0, 0, 0
// bounds*/sway/flags are per-(species,variant) constants baked per instance
// so the per-draw state is ONLY the instance texture + the base texture
// (a second per-draw cbuffer bound ambiguously on this path —
// docs/lessons.md, the 2026-09-05 second-cbuffer entry).
//
// The instance position stays ABSOLUTE f32 in the texture: the camera
// anchor arrives split (f4ExtraData[3] = f32(anchor), f4ExtraData[4] = the
// sub-mm residual) exactly as the terrain VS consumes it; the high-part
// subtraction is exact (Sterbenz — anchor and visible props share a binade)
// and the residual kills the f32(anchor) ULP step. The view matrix is
// rotation-only (the camera eye is the render-space origin).
//
// Wind sway is the old engine's azgaar_props.vert port: height-weighted
// (h², base anchored) sin drift along the world wind direction, de-synced
// per instance by `phase`; the CPU integrates the gust-modulated phase so
// the field breathes. Plus the player reaction — a horizontal push away
// from the player's feet that parts the vegetation while standing and
// swishes harder while moving (f4ExtraData[2] = feet.xyz, horizontal
// speed). Wind (dir.xy, speed, strength) and the phase ride in
// f4ExtraData[0]/[1].

#include "BasicStructures.fxh"
#include "PBR_Structures.fxh"
#include "RenderPBR_Structures.fxh"

cbuffer cbFrameAttribs
{
    PBRFrameAttribs g_Frame;
};

Texture2D<float4> g_InstanceTex;

struct VSPropsIn
{
    float3 Position  : ATTRIB0;  // mesh local (metres, base y = 0, unit height)
    float4 Normal    : ATTRIB1;  // xyz normal (w = 0)
    float2 UV        : ATTRIB2;
    float4 PartColor : ATTRIB3;  // rgb baked part colour (w = 1; white = tintable)
};

struct PSPropsIn
{
    float4 Position  : SV_Position;
    float3 WorldPos  : WORLD_POS; // absolute world metres (PS view direction)
    float3 Normal    : NORMAL;
    float3 Tint      : TINT;      // per-instance biome tint
    float2 UV        : UV;
    float3 PartColor : PART_COLOR;
    nointerpolation uint Flags : FLAGS;
};

PSPropsIn main(in VSPropsIn In, uint InstId : SV_InstanceID)
{
    PSPropsIn Out;

    // Per-instance texel fetch (2048 texels/row, 4 texels per instance).
    uint lin = InstId * 4u;
    uint tx  = lin & 2047u;
    uint ty  = lin >> 11u;
    float4 t0 = g_InstanceTex.Load(int3(tx,     ty, 0));
    float4 t1 = g_InstanceTex.Load(int3(tx + 1, ty, 0));
    float4 t2 = g_InstanceTex.Load(int3(tx + 2, ty, 0));
    float4 t3 = g_InstanceTex.Load(int3(tx + 3, ty, 0));

    float3  iPos    = t0.xyz;
    float   yaw     = t0.w;
    float   scale   = t1.x;
    float3  tint    = t1.yzw;
    float   phase   = t2.x;
    float   bMinY   = t2.y;
    float   bMaxY   = t2.z;
    float   swayF   = t2.w;
    uint    flags   = asuint(t3.x);

    // Yaw rotation around Y (explicit math: no matrix-convention ambiguity
    // on the glslang HLSL path). Mesh base sits on the ground plane, so the
    // rotation pivots at the instance origin.
    float cy = cos(yaw);
    float sy = sin(yaw);
    float3 local = In.Position;
    float3 rot   = float3(cy * local.x + sy * local.z, local.y,
                          -sy * local.x + cy * local.z) * scale;
    float3 nrm   = float3(cy * In.Normal.x + sy * In.Normal.z, In.Normal.y,
                          -sy * In.Normal.x + cy * In.Normal.z);

    // Camera-anchored clip: subtract the anchor split from the absolute
    // instance position BEFORE adding the (small) local offset.
    float3 rel = iPos - g_Frame.Camera.f4ExtraData[3].xyz
                        - g_Frame.Camera.f4ExtraData[4].xyz;

    // Wind sway: weight by the fraction of the mesh height (authored space,
    // so it is identical for unit-height and future hand-authored models),
    // squared so the base stays anchored. The phase (rad) is integrated on
    // the CPU with the gust-modulated speed (f4ExtraData[1].x); strength is
    // gust-modulated too (f4ExtraData[0].w).
    float4 wind = g_Frame.Camera.f4ExtraData[0];
    float  span   = max(bMaxY - bMinY, 1e-3);
    float  hN     = clamp((In.Position.y - bMinY) / span, 0.0, 1.0);
    float  swayW  = hN * hN * swayF;
    float  sway   = sin(g_Frame.Camera.f4ExtraData[1].x + phase) * wind.w * swayW;

    float3 worldRel = rel + rot;
    worldRel.xz += wind.xy * sway;

    // Player reaction (port of the old engine's azgaar_props.vert term): a
    // horizontal push away from the player's feet — the vegetation parts
    // around a standing player (kPlayerBase) and swishes harder while they
    // move (kPlayerSpeedScale * horizontal speed m/s). The falloff is the 3D
    // distance, so vegetation on a hill below the player does not react;
    // weighted by swayW, static species (rocks) are unaffected.
    float4 player = g_Frame.Camera.f4ExtraData[2]; // xyz feet (m), w speed (m/s)
    const float kPlayerReach       = 2.0f;  // old PROPS_PLAYER_REACH (m)
    const float kPlayerBase        = 0.15f; // old PROPS_PLAYER_BASE (m)
    const float kPlayerSpeedScale  = 0.05f; // old PROPS_PLAYER_SPEED_SCALE (m per m/s)
    float pDist = length(iPos - player.xyz);
    float pFall = 1.0 - smoothstep(0.0, kPlayerReach, pDist);
    float pAmp  = (kPlayerBase + kPlayerSpeedScale * player.w) * pFall * swayW;
    float2 pDir = (iPos.xz - player.xz) / max(pDist, 1e-3);
    worldRel.xz += pDir * pAmp;

    Out.Position = mul(float4(worldRel, 1.0), g_Frame.Camera.mViewProj);
    Out.WorldPos = iPos + rot;
    Out.WorldPos.xz += wind.xy * sway + pDir * pAmp;
    Out.Normal    = nrm;
    Out.Tint      = tint;
    Out.UV        = In.UV;
    Out.PartColor = In.PartColor.xyz;
    Out.Flags     = flags;
    return Out;
}
