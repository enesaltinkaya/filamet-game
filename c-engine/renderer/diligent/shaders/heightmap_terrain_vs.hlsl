// Heightmap terrain vertex shader (diligent backend).
//
// Thin transform of the CPU lattice corners: the world-space position and
// the border-aware stencil normal are precomputed per tile on the CPU
// (ecs/system/heightmap/HeightmapLattice.h), so the vertex stage does
// nothing but clip — in ANCHOR space. The view matrix is rotation-only
// (the camera is the render-space origin — docs/lessons.md, the 2026-09-04
// f32 entry: at |world| ~ 4e4 m an absolute f32 view translation cancels
// to a 3.9 mm grid and shimmers the scene), so the anchor is subtracted
// before the view transform to bring the tile into the near/far range. The
// anchor arrives SPLIT (f4ExtraData[3] = f32(anchor), f4ExtraData[4] = the
// sub-mm residual anchor − f32(anchor)): subtracting the high part is exact
// (same binade) and the residual kills the f32(anchor) ULP step, so the
// ground does not re-shimmer by 3.9 mm on every camera ULP crossing. The
// absolute position is passed through for the pixel stage's world-anchored
// lookups (map UV, grass tiling, value noise — the aperiodic ones must stay
// on world xz, see the 2026-09-04 lesson) and its view direction.
//
// Compiled at pass init via device->CreateShader:
//   SourceLanguage = SHADER_SOURCE_LANGUAGE_HLSL
//   pShaderSourceStreamFactory = &DiligentFXShaderSourceStreamFactory::GetInstance()
//   (the "Name.fxh" includes resolve against that factory's embedded list)
//   EntryPoint = "main", SHADER_TYPE_VERTEX.
//
// Input layout (PSO): slot 0 = tile VBO, (float3 Position, float3 Normal),
// 24 B stride, stream size per-tile; the shared lattice IBO is bound with
// SetIndexBuffer (no input-layout slot of its own on this path).
// The input struct uses : ATTRIBn semantics — the only style the
// Vulkan backend's input layout accepts (DiligentFX convention; the
// PSO's layout elements use the "ATTRIB" semantic with InputIndex n).
//
// The view/proj matrices and the anchor split come from the shared PBR
// frame attribs cbuffer (filled in HeightmapTerrainDiligent.cpp
// fillFrameAttribs — transposed, see the note there) — no separate uniform.

#include "BasicStructures.fxh"
#include "PBR_Structures.fxh"
#include "RenderPBR_Structures.fxh"

cbuffer cbFrameAttribs
{
    PBRFrameAttribs g_Frame;
};

struct VSTerrainIn
{
    float3 Position : ATTRIB0; // world metres
    float3 Normal   : ATTRIB1; // world-space, border-aware stencil normal
};

struct PSTerrainIn
{
    float4 Position : SV_Position;
    float3 WorldPos : WORLD_POS; // absolute world metres (PS lookups)
    float3 Normal   : NORMAL;
};

PSTerrainIn main(in VSTerrainIn In)
{
    PSTerrainIn Out;
    // Camera-anchored clip: subtract the anchor split (high + residual)
    // to bring the tile into the near/far range, then the rotation-only
    // view.
    float3 rel = In.Position - g_Frame.Camera.f4ExtraData[3].xyz
                         - g_Frame.Camera.f4ExtraData[4].xyz;
    Out.Position = mul(float4(rel, 1.0), g_Frame.Camera.mViewProj);
    Out.WorldPos = In.Position;
    Out.Normal   = In.Normal;
    return Out;
}
