# Plan

Shadows regressed when the terrain rendering was switched away from azgaar. Strategy: first locate the regression surface — compare the current terrain code against the git history for the azgaar version (what shadow-related inputs/outputs the old terrain provided: shadow sampling in its shader, depth write in the shadow pass, normal/depth values). Then audit the two halves of the shadow pipeline against known-good references: shadow map generation (light-space matrix, depth bias/slope scale, depth format) and shadow sampling in the world/PBR pass (matrix, UV remap, PCF, compare function), using Diligent's samples (PBR / ShadowMap samples), its tutorials (SHADOW_MAP tutorial), and Radient's shadow implementation as ground truth. Check the "all types" angle: directional and any point/spot light shadows, plus terrain self-shadowing. Reproduce with ENGINE_SCREENSHOT and/or RenderDoc capture (the `shadow` pass group exists) before and after fixes. Keep changes minimal and in c-engine/renderer; no code comments.

Verification: cmake --build build -j8
Baseline commit: fef1a7889af8d720caf1d49900750dc67bdb72e2 (dirty: only .miniagent/ledger files modified, unrelated)
