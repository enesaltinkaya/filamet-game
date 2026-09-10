# Plan: fix stretched cliff texture on steep slopes

**Root cause (verified in code):** in `c-game/data/pak_1/materials/splat_terrain_ps.hlsl` the cliff
band is blended via `wCliff = smoothstep(SPLAT_CLIFF_LO, SPLAT_CLIFF_HI, slope)` (line ~343) but
`g_CliffAlbedo`/`g_CliffNormal` are sampled at the same world-XZ `tiledUV` (lines 348, 355) that the
flat splat chain uses — a pure XZ projection. On steep slopes that projection stretches/compresses,
hence the ugly stretched look the user sees on the hill.

**Reference:** the old engine's `heightmap_terrain.frag` (game-001-cpp) solved exactly this with
"slope-based triplanar cliff": `triplanarWeights(worldNormal, sharpness=4)` and per-face 2D
projections at `CLIFF_TRIPLANAR_SCALE = AZGAAR_CLIFF_DETAIL_TILE(32) / 4096`, blended into base
color/normal with the same slope smoothstep. That is the behavior to port.

**Approach:**
1. In `splat_terrain_ps.hlsl`, replace the cliff `SampleGrad(g_DetailSampler, tiledUV, du, dv)`
   (albedo + normal) with a triplanar sample of `g_CliffAlbedo`/`g_CliffNormal`: three world-axis
   2D projections of the world position (`In.AnchoredPos + g_Anchor`), weighted by the squared
   absolute world normal components with a sharpness exponent (~4, as in the old shader), using the
   old engine's cliff scale so pattern density matches the reference. Keep the existing `wCliff`
   smoothstep blend into albedo and nT; leave the sand/snow and splat chain untouched. Use explicit
   sampler-grad sampling where the sampler state requires it (g_DetailSampler is REPEAT/aniso).
2. No C++/resource changes expected — textures and sampler bindings already exist
   (`g_CliffAlbedo`/`g_CliffNormal` declared and bound in SplatTerrainDiligent.cpp ~1309).
3. No comments in the code (AGENTS.md). The .hlsl lives in c-game/data/pak_1 — `scripts/build.sh`
   repacks changed paks via its data.sh step, so one build run picks up the shader edit.
4. Visual A/B via `ENGINE_SCREENSHOT` headless runs; check the cliff texture is unstretched and no
   other terrain bands regressed.

Verification: export ENGINE_HIDDEN_WINDOW=1 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json && ./scripts/build.sh && timeout 90 env ENGINE_SCREENSHOT=/tmp/cliff_fix.jpg ./build/c-game/c-game; test -s /tmp/cliff_fix.jpg
