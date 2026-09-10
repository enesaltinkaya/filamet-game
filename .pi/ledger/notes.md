# notes

Context: this ledger was re-created for a new task. The working tree is dirty from a prior "fix terrain shadows" run (ShadowDiligent, DiligentRenderer, SplatTerrainDiligent, Player.cpp modified; prior-run notes are not preserved here).

## brainstorm

## Core difficulty

The symptom "player shadow barely visible" is one observation produced by four independent subsystems sharing one depth atlas (cascade coverage, caster draw, atlas sampling, receiver fade/bias), and the existing readback hook cannot tell the player's depth apart from the terrain's in the same cascade — so the hard part is cheaply splitting the fault between caster-side (depth never lands in the atlas) and receiver-side (depth lands but is sampled away).

## Reductions / key lemmas

- The camera view is rotation-only and anchored at the player, so the player sits at the near end of cascade 0 at every quality tier; "wrong cascade selected" is structurally impossible for the player caster/receiver. The fault must be in caster coverage, caster draw, or sampling.
- Caster and receiver agree on the matrix by construction: the PBR caster draws with `GetCascadeTransform(i).WorldToLightProjSpace` (untransposed, row-major packed) and the PBR receiver gets the same value from `shadowDiligentPbrWorldToLightProj`; the terrain caster/receiver both use the transposed `LightAttribs` copy. A matrix-convention bug therefore cancels between caster and receiver — a mis-placed shadow would indicate it, a *missing/faint* one does not.
- Quality tiers (1024/2casc/60m, 2048/2casc/80m, 2048/3casc/120m, PCF 3/3/5) share the same cascade-0 geometry for the player. If the shadow is equally faint at q0/q1/q2 the fault is structural (caster/bias/winding, sun elevation), not resolution- or PCF-related; if it improves with quality, the fault is coverage or fade (tier distance 60/80/120 m + `f4ShadowFade` clamp).
- A/B isolation exists in-repo: `ENGINE_SHADOW_NO_PLAYER` skips only the player caster draw; `ENGINE_SHADOW_READBACK=frame` gives per-cascade min/max/zero% and a geometry bbox in uv; `ENGINE_SHADOW_DUMP=path` dumps cascade PGMs. Diffing the cascade-0 PGM with/without the player isolates the player's depth footprint deterministically on CPU, without RenderDoc.
- Known suspect (from the prior terrain run): the terrain caster has an `ENGINE_SHADOW_CASTER_NOCULL` env because light-space winding flips culling; the player caster (`gltfDiligentShadowDraw`) reuses the world PBR PSOs with default flags — if those PSOs cull front faces and the light view (right-handed, Z-flipped) inverts winding, the player draws back faces and casts a faint/empty shadow. This is the single most likely cause and is checkable statically.
- Secondary suspects: `sa.fFixedDepthBias` is scaled by `Cascades[0].f4LightSpaceScale.z` but applied verbatim (NDC) by receivers of *all* cascades; `casterPad` defaults to 1.0 (cascade frustum exactly equals camera frustum — a player shadow cast at a shallow sun angle exits the light frustum quickly); and a low sun elevation alone makes any shadow long, thin, and clipped by the tier distance, which "barely visible" also matches.

## Candidate approaches

