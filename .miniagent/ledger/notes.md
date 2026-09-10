# notes

## Invariants (all rounds)

1. **The raw cascade `WorldToLightProjSpace` must be transposed exactly once before landing in the splat caster cbuffer** — the splat runtime-HLSL family is row-vector math (same convention as `splatFrameTranspose`, SplatTerrainDiligent.cpp:865-876). Uploading untransposed is the #1 trap. Confirmed by construction: the splat world *receiver* PS consumes the same transposed matrix + anchored pos (`mul(float4(AnchoredPos,1), sWorldToLightProj)`, splat_terrain_ps.hlsl:416), so caster/receiver match; a mismatch would manifest as a displaced/mirrored shadow, not an error.
2. `g_Anchor` = this frame's `diligentWorldAnchor()` read fresh at draw time as f32, never cached (caching = the `poseRebuild()` detachment failure). Same fresh f32 anchor reused for the cull.
3. **No rasterizer depth bias on the caster PSO** (PBR caster draws bias-free; the receiver subtracts its own `fFixedDepthBias` → `sSlice.y`). A caster bias would double-peter-pan. Caster cbuffer is `cLightViewProj` + `g_Anchor` only; PSO verified bias-free in source (SplatTerrainDiligent.cpp:1671-1674) and via renderdoc.
4. **Cull = 8-corner anchor-space light-box test** (NOT the `FrustumCull.h` 6-plane helper): transform the chunk AABB's 8 corners with the RAW untransposed W2L in column-vector math (the effective transform of the transposed cbuffer matrix), anchor-subtracted with the same fresh f32 anchor, slab-test transformed min/max against the cascade NDC box. Margin = 2 cascade pixels in NDC (4.0/cascadeSize). `ENGINE_SHADOW_CASTER_NOCULL` (any value) bypasses culling and reproduces the unculled run exactly.
5. NDC box is x∈[-1,1], y∈[-1,1], z∈[MinZ,1] — confirmed from `ShadowMapManager::DistributeCascades` (CascadeProj maps f3MinXYZ→(-1,-1,MinZ), f3MaxXYZ→(1,1,1)); MinZ comes from `GetDeviceInfo().NDC.MinZ` (0 on this Vulkan device, not the GL -1). The box z range is queried, not assumed.
6. **Gate**: shared predicate `splatTerrainShadowDrawsDiligent()` = `ENGINE_SPLAT_TERRAIN` not "0" && `splatTerrainDiligent()` loaded && `taaColorRTV()` non-null && `splatShadowsOn()` (PCF mode only — the splat receiver only works in mode 1). Caster draws iff the world splat pass draws. Init independent of `splatPassInit` (no TAA-chain dependency); resources released via `splatShadowCasterRelease` inside `splatPassRelease` (so `splatTerrainDestroyDiligent` covers it).
7. **0-RT `D32_FLOAT` PSO was ACCEPTED by the device** (no dummy-RT fallback triggered; the fallback exists and recreates a cascade-sized RT if the tier/atlas changes).

## Known gotchas (keep)

- Combined screenshot+readback runs need `ENGINE_SCREENSHOT_FRAME=300` — `ENGINE_SCREENSHOT` defaults to frame 100 and QUITS the engine before a frame-300 readback fires.
- Screenshots are non-deterministic run-to-run even for identical code (TAA temporal noise; ~0.001% of px >16 diff between two identical runs; 0.009% is the noise baseline, a real change shows ~33%).
- The headless autotest camera is static within a run but **not bit-deterministic across runs** (one run read cascade1 min 0.385 / 11/16 instead of 0.012008 / 13/16). Compare readbacks within a run, not across runs.
- `run.sh` does NOT set `ENGINE_AUTOTEST` despite docs/renderdoc-capture.md saying it does — export `ENGINE_AUTOTEST=enter` explicitly.
- **RenderDoc replay gotcha: `SaveTexture` on ANY D32 texture in this build returns a meaningless uniform fill** (atlas → uniform 1.0; world depth → uniform 0.0/1.0) even though color passes replay correctly. Headless D32 content is untrustworthy — use qrenderdoc (GUI) or the engine's own `ENGINE_SHADOW_DUMP` / `ENGINE_SHADOW_READBACK`.
- Clean up old /tmp/RenderDoc captures (~400 MB each) after sweeps.

