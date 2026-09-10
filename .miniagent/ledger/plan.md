# Plan: band textures on terrain (cliff / snow / sand)

Task: while rendering terrain, use the cliff texture on steep slopes, snow on high places, sand on low places.

Current state (from the aborted 2026-09-10 session): the C++ side is already done —
`c-engine/renderer/diligent/SplatTerrainDiligent.cpp` (~L750–810) loads the three band
detail sets (`images/terrain/snow_default|sand_default|cliff_side_default/{albedo,normal}.ktx2`,
in pak_0_engine) into `t->band[3]` and (~L1349–1361) binds them as
`g_SnowAlbedo/g_SnowNormal/g_SandAlbedo/g_SandNormal/g_CliffAlbedo/g_CliffNormal` in the
splat SRB; the resource signature (~L1282–1293) already lists all six. What is missing is
entirely in the pixel shader: `c-game/data/pak_1/materials/splat_terrain_ps.hlsl` does not
declare or sample those six textures.

Approach: in the PS, declare the six `Texture2D`s, then after the existing splat chain +
base-detail blend, compute three band weights and blend them into albedo and tangent
normal, in the old-engine order sand → cliff → snow (old-engine parity:
`game-001-cpp .../heightmap_terrain.frag` — sand: low land near sea level 0 m with
`landMask = smoothstep(0.0, 0.2, worldY)`; cliff: `slope = 1 - max(worldNormal.y, 0)`,
`smoothstep(0.1, 0.4, slope)`; snow: altitude band — smoothstep over normalized world
height, e.g. ~0.55–0.85 of max land height). World height is `In.AnchoredPos.y +
g_Anchor.y`; use the UNperturbed `In.WorldNormal` for slope. Sample band textures with the
same world-tiled `tiledUV` and the SAME explicit `SampleGrad` (ddx/ddy already computed) —
implicit LOD is known-broken at this uv magnitude (see PS header comments). This HLSL
build cannot resolve `mix(float3, float3, float)` — write blends as `c + (t - c) * w`.
Thresholds as `#ifndef`-guarded `#define`s so they can be tuned without a repak. Note the
PS lives in the repo at c-game/data/pak_1/materials and must be repacked into
build/c-game/data/pak_1.pak via scripts/build.sh. No new comments in the code (AGENTS.md).
If the main-menu frame has no terrain in view, pick `ENGINE_SCREENSHOT_FRAME` where the
world is visible (the terrain draws in the `terrain` pass — a RenderDoc dump of that pass
is the fallback visual check).

Verification: ./scripts/build.sh && ENGINE_HIDDEN_WINDOW=1 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json TERM=xterm ENGINE_SCREENSHOT=/tmp/splat_bands.jpg ENGINE_SCREENSHOT_FRAME=300 ENGINE_LOG_TIMEOUT=120 ./build/c-game/c-game