1. **Quality sweep + existing readback (visual triage first).** Run the plan's q0/q1/q2 sweep with screenshots plus `ENGINE_SHADOW_READBACK=100` in the same runs; record per-cascade stats and the sun direction from the log. Risk: confirms the symptom but only weakly localizes it — the readback bbox is terrain∪player, and a faint shadow still passes "geometry present". Effort: low (~4 runs).
2. **CPU depth A/B isolation (recommended first diagnostic).** At one fixed frame, run with and without `ENGINE_SHADOW_NO_PLAYER` under `ENGINE_SHADOW_DUMP=/tmp/pb`, diff the cascade-0 PGMs (plus readback bbox). If the player footprint appears/disappears → caster-side (proceed to 3); if the player depth is there but the shadow is still faint → receiver-side (bias subtraction, PCF, fade, slice, or sun angle). Risk: the two runs must hit an identical frame and player pose (use the pinned-player trick from the plan's notes); terrain motion between runs is a non-issue since the anchor is the player. Effort: low-medium (2 runs + a small PGM diff).
3. **Static audit of the PBR caster vs. the light view.** Read the PBR PSO cull mode used in `gltfDiligentShadowDraw` and the winding convention after `UseRightHandedLightViewTransform`; compare against the terrain caster's dedicated depth PSO (cull disabled) and the `ENGINE_SHADOW_CASTER_NOCULL` precedent. If front faces are culled in light space, the fix is a cull-disabled/depth-only PSO for the PBR caster. Risk: the PBR renderer may not expose per-PSO cull override (it is a shared FX renderer) — a fix might need a small renderer extension. Effort: low to read, low-medium to fix.
4. **RenderDoc cascade inspection.** Capture with `scripts/run.sh renderdoc`, dump the `shadow` pass depth outputs (qrenderdoc or replay API), sample the atlas at a ground pixel under the player and compare against the receiver's expected depth + bias. Most direct receiver-side evidence, but the shadow pass has no color outputs and this workflow is the most expensive of the four. Risk: time; only worth it if 2/3 point at the receiver and the CPU evidence is inconclusive. Effort: high.

## Recommended approach

Approach 2 (depth A/B isolation) first, with approach 3's static cull/winding check done in the same round since it costs almost nothing; approach 1's sweep brackets everything (it doubles as the before/after verification), approach 4 only as a contingency. This order is correct because the caster-vs-receiver split is the only fork that determines the fix location, and it is answerable on CPU from depth dumps; the cull/winding audit is the prior most-likely cause and is a file read. For this to work: the two A/B runs must capture the same frame with an identical player pose (pin the player in `build/c-game/data/db/db.db` as the prior run did, or rely on `ENGINE_AUTOTEST=enter` reproducibility), and `ENGINE_SHADOW_DUMP`/`READBACK` must fire at the screenshot frame (~100).

## Proposed tasks

1. **Sweep + baseline stats.** Run the plan's q0/q1/q2 screenshot sweep (mode=1) adding `ENGINE_SHADOW_READBACK=100` and the one-shot `shadow dbg` cascade dump; capture the sun direction/elevation from the log; report whether the faint-shadow symptom persists across all three qualities and whether cascade-0 geometry bbox differs by quality.
2. **Caster A/B depth isolation.** With the player pinned (or autotest-identical pose), run once normally and once with `ENGINE_SHADOW_NO_PLAYER` at the same frame, `ENGINE_SHADOW_DUMP=/tmp/pb` both runs; diff the cascade-0 PGMs and compare readback bboxes; report whether the player writes depth into cascade 0 and where it lands relative to the player's ground position.
3. **Static cull/winding/bias audit.** Verify the PBR caster PSO cull mode against the right-handed light-view winding (vs. the terrain caster's cull-disabled depth PSO), and audit the receiver path: `fFixedDepthBias` cascade-0 scaling applied to all cascades, PCF filter, `f4ShadowFade` cutoff, and the `ShadowMapSlice`/UVScale receiver wiring in the PBR shadow code.
4. **Fix + verify (contingent on 1-3).** Apply the minimal fix the diagnosis identifies (expected: light-space culling on the PBR caster, or a bias/coverage correction), rebuild via `scripts/build.sh`, re-run the q0/q1/q2 sweep and the A/B diff; acceptance is a clearly visible player shadow at all three quality levels with no self-shadow acne on the player.

## round 1 (task 4 — faint player shadow diagnosis)

**Verdict: receiver-side fault (cascade-coverage mismatch). Caster is NOT at fault for missing depth.**

### Evidence

1. **Sweep (q0/q1/q2, mode=1, ENGINE_SHADOW_READBACK=100).** Cascade zEnds per tier: q0 c0=3.83m/c1=60m; q1 c0=4.69m/c1=80m; q2 c0=3.01m/c1=14.73m/c2=120m. NOTE: the first sweep used the pinned db camera (2 km away, 169 m above) which does NOT frame the player — treat its per-cascade readback as invalid for player framing. Valid player-in-frame runs used ENGINE_AUTO_RUN=1 (orbits the player; camera ~11.24 m from it, pitch ~15-35 deg down). Sun elevation ~52 deg (W2L light-Z Y-comp 0.788) -> ~1.4 m shadow for a 1.8 m char; not a low-sun issue.

2. **A/B depth isolation (ENGINE_SHADOW_NO_PLAYER vs normal, ENGINE_SHADOW_DUMP, frame 100, q1, player in frame).**
   - cascade-0 diff = 6907 texels: a FULL, clean running-pose silhouette (head/torso/both arms/both legs). The caster writes complete, correct depth into cascade 0.
   - cascade-1 diff = 346 texels, almost entirely a 1-texel diagonal line sitting on the cascade-frustum boundary (depth~0 in A, empty in B). The player's actual depth footprint in cascade 1 is a sliver — it is clipped out of cascade 1.

3. **Per-pixel cascade debug (ENGINE_SPLAT_SHADOW_DEBUG=1: red=c0, green=c1; mode 2 = attenuation mask).** The ground band at the very bottom of frame (nearest, below the camera) is cascade 0; EVERYTHING else — including the player and the ground around/under them — is cascade 1. The shadow shows as a thin dark strip in the attenuation mask (mode 2): present but narrow.

### Root cause

The player sits ~11.2 m from the orbit camera, outside cascade 0's ~4 m range, so both the player and its shadow-receiving ground are assigned to cascade 1 at every quality tier (11 m is in c1 for q0/q1/q2 — hence the faintness is quality-independent). The shadow-receiving ground correctly samples cascade 1, but the player's caster depth is concentrated in cascade 0 and is clipped to a sliver in cascade 1 — so the receiver finds little/no caster depth where the player should occlude. Net: thin/faint shadow. The full clean cascade-0 silhouette proves caster shading/winding is correct.

### Suspect audit (all ruled out as primary)

- **Cull/winding (PBR caster reuses world PSOs, CULL_MODE_BACK for non-double-sided):** disproven — cascade-0 A/B diff shows the complete body, so front faces are not being culled in the right-handed light view.
- **fFixedDepthBias (scaled by Cascades[0].f4LightSpaceScale.z, applied verbatim in NDC to all cascades):** the shadow IS present (mode 2 shows real attenuation), so the bias is not zeroing it; at most a minor per-cascade relative-bias contribution in the coarser c1.
- **PCF (3x3 q0/q1, 5x5 q2):** secondary — blurs the already few-texel cascade-1 footprint; not the primary cause.
- **f4ShadowFade (tier 60/80/120 m):** player at 11 m -> fade=1, not the cause.
- **casterPad (1.0, no pad):** sun is high (52 deg), shadow short (1.4 m), so pad does not clip it out of the light frustum; not the primary cause.

### Fix direction for task 5

Make the character's caster and its shadow-receiving ground agree on a high-resolution cascade. The player is in cascade 1 (~11 m) while its caster depth lands in cascade 0 (and is clipped in c1). Options: (a) widen/shift cascade 0 so it covers the character's band, (b) anchor/distribute the cascades around the player (the third-person focus) rather than pure camera distance, or (c) ensure the caster fully renders into the cascade the character actually occupies so the c1 receiver finds the full silhouette. Minimal scoped change in the shadow distribution/caster path (ShadowDiligent.cpp distribute + gltfDiligentShadowDraw), not in PCF/bias/fade.

### Artifacts

/tmp/shadow-q{0,1,2}.jpg+log (sweep, far pinned camera — framing invalid, readback valid); /tmp/pb-a.jpg /tmp/pb-b.jpg (+ .0/.1 PGM dumps, +log) A/B at q1 player-in-frame; /tmp/sdbg-{1,2}.jpg (+log) cascade/attenuation debug; /tmp/cdiff-{0,1}.png, /tmp/c0-zoom.png, /tmp/c1-zoom.png (A/B diffs highlighted red).

## summary (partial-work state after last round)

Last round was still diagnostic, not the task-5 fix. State on disk:

- **Task 5 fix: not applied** (tasks.json task 5 = pending, no round-5 ledger entry). No cascade-distribution change (still pure camera-distance distribution, fPartitioningFactor 0.95), no caster forcing/scissor, no bias/PCF change.
- **What exists now** (source + binary, last build 22:49:32 is current — binary mtime is after the last source edit):
  - `ShadowDiligent.cpp`: PBR player receiver uses a per-frame CPU cascade pick (feet cam-space z vs `fCascadeCamSpaceZEnd`, +1 m torso) so the player body itself samples the right cascade (`pbrW2L/pbrSlice/pbrBias` -> GltfDiligent.cpp ~1327). Caster still draws the player into ALL cascades (loop in `renderCascadesImpl`). Readback hooks now log per-cascade player feet/head NDC + inside flags.
  - `Player.cpp` / `FlyingCamera.cpp`: automated-run gating (screenshot/dolly/no-player keep the player parked, scripted camera skips DB restore).
  - `GltfDiligent.cpp` gltfDiligentShadowDraw: poseRebuild before the caster draw (shadow stays attached while the orbit camera moves).
- **Evidence, current binary** (RenderDoc run 22:49:40, same spawn/pose as earlier A/B): `/tmp/rdc-c1-depth.png` shows the FULL player silhouette inside cascade-1 depth (small: ~10 px figure at 2048, c1 xy scale 0.0106 NDC/m). Readback NDC: c1 feet (0.283,-0.312,0.189) head (0.277,-0.302,0.181) — both inside.
- **Contradiction to resolve**: the r5 A/B diff (`/tmp/r5-cdiff-1.png`, older binary, 22:45) showed the player's c1 footprint as a 1-px sliver; the newer rdc dump shows the full silhouette at the same pose. So either the caster's c1 write changed between binaries (the 22:42:52 ShadowDiligent edit is the only relevant source change, but its exact content can't be identified without git) or the runs differ. Re-run `ENGINE_SHADOW_DUMP` A/B on the CURRENT binary before choosing the fix.
- **Still structurally true**: even with a complete c1 silhouette, the player occupies a tiny atlas area (c1 spans 4.69→80 m in 2048²), so the ground shadow is only a few texels wide; fFixedDepthBias (0.00022 NDC) is negligible at c1's 0.0052 NDC/m scale. A visible-shadow fix still needs more atlas density on the player band (widen/shift c0 or re-anchor cascades toward the player).
- Artifacts: `/tmp/rdc-c1-depth.png` (c1 depth, current binary), `/tmp/r5-*.png` (A/B, old binary), `/tmp/RenderDoc/c-game_frame300.rdc` (707 MB — delete after use), `/tmp/rdc-run4.log` (run log incl. cascade scale/bias dump).