## Decisions

- Approach A implemented (dedicated depth-only pass `splatTerrainShadowDrawDiligent`, not reusing the world splat PSO — that path *samples* `g_ShadowMap`, a read/write hazard on the texture being written — and not routing through the PBR scene renderer). No-cull de-risking folded in as the `ENGINE_SHADOW_CASTER_NOCULL` bypass.
- Caster is wired in `ShadowDiligent.cpp` `renderCascadesImpl` after `gltfDiligentShadowDraw`, per-cascade (4th param `int cascadeIndex`), under `Diligent::ScopedDebugGroup(ctx, "terrain")`.

## Open question

- Visual self-shadowing still not confirmable from the static headless camera (180,683.74,180): the visible terrain faces are the LIT side (sun nearly behind camera). No flat-ground self-shadowing or peter-panning is visible, and depth dumps show no mirrored/detached shapes, but a shadowed face in view is needed for visual confirmation. Not an artifact.

## Final verification evidence (2026-09-10, full ledger command + ENGINE_SCREENSHOT_FRAME=300, exit 0, no VUIDs)

- Log: `splatTerrain: shadow caster pass ready` (0-RT D32 PSO, no fallback); culled `12/16` cascade 0 + `13/16` cascade 1 — stable across rounds 1-4 at the usual camera.
- Readback (frame 300): cascade0 all 1.0 (geometrically correct — the headless camera floats above the nearby terrain tops and cascade0 zEnd is only ~4.69 m); cascade1 min 0.012008, max 1.0, ~99.8% ones, geometry bbox uv (0.000,0.381)..(0.692,0.611) — terrain-scale coverage, far cascade non-empty. The 0.012 < the in-box range [0.39,0.53] is expected and cannot be removed by chunk-level culling: the cascade NDC clip starts 0.3895 (bias) below the lightbox near plane, so in-chunk geometry behind the near plane (up to ~205 m) survives the z clip. Fixing it would need per-triangle clipping — accepted limitation.
- A/B `ENGINE_SPLAT_TERRAIN=0`: no caster log lines, both cascades all 1.0, player shadow unchanged (the player sits ~1.1-1.3 km from the static headless camera, outside the 80 m shadow distance, so the player caster writes nothing in these runs — pre-existing, not a regression).
- Renderdoc (capture /tmp/RenderDoc/c-game_frame300.rdc): `shadow` group holds two nested `terrain` groups — 12 draws (cascade 0) + 13 draws (cascade 1) — matching the culled log exactly; each cascade also has one player gltf cast draw outside the terrain groups. Casts write the D32 2048² atlas (arr=2, one slice per cascade).
- Live depth ground truth (engine, not replay): `ENGINE_SHADOW_DUMP` PGMs → /tmp/rdc-dump/atlas_pg{0,1}_small.png — cascade0 one small spot, cascade1 curved terrain bands matching the readback bbox; no mirrored/detached shapes.
- Screenshots: /tmp/splat_caster.jpg (frame 300), /tmp/splat_ab0.jpg (A/B).

## Receiver self-shadowing confirmed in the receiver pass (task 4)

- splat_terrain_ps.hlsl declares `Texture2DArray g_ShadowMap` + comparison sampler, and when `ShadowMapIndex >= 0` (set from `splatShadowsOn()`, SplatTerrainDiligent.cpp:939) the PS transforms `AnchoredPos` by the transposed `sWorldToLightProj` and runs `filterShadowPCF3` (3x3 Witness PCF, `SampleCmpLevelZero`), subtracting the receiver-side NDC bias `sSlice.y` (splat_terrain_ps.hlsl:411-420).
- `splatBindDynamicResources` binds `shadowDiligentShadowSRV()` — the same atlas the caster writes — when shadows are on (SplatTerrainDiligent.cpp:1467), else a dummy. So terrain self-shadows via its own caster's atlas sampling, and the caster/receiver convention match by construction (same transposed W2L, same fresh f32 anchor).
- "terrain in the CSM shadow pass (casters), self-shadowing" marked done in plans/blender-terrain.md (dated 2026-09-10).

