// Splat terrain vertex shader (diligent backend, runtime-compiled HLSL —
// the rmlui_ui_* pattern: the source loads from the packed materials/ dir
// and compiles through glslang).
//
// The SplatVertex (48 B, SplatTerrainDiligent.h) is world-space (absolute,
// NOT anchor-subtracted): the per-frame f64 camera-eye anchor comes through
// the cbuffer as f32 and the VS subtracts it here, so the rendered numbers
// stay small without a per-frame buffer rebuild (the PBR pass rebuilds its
// poses for the same reason, docs/lessons.md 2026-09-04 f32 entry).
//
// The matrices are stored TRANSPOSED — the runtime-compiled HLSL path
// (glslang) consumes cbuffer matrices transposed relative to Diligent's
// row-major math (docs/lessons.md, the 2026-09-05 transpose entry), and the
// transform is applied row-vector style: clip = mul(mul(p, cCamView), cCamProj).
// cCamView is rotation-only (the camera-anchored view: the eye is the origin
// of rendered space), cCamProj the TAA-jittered projection.
//
// The cbuffer is a flat mirror of HLSL::PBRFrameAttribs (the PBR pass' frame
// struct, SplatTerrainDiligent.cpp fills it with the same getters as
// GltfDiligent.cpp fillFrameAttribs, matrices transposed for this path) plus
// the splat g_Anchor — the VS only reads cCamView/cCamProj/g_Anchor, the PS
// reads the light/shadow/IBL parts. The declaration must stay field-identical
// in both shaders.
//
// Input layout (PSO): one VBO, 48-byte stride
//   ATTRIB0 float3 @0   world position
//   ATTRIB1 float3 @12   world normal
//   ATTRIB2 float4 @24   tangent (glTF TANGENT: xyz + handedness)
//   ATTRIB3 float2 @40   the chunker-remapped [0,1]^2 splat uv
// Indices: u16, bound with SetIndexBuffer.
//
// The UDIM uv is emitted in grid space (10 * the remapped uv — the chunker
// mapped the original UDIM udimU = 10*u, udimV = 10*v_uv - 9 extent
// {0..10} x {-9..1} linearly into [0,1]^2, so 10 * uv.x / uv.y are the
// column/row-space coordinates 0..10). The PS does the tile pick:
// col = clamp(floor(x), 0, 9), row = clamp(floor(y), 0, 9) — the clamps land
// AFTER the floor because chunk-boundary uvs overshoot by ~0.002 and must
// map to tile 9's edge, not to the grid's left/bottom corner.

cbuffer cbSplatFrame
{
    float4   cCamPosition;
    float4   cCamViewport;
    float4   cCamClip;
    float4   cCamScene;
    float4   cCamHand;
    float4   cCamFocus;
    float4   cCamSensor;
    float4x4 cCamView;
    float4x4 cCamProj;
    float4x4 cCamViewProj;
    float4x4 cCamViewInv;
    float4x4 cCamProjInv;
    float4x4 cCamViewProjInv;
    float4   cCamExtra[5];
    float4   pCamPosition;
    float4   pCamViewport;
    float4   pCamClip;
    float4   pCamScene;
    float4   pCamHand;
    float4   pCamFocus;
    float4   pCamSensor;
    float4x4 pCamView;
    float4x4 pCamProj;
    float4x4 pCamViewProj;
    float4x4 pCamViewInv;
    float4x4 pCamProjInv;
    float4x4 pCamViewProjInv;
    float4   pCamExtra[5];
    float4   rCam;
    float4   rIBLScale;
    float4   rEnvRot;
    float4   rMisc;
    float4   rLight;
    float4   rUnshaded;
    float4   rHighlight;
    float4   rLoadA;
    float4   rLoadB;
    float4   rLoadC;
    float4   lTypePos;
    float4   lDir;
    float4   lInt;
    float4   lSpot;
    float4x4 sWorldToLightProj;
    float4   sUV;
    float4   sSlice;
    float4   g_Anchor;
};

struct VSSplatIn
{
    float3 Position : ATTRIB0;
    float3 Normal   : ATTRIB1;
    float4 Tangent  : ATTRIB2;
    float2 TexCoord : ATTRIB3;
};

struct PSSplatIn
{
    float4 Position    : SV_Position;
    float3 AnchoredPos : TEXCOORD0;  // world - anchor
    float3 WorldNormal : TEXCOORD1;
    float4 Tangent     : TEXCOORD2;
    float2 UdimUv      : TEXCOORD3;  // 10 * TexCoord (0..10 grid space)
    float4 PrevClip    : TEXCOORD4;  // anchor-corrected prev-frame clip
};

PSSplatIn main(in VSSplatIn In)
{
    PSSplatIn Out;
    float3 p = In.Position - g_Anchor.xyz;
    Out.Position    = mul(mul(float4(p, 1.0), cCamView), cCamProj);
    Out.AnchoredPos = p;
    Out.WorldNormal = In.Normal;
    Out.Tangent     = In.Tangent;
    Out.UdimUv      = In.TexCoord * 10.0;
    Out.PrevClip    = mul(float4(p, 1.0), pCamViewProj);
    return Out;
}
