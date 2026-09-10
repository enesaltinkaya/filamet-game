# Plan: terrain receives and casts shadows

## Strategy

The cascaded shadow pass (DiligentFX ShadowMapManager in ShadowDiligent.cpp) today
has exactly one caster: the glTF player (`gltfDiligentShadowDraw`, called once per
cascade from `renderCascadesImpl`). The splat terrain already *receives* PCF
shadows (`splatShadowsOn` → `ShadowMapIndex = 0`, fed from
`shadowDiligentPbrWorldToLightProj/Slice/DepthBias` in SplatTerrainDiligent.cpp),
so the main gap is the caster side. Plan: (1) study the splat frame draw
(chunk culling, per-frame cbuffer, VS) and the glTF caster to mirror its
contract; (2) add a depth-only terrain caster
`splatTerrainShadowDrawDiligent(ctx, cascadeWorldToLightProj, dsv)` with a
depth-only PSO using `shadowDiligentCasterBias` and per-cascade frustum
culling of chunks; (3) wire it into `shadowDiligentRenderCascades` after the
glTF draw; (4) verify the receive path actually works on the terrain in PCF
mode (single cascade anchored to the player — check slice/bias/fade and extend
if the receive quality is wrong for terrain); (5) verify with screenshots and
the `ENGINE_SHADOW_READBACK=frameN` one-shot depth readback (logs per-cascade
geometry bbox / "NO geometry depth"), tuning bias for acne/peter-panning.

Approach notes: no comments in code; reuse the existing splat VS/vertex
buffers — only a new depth-only PSO (no PS or a trivial discard PS, matching
the glTF caster style); cull chunks against the cascade box the same way
world draw culls against the camera; guard the call so the shadow pass is
unaffected when the splat pass is not loaded (ENGINE_SPLAT_TERRAIN=0).

Verification: scripts/build.sh && ENGINE_HIDDEN_WINDOW=1 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json ENGINE_AUTOTEST=enter ENGINE_SCREENSHOT=/tmp/terrain_shadow.jpg build/c-game/c-game