## Rounds (summary — details above and in tasks.json)

- Round 1 (task 1): caster pass implemented + wired; 16/16 chunks unculled.
- Round 2 (task 2): 8-corner light-box cull; draw counts drop 12/16 + 13/16, readback byte-identical to unculled; NOCULL reproduces task 1 exactly.
- Round 3 (task 3): renderdoc sweep, live depth dumps, A/B, cleanup, full ledger verification — no code changes needed.
- Round 4 (task 4): ledger + plans/blender-terrain.md updated; receiver self-shadowing confirmed (above).

## round 5 — Open question CLOSED: PCF self-shadowing confirmed visually via A/B (no renderdoc needed)

The open-question premise ("static camera sees only the lit side, no shadow visible") was wrong: the lit faces in the default view DO receive a cast PCF shadow from terrain on the sun side of the camera. No camera/sun knob was needed — the existing `ENGINE_PBR_NO_RECEIVE` hook (SplatTerrainDiligent.cpp:845 → `splatShadowsOn()` false → `ShadowMapIndex = -1`, PS skips `filterShadowPCF3` entirely and binds the dummy SRV; caster gate drops too) is the receiver-off knob. When the splat pass draws, the PBR scene terrain draw is skipped (GltfDiligent.cpp:1426), so all visible terrain pixels are splat-pass output — the A/B attributes cleanly to the splat receiver PCF path.

- Runs (frame 300, same static camera, exit 0, no VUIDs): /tmp/t5_shadow_on_final.jpg (default) vs /tmp/t5_shadow_off_final.jpg (ENGINE_PBR_NO_RECEIVE=1, no caster log lines). Logs: /tmp/t5_on.log, /tmp/t5_off.log; crops /tmp/t5_pair_big.jpg, darkening heatmap /tmp/t5_diffheat.jpg.
- Diff: 29.1% of terrain pixels >8 levels DARKER in the ON run, only 0.01% brighter (TAA noise is symmetric ~0.009%; one-sided = shadow factor, not noise). 0.5% of pixels >24 levels. The darkening heatmap traces the terrain silhouette with soft PCF edges along ridgelines — a cast shadow band across the visible slopes.
- Caster logs this run: 12/16 (cascade 0) + 13/16 (cascade 1), same as rounds 1-4. Readback: cascade0 empty (all 1.0), cascade1 min 0.012008, bbox uv (0.000,0.381)..(0.692,0.611) — byte-identical to prior rounds.
- Cascade chain: `shadowDiligentPbrSlice()` CPU-picks the cascade covering the player feet' cam-space z (~250 m > cascade1 zEnd 80 m → clamped to cascade 1); the splat receiver samples that one matrix/slice (`sWorldToLightProj` + `sSlice`, splat_terrain_ps.hlsl:411-420). So the darkened pixels read exactly the cascade-1 region the readback bbox covers — caster → atlas → PCF receiver, end-to-end.
- Note: ENGINE_PBR_NO_RECEIVE also disables the caster (shared gate), so the OFF run's atlas is empty; attribution still holds because that env var touches nothing else (only the two shadowOn conditions — GltfDiligent.cpp:75, SplatTerrainDiligent.cpp:845).
- Camera/sun knobs exist (ENGINE_TELEPORT moves spawn+framing, ENGINE_CAMERA=topdown|close|character, ENGINE_CAMERA_DOLLY pans, ENGINE_FAKE_DRAG orbits) but none were needed; no sun-direction knob exists (hardcoded (-0.6,-1.0,-0.5), Game.cpp:226).
- No new RenderDoc captures created; the stale round-3 capture /tmp/RenderDoc/c-game_frame300.rdc (446 MB) was deleted per the standing cleanup rule.
- No code changes this round.

## final (compaction)

notes.md compacted in place (220 → 51 lines before this note): merged the
triply-repeated verification evidence of rounds 1/3/4 into one final block;
dropped dead ends already reflected in tasks.json (rejected approaches B/C,
the 6-plane FrustumCull far-plane concern, the wrong prediction that culling
would pull cascade1 min into [0.39,0.53] — kept only as the accepted
in-chunk near-plane limitation); kept invariants, decisions, gotchas, and the
open question verbatim.
