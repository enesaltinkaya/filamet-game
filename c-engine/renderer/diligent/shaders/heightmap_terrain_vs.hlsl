// Heightmap terrain vertex shader (diligent backend).
//
// Thin transform of the CPU lattice corners: the world-space position and
// the border-aware stencil normal are precomputed per tile on the CPU
// (ecs/system/heightmap/HeightmapLattice.h), so the vertex stage does
// nothing but clip. Every look decision lives in the pixel stage
// (heightmap_terrain_ps.hlsl), which rebuilds the tangent frame from the
// normal like terrain.mat's buildTerrainTBN.
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
// The view/proj matrices come from the shared PBR frame attribs cbuffer
// (filled in HeightmapTerrainDiligent.cpp fillFrameAttribs — transposed,
// see the note there) — no separate uniform.

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
    float3 WorldPos : WORLD_POS;
    float3 Normal   : NORMAL;
};

PSTerrainIn main(in VSTerrainIn In)
{
    PSTerrainIn Out;
    Out.Position = mul(float4(In.Position, 1.0), g_Frame.Camera.mViewProj);
    Out.WorldPos = In.Position;
    Out.Normal   = In.Normal;
    return Out;
}
