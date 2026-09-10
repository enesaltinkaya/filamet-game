# Plan — splat terrain CSM shadow caster

The ledger task is "continue" with no context.md; the prior state is on disk: the
splat terrain pass (phase 2 of plans/blender-terrain.md) is built and wired —
`splatTerrainLoadDiligent` runs in Game.cpp on ENTER WORLD, `splatTerrainDrawDiligent`
draws the culled chunks under the `terrain` debug group in GltfDiligent.cpp's
world pass, and it already receives CSM shadows (`splatShadowsOn()`). The one
pending item named in the plan ("terrain in the CSM shadow pass (casters),
self-shadowing") is the in-flight work: `splat_terrain_shadow_{vs,ps}.hlsl` exist in
pak_1 (edited 15:57 today) but nothing compiles or draws them — the shadow pass'
per-cascade loop still casts only the character via `gltfDiligentShadowDraw`.

Approach:
1. Add `splatTerrainShadowDrawDiligent(ctx, lightViewProjRowMajor, cascadeDSV)` to
   SplatTerrainDiligent.{h,cpp} (signature mirroring `gltfDiligentShadowDraw`):
   compile the two caster shaders via the existing `createSplatHlsl`, build a
   depth-write + depth-test PSO (0-RT D32, dummy-RT fallback), upload the per-cascade `cbSplatShadowCaster`
   (`cLightViewProj` = the raw cascade `WorldToLightProjSpace` **transposed once** — the splat runtime-HLSL
   family is row-vector math per `splatFrameTranspose`; see notes.md lemma 1 — `g_Anchor` = this frame's
   `diligentWorldAnchor()` as fresh f32), and draw every chunk that survives cascade culling. No rasterizer
   depth bias (PBR caster draws bias-free; receiver subtracts its own). No culling in the first task; the
   8-corner anchor-space light-box test (not the FrustumCull helper — aggressive far plane if z is remapped)
   with an `ENGINE_SHADOW_CASTER_NOCULL` bypass lands in the next task.
2. Wire it into `renderCascadesImpl` next to the `gltfDiligentShadowDraw` call
   (no-op when the splat terrain is not loaded or the caster pass failed to init);
   release the caster PSO/pipeline resources in `splatPassRelease` /
   `splatTerrainDestroyDiligent`.
3. Verify visually: headless run to ENTER WORLD, screenshot shows the terrain
   casting the CSM shadow onto itself/props; cross-check the atlas with
   `ENGINE_SHADOW_READBACK` and A/B against `ENGINE_SPLAT_TERRAIN=0` (PBR fallback
   caster) to confirm the silhouette is not displaced by the anchor/precision
   convention (a displaced caster would leave the ground shadow detached — the
   failure mode gltfDiligentShadowDraw's comments describe).

Verification (task 1): ninja -C build && TERM=xterm-256color timeout 60 env ENGINE_HIDDEN_WINDOW=1 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json ENGINE_AUTOTEST=enter ENGINE_SHADOW_READBACK=300 ENGINE_SCREENSHOT=/tmp/splat_caster.jpg ./build/c-game/c-game (plus A/B with ENGINE_SPLAT_TERRAIN=0)
