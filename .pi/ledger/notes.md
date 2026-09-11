# notes

## brainstorm

## Core difficulty

The "cut-out" on the long cliff is a hard edge, not blur — it points at geometry being *excluded* from a cascade's light-space frustum (or culled as a caster chunk), not at PCF/bias coarseness. The cliff top/face either sits outside the cascade bounding box that `DistributeCascades` builds (StabilizeExtents sphere padding), pokes in front of the shadow camera's light-space near plane, or extends past the far plane — so the cliff stops being a depth caster and the shadow on it/behind it disappears in a straight band.

## Reductions / key lemmas

1. The caster and receiver use the *same* cascade transform (`mgr.GetCascadeTransform(c)`), so the shadow is only as good as the cascade's light-space box: anything outside the box never enters the depth atlas. There is no bias value that can fix a missing caster — the fix must be in cascade extents/splits or caster culling.
2. Caster culling is per-cascade AABB-vs-NDC (`splatShadowCasterChunkVisible`, `SplatTerrainDiligent.cpp`): a chunk is dropped if its NDC box doesn't overlap `[ndcMinZ−4/size, 1+4/size]` on x/y/z. A long cliff chunk can legitimately lie outside one cascade's box but inside another's — that is fine *only if* the correct cascade also receives it. The bug shows up as: cliff in cascade k's box on the *receiver* side, but its chunks culled or clipped on the *caster* side (near/far plane carve).
3. The depth atlas is `TEX_FORMAT_D32_FLOAT`, so extending a far plane costs almost no precision — far-plane extension for the caster is a cheap, targeted lever.
4. Cascade 0 is widened to a "focus band" (player cam-Z + margin, clamped to 0.8·distanceM) and the rest re-split log-uniformly. A long cliff whose receiving face falls just past `distanceM` (60–120 m by tier) simply has no shadow at all — indistinguishable from clipping in a screenshot, so coverage vs. clipping must be disambiguated first.
5. The pass is already instrumented: `splatTerrain: shadow caster frame N cascade d — x/y chunks drawn (culled)` logs per cascade, `ENGINE_SHADOW_CASTER_PROBE` dumps NDC of nearest chunk + player, and the `shadow dbg` block prints per-cascade scale/bias/zEnd. The `shadow` pass is RenderDoc-labeled, so `rdc.py dump shadow` gives per-cascade depth atlases.

## Candidate approaches

1. **Diagnose, then widen caster light-space extents** (far/near padding or a caster-only extended far plane, keeping receiver matrix unchanged). Sketch: add a pad factor on the light view's near/far (or `StabilizeExtents` margin) used only for the caster draw + cull matrix. Risk: naive far-plane extension shrinks the DSV precision for everything in the cascade — mitigated by 32-bit depth and by extending only where the cliff overflows. Effort: S, once the failing plane is identified.
2. **Re-tune cascade splits / shadow distance** (raise `tier.distanceM`, adjust `focusBand`/`fPartitioningFactor`, or add a cliff-band like the existing focus band). Sketch: move the cliff into a finer cascade or in-range. Risk: if the failure is a light-space *near* clip on the cliff, split tuning does nothing — and it makes all other shadows coarser. Effort: S (params live in one place, `kQualityTiers` + `AdjustCascadeRange`), but may be the wrong lever.
3. **Fix caster chunk culling** (relax `splatShadowCasterChunkVisible` margin / draw a chunk into every cascade whose box it intersects, not just the picked one). Sketch: the cliff's chunk AABB may straddle a cascade boundary and be dropped from the cascade where its shadow is actually received. Risk: likely a red herring if the chunk count log shows the cliff's chunk *is* drawn in the failing cascade; over-drawing costs a little depth-pass time. Effort: S.
4. **Coverage extension beyond distanceM** (tier distance bump + `fReceiverFadeScale` adjustment if the cliff is simply past the shadow distance). Sketch: the cliff face at 150+ m just has no cascades left. Risk: 2048×3 cascades at 200 m is coarse; also a global cost. Effort: XS.

## Recommended approach

Diagnose first, then approach 1 (caster light-space extent fix) with approach 2/4 as fallbacks. The hard-band symptom plus shared caster/receiver matrix means the cliff is being clipped out of the depth atlas; a 2-minute RenderDoc dump of the `shadow` pass at the parked vantage will show exactly which cascade is empty where the cliff should be, and the per-cascade caster log will show whether the cliff's chunks are even drawn. For the fix to work by extent-widening, the cliff must be in-cascade on the receiver side (else the answer is distance/split tuning) and the depth budget must absorb the extension (32-bit depth makes this safe).

## Proposed tasks

1. **Baseline evidence**: run a parked headless screenshot + a RenderDoc capture (`ENGINE_RENDERDOC_CAPTURE=1 ... ./scripts/run.sh renderdoc`), then `scripts/rdc.py dump shadow` to get per-cascade depth atlases; grab the `splatTerrain: shadow caster ... (culled)` chunk counts and `shadow dbg` cascade zEnd/scale log lines. Deliverable: which cascade the cliff lives in, where the atlas goes empty, and whether the cliff's chunks are drawn in that cascade.
2. **Clip-plane attribution**: from the capture + `shadow dbg` dump, compute/inspect the failing cascade's light-space box (near, far, x/y extents) against the cliff's AABB (the `caster probe` log gives chunk min/max); state the exact plane (near/far/top) that carves the cliff, or confirm the cliff is simply beyond `distanceM`. Deliverable: one-line root cause (e.g. "cascade 1 light-space far plane at 94 m, cliff face extends to 131 m along the sun direction").
3. **Fix**: implement the identified correction — caster-only far/near extent padding in `ShadowDiligent.cpp` (caster transform padded before `splatTerrainShadowDrawDiligent` + its cull matrix), and/or `kQualityTiers` distance/split change. Rebuild, re-run the parked screenshot, and diff against task 1's baseline (plus a quick `rdc.py dump shadow` on the new capture to confirm the atlas now covers the cliff).
4. **Regression check**: one parked screenshot run at the default quality tier (and the highest tier if cheap) to confirm no new acne/bleeding on the ground band and no perf-visible stutter; report pass/fail.

## round 1

Baseline evidence (task 1). All runs parked (player/camera untouched), frame 300.

### Artifacts

- `/tmp/baseline.jpg` — parked headless screenshot. Cliff rock wall in left foreground;
  its shadow lands on the grass as a dark triangle with a straight hard cut-out edge —
  beyond that line the grass is fully lit (missing shadow band).
- `/tmp/RenderDoc/c-game_frame300.rdc` (931 MB) — capture fired at frame 300.
  Old 1.1 GB `c-game_frame450.rdc` from 07:57 still in /tmp/RenderDoc (candidate for `rdc.py clean`).
- `rdc.py list`: shadow pass = eids 50–175 (15 draws). Per cascade: 1 player draw
  (RT ResourceId::1135) + 2 terrain-chunk draws (RT ::1155) into the cascade DSV slice,
  then ConvertToFilterable: raw splat (RT ::1114) + filterable EVSM atlas write
  (RT ::1108 = R32G32B32A32_FLOAT 2048² array 3, slices 0/1/2 drawn at eids 147/161/175).
  `rdc.py dump shadow` works (layer 0 only — SaveTexture slice mapping is IGNORED for array
  textures in this SWIG build; all 3 dumps were md5-identical). GetBufferData on textures
  returns 0 bytes here.
- Raw per-cascade DSV via built-in debug: `ENGINE_SHADOW_READBACK=300
  ENGINE_SHADOW_DUMP=/tmp/rdc-dump/rawdepth` (no renderdoc needed). PGMs:
  `/tmp/rdc-dump/rawdepth.{0,1,2}` (2048², 0=nearest, 255=clear), annotated
  `atlas_clear_{0,1,2}.png`, oracle log `/tmp/oracle_run.log`, capture run log
  `/tmp/rdc_run.log`. Caster-cull reconstruction script: `/tmp/cull.py`.

### Caster chunk counts (at capture frame, stable since frame 2)

cascade 0: 2/16, cascade 1: 2/16, cascade 2: 2/16 (first frame 3/3/4).
Reconstructed each cascade's light-projection affine from the oracle dump
(ENGINE_SHADOW_ORACLE=300 — 4 points × 3 coords, least-squares, residual ~1e-15) and
re-ran `splatShadowCasterChunkVisible` exactly: the drawn set is chunks **{9, 13} in
ALL three cascades**. Chunk 9 AABB [-1871,79,-12]-[37,609,1906] contains BOTH the
camera (-320,588,344) and the parked player (-1077.78,525.76,1605.42); its top (y 609)
≈ camera height → **the cliff is chunk 9, and it IS drawn in every cascade, including
the one that samples the shadowed grass.** Chunk 13 = behind the player.

### Cascade math (shadow dbg, frame 300)

mode 4 (EVSM 32-bit), res 2048, 3 cascades, tier distanceM = 120 m, focus band INACTIVE.
zEnd (cam-space): 3.01 / 14.73 / 120.00 m. scale (x=y): 0.2814 / 0.0576 / 0.0071.
bias all cascades (0.3730, -0.4307, 0.1998). W2L (rows, rel to world anchor
(-1095.06,521.49,1596.89)): r1 [-0.203,-0.338,0.919] r2 [-0.857,0.514,0] r3
[-0.473,-0.788,-0.394], no translation. Oracle PASS: caster and receiver agree —
not a convention mismatch.

### Which cascade the cliff is in / where the atlas goes empty

- Receiver cascade pick = camera-space z vs zEnd; the grass receiving the cliff shadow
  at >14.7 m from the camera is in **cascade 2** (zEnd 120 m). Player itself is
  ~1472 m from the parked camera (camera is far, looking at the parked player), so the
  player is also clamped to cascade 2. Everything beyond ~120 m cam-distance has no
  cascade coverage at all.
- Raw DSV empties: c0 clear 62.9% (empty across top, geometry bbox uv y ≤ 0.887);
  c1 clear 55.8%; **c2 clear 9.1%, confined to uv x[0.00,0.57] × v[0.67,1.00] — the
  upper-left quadrant of the cascade 2 atlas is empty** (see atlas_clear_2.png).
  c2 min depth 0.185 (nearest caster far from the near plane), c0/c1 min ~0.
- Chunk 9's AABB projects OUTSIDE the c2 box on all axes (uv x[-5.7,2.5], y[-2.3,4.5],
  z[-1.6,5.4]) — its AABB passes cull, actual coverage is the PGM's mid blob only;
  the empty upper-left is where the near/camera-side part of the cliff face should cast.

### Handoff for task 2

- The straight cut-out edge = a plane (projects straight); prime candidates: the c2
  box's far/xy edges or the 120 m coverage boundary (receiver skips out-of-box → no shadow).
- For any world point, per-cascade atlas uv is computable from the solved affines
  (in /tmp/cull.py, `M[c]` rows for x/y/z, uv = 0.5+0.5·X, 0.5-0.5·Y, rel to anchor
  (-1095.06,521.49,1596.89)). Cliff chunk 9 corner (-1870,609,1906) → rel (-774.94, 87.51,
  309.11) → c2 uv (1.90,-2.26) out-of-box.
- Caster counts do NOT change per cascade (same {9,13}) — the bug is not chunk culling.

## round 2

Task 2 — clip-plane attribution (one-line root cause). All numbers from the
frame-300 parked run (cull.py M[2] affine, oracle-verified against in-engine
readback "player NDC feet (-0.086, 0.422, 0.769)" — exact match; fit resid 2e-15).

### Setup correction (important for later tasks)

The shadow cascade is distributed around `diligentFrameView()` — the
third-person orbit camera near the PLAYER, anchor (-1095.06, 521.49, 1596.89)
= render-space origin. The parked FLYING camera (-320,588,344, 1472 m away)
is only the screenshot vantage. So "zEnd 120 m" / "distanceM 120 m" are
distances from the orbit cam near the cliff, NOT from the flying camera.
The c2 box lives at ~1275–1675 m from the flying cam = right on the cliff.

### c2 light box (world meters, light-aligned)

half-width light X = light Y = 1/0.0071 = 140.8 m; depth light Z =
1/0.0034 = 294 m (near NDC 0 → far NDC 1). Box center ≈ (-991, 554, 1648),
i.e. hugging the cliff. Face distances from orbit anchor: far Z=1 ≈ 53 m,
near Z=0 ≈ 243 m, X=±1 ≈ 126/157 m, Y=±1 ≈ 69/214 m.

### The cliff (real geometry, extracted from pak_1 GLB models/terrain/oghuzlands.zstd → terrain_chunk_1_2 = chunk 9)

Visible rock wall (terrain y>505 within 650 m of player): world AABB
x[-1393,-580] y[500,593] z[1109,1906] — an ~810 m long, ~90 m tall wall
("the long cliff"). In c2 light space (NDC): X[-2.67, 0.99], Y[-2.35, 2.06],
Z[-0.61, 2.13] → meters X[-376,140] Y[-331,290] Zdepth[-178,628].

### Overflow vs the c2 box ([-141,141]² m × [0,294] m depth)

The cliff overflows the box on EVERY face: 236 m past the west X=-1 face,
~150–190 m past both Y faces, 178 m past the near plane, and 334 m past the
FAR plane (Z=1). The cascade only contains the central ~1/3 of the wall.

### Which plane carves the cut

The shadow falls along the light-travel dir r3=[-0.473,-0.788,-0.394] (down,
-x, -z) = +light-Z. The cliff sits at small light Z (between sun and grass);
the receiving grass is at larger light Z. Moving down-slope with the shadow,
the grass's light-Z increases at constant light X/Y and leaves the c2 box
through the FAR plane Z=1; the cliff portion that would occlude the grass
beyond that line is at light Z>1 (far-clipped, 334 m of wall) and is not in
the depth atlas → those grass samples read clear → fully lit. The straight
cut edge = world line (cliff ∩ far plane Z=1) projected onto the grass.
Not the 120 m receiver-coverage boundary (grass at the cut is still inside
the 120 m tier / inside the box in X/Y), not the near plane, not pure xy
(the far plane is crossed first along the shadow direction).

### ONE-LINE ROOT CAUSE

Cascade-2 light-space FAR plane (Z=1, far end of the 294 m-deep light box at
the 120 m tier end) carves the cut-out: the ~810 m cliff wall overflows the
±141 m / 294 m c2 box by ~334 m in light depth, so the far part of the wall
(and its shadow) is clipped at the far plane, leaving a straight lit band
where the grass leaves the box.

### Fix implication for task 3

Receiver is in-box at the cut; the missing occluder is a CASTER far-clip.
Caster-only far-plane extension (pad the caster light-space Z beyond 1 while
keeping the receiver matrix/box unchanged) is the targeted fix — matches
plan approach 1. A pure tier distanceM bump (approach 2/4) would also move
the far plane outward but coarsens all cascades. 120 m coverage / xy / near
are ruled out.

## round 3 (partial, summarized)

Task: summarize partial work (ledger + git diff of ShadowDiligent.cpp) and
compare /tmp/fixed.jpg vs /tmp/baseline.jpg.

### What exists now (working tree, uncommitted on top of 93e31c7)

`c-engine/renderer/diligent/ShadowDiligent.cpp` — implements plan approach 1
(caster-only light-space far-plane pad), uncommitted:
- `casterW2LP[8]` matrix array: after `mgr.DistributeCascades`, per cascade the
  WorldToLightProjSpace is copied and its light-Z columns are multiplied by
  `farPadS = 1/(1+K)` (K = 1.5 default, env `ENGINE_SHADOW_FAR_PAD` clamped
  [1,8]) so casters up to K·range past the box far plane still map into
  [0,1] in light Z.
- Same `farPadS` applied to `f4LightSpaceScale.z` and
  `f4LightSpaceScaledBias.z` so the receiver sampling path reads the same
  remapped depth the casters wrote.
- Every draw/probe path now uses `casterW2LP[i]` instead of
  `mgr.GetCascadeTransform(i).WorldToLightProjSpace`: player shadow draw,
  terrain shadow draw, oracle dump, debug `shadow dbg` line, NDC probe.
  Light-view matrix, box XY, cascade picks, uv mapping untouched.
- No other source files changed (git status: only ShadowDiligent.cpp + AGENTS.md
  dirty, .pi/ untracked). Build artifact state unknown from here.
- NOTE: the diff adds a large explanatory comment block — violates the
  "Do not use comments in the code" rule; strip before commit.

### Screenshot comparison /tmp/baseline.jpg vs /tmp/fixed.jpg (parked vantage,
player/camera readouts identical)

- Baseline: cliff shadow = dark wedge on the grass with a straight hard edge;
  straight lit cut-out band beyond it (the reported bug).
- Fixed: the straight cut-out band is GONE — but so is the ENTIRE cliff shadow.
  The grass region that held the shadow wedge in the baseline is now uniformly
  lit; no dark band anywhere in that area.
- Verdict: the fix regressed from "partially missing shadow (straight cut)" to
  "cliff shadow missing entirely on the grass" (over-correction: the pad +
  receiver remap apparently killed in-box shadow hits too, not just extended
  them).
- New artifacts: none else visible — no shadow acne or peter-panning on the
  cliff face or elsewhere; cliff surface and terrain elsewhere identical;
  no changes in other screen regions (UI/terrain pixels match).

### Handoff

Suspect candidates to check: (a) `farPadS` applied to the *biased* z of the
caster matrix — the matrix's own far-plane mapping (row 3/4 in NDC z terms)
may now map the box far plane to NDC < 1 differently than the scale/bias remap
assumes, shifting in-box depths so receivers compare against the wrong range;
(b) depth-bias sign interaction: with light-Z squeezed 1/2.5×, the fixed
fractional bias in NDC is 2.5× larger in light units on casters but the
receiver compares against remapped depth — confirm the bias is not pushing
every in-box sample into self-shadowing-miss (i.e. reading clear); (c) verify
with a smaller K (`ENGINE_SHADOW_FAR_PAD=1.0`) or with K applied to caster only
without the receiver scale/bias remap, plus a fresh `ENGINE_SHADOW_DUMP` of the
c2 atlas to see if the cliff now fills the atlas and whether in-box depths
shifted.

## manager note (post round 4)
- Reverted c-engine/renderer/diligent/ShadowDiligent.cpp to baseline (round-3 regression: far-pad squeezed in-box shadow entirely out).
- Lesson for re-land: the z-squeeze (equivalent to far-plane extension by 1+K for ortho light) must ALSO scale the sampling-side depth bias (fFixedDepthBias -> *farPadS); unscaled fixed bias becomes (1+K)x in light units => shadow vanishes/peter-pans. No comments in code (project rule). Verify with parked screenshot + ENGINE_SHADOW_DUMP c2 atlas fill, compare /tmp/baseline.jpg.

## round 5 (task 4: re-land far-pad with sampling bias scaled)

Re-landed the far-pad fix exactly as the manager specified (no comments in code):
`farPadS = 1/(1+K)` default K=1.5 (env `ENGINE_SHADOW_FAR_PAD` overrides K in [1,8]),
caster W2LP z-column scaled by farPadS into a `casterW2LP[]` used by all depth draws,
`f4LightSpaceScale.z` + `f4LightSpaceScaledBias.z` scaled by farPadS,
`fFixedDepthBias *= farPadS`. Builds clean, no LSP diagnostics.

Same-state evidence (all runs pinned to the flying camera anchor (-320,588,344) at
frame 100; baseline vs fixed byte-identical c2 raw dumps except the remap):
- c2 raw atlas: baseline clear 14.8% (empty quadrant) -> fixed 0.0%; raw max 0.33
  -> 0.52, i.e. caster content now extends ~30% past the old far plane; c2 clear
  gone. The far extension itself works (atlas fill verified).
- Filterable (EVSM) atlas verified == warp(raw) texel-for-texel in the fixed state.
- Render: shadow still absent - the wedge is gone (same as round 3). Same-state
  side-by-side (baseline3 vs fixed, flying anchor): baseline has the dark wedge +
  straight cut; fixed has no shadow at all on the grass. The cliff shadow does NOT
  extend past the old cut; the regression persists.

Root-cause update (supersedes the "unscaled bias" explanation for this build):
the active mode is EVSM4 (shadowMode 4) and its splat-PS branch does NOT apply
fFixedDepthBias at all (only the PCF branch does: `LightDepth = cascadeNdc.z -
sBiasParams.y`). Scaling fFixedDepthBias therefore cannot affect the rendered
result. What does: the far-pad compresses all shared NDC depths into [0, farPadS]
(~[0,0.4]); the EVSM warp exp(40*(2z-1)) then lands in its underflow half
(w1 ~ 1e-10..1e-4, M1^2 ~ 1e-20 = subnormal f32), so the Chebyshev test degenerates
to lit (GPU PS debug readback: pPos~pNeg~0.8 on in-box wedge pixels where the
CPU Chebyshev over the same atlas gives p~0). Baseline depths span [0.33,1.0]
where the warp has full dynamic range, which is why baseline works.

Suggested next direction for task 5 (not done here, out of scope): either
(a) remap the EVSM warp exponents to the compressed range (both converter and PS,
i.e. warp on z/farPadS), or (b) keep the [0,1] mapping and extend the cascade
light-box far plane in DistributeCascades instead of scaling the z-column
(the original plan "approach 1"). Either way the fix must stay in this engine's
files if possible; the warp remap touches DiligentFX EVSMHorzPS + the PS cbuffer.

Cleanup: temporary splat-PS debug instrumentation reverted (git checkout).

## round 6 (task 5: EVSM warp remap to z-tilde)

### Implemented (all in working tree, not committed)

- Converter (`ShadowConversions.fx EVSMHorzPS` + `ShadowMapManager`): z-tilde
  warp `exp(E·(2·depth/farPadS − 1))`, exponents range-scaled by
  `evsmRange = farPadS/(2−farPadS)` (E 40→10, 5→1.25), cleared-texel
  exclusion + point-mass sentinel. `SetEVSMFarPadS()` delivers farPadS.
- PS (`splat_terrain_ps.hlsl`): `warpDepthEVSM(depth, f4ShadowFade.z, out ex)`
  mirrors the same z-tilde + range scaling; min-var floor uses the scaled
  exponents. farPadS travels via `f4ShadowFade.z` (staged in
  `SplatTerrainDiligent.cpp` from `shadowDiligentFarPadS()`).
- Two enabling changes in the far-pad state (`farPadS < 1`):
  1. `sa.iFixedFilterSize = 2` → DiligentFX `bSkipBlur` → the converter writes
     PER-TEXEL point-mass moments (the fixed 5-tap horizontal moment filter
     was smearing the cliff into the grass; at E=10 that smear is fatal).
  2. New nearest sampler `g_ShadowMapNearestSampler` for the EVSM 8-tap moment
     sampling (linear 4-texel mixing of point-mass moments defeats the test
     at compressed exponents; nearest = per-texel test, 8 taps = PCF).
- Instrumentation kept (env/debug-gated): PS debug submode 4
  (`(pPos,pNeg)` per-tap Chebyshev map, `ENGINE_SPLAT_SHADOW_DEBUG=4`) and 5
  (uv map); `ENGINE_EVSM_RAWDUMP=path` writes raw-depth PGM per slice
  (alongside `ENGINE_EVSM_DUMP` filterable log10(R) PGMs) in the
  `ENGINE_SHADOW_EVSM=frameN` probe.

### Verified

- Atlas (c2, skip-blur state) is exact point-mass z-tilde E=10: raw 0.145 →
  M1 0.0631 (= e^(10·(2·0.3625−1)) ✓); per-column first-hit depths read back
  per-texel.
- PS is self-consistent with the atlas: per-pixel pPos correlates 0.998 with
  atlas M1 at the PS's own uv.

### Why the acceptance criterion still fails (wedge does not extend)

The wedge vanished in the far-pad state (fixed.jpg shows only a thin sliver
at the ridge; the whole in-box band + past-cut band render lit). Chain of
evidence:

1. Cascade selection is IDENTICAL in both states (slice map, mode 1): the
   wedge/band receivers pick **c1** in baseline AND far-pad.
2. Far-pad c1 columns at the wedge/band receiver uv (PS frame) are CLEARED
   (raw 1.0 → sentinel → Chebyshev clamp → lit). The cliff casters live in
   **c2** (c2 raw 0.145–0.149 = the wall at the receivers' uv).
3. Chunk culling is identical between states (c0 3/16, c1 3/16, c2 4/16 on
   frame 1; 2/16 steady) — the same chunks are drawn into c1 in both states.
4. So the c1 caster uv FOOTPRINT moved relative to the receiver uv between
   states even though chunk counts and the light-view XY box are nominally
   unchanged. Missing datapoint: baseline c1 raw PGM (the RAWDUMP probe lives
   in the working tree; a baseline run needs the far-pad code stashed, which
   stashes the probe too — port the ~20-line RAWDUMP hunk to a baseline
   checkout to fill the gap).

Likely suspects for the footprint shift: f4LightSpaceScale.xy /
f4LightSpaceScaledBias.xy are taken from the DiligentFX cascade transform
(unpadded box), while casters rasterize through `casterW2LP` (z-column
padded). If any xy component is derived from the padded range anywhere on one
side only, casters and receivers desynchronize in uv. Check by dumping
c1 raw in both states (PS-frame coords) and diffing the caster footprint.

### Pitfalls found this round

- CPU atlas readback PGMs are Y-FLIPPED relative to the PS uv frame (the
  uv map / pPos map correlation is symmetric here, but per-texel reads must
  use the PS frame, i.e. sample PGM row (1−v)·H).
- `FILTER_TYPE_NEAREST` does not exist in Diligent — it is
  `FILTER_TYPE_POINT`.
- `sa.iFixedFilterSize = 2` is DiligentFX's "skip blur" sentinel (radius 0);
  `= 5` (tier.pcfFilterSize) forces the 5-tap horizontal moment filter.
- The PS's 8-tap Poisson loop is hard-coded; `sTail.y` (iFixedFilterSize)
  is not a PS-side tap count, so converter-side filter changes don't affect
  the PS.
- The EVSM debug IBL values are post-tone-map (monotone, fine for maps).

### State

`/tmp/fixed.jpg` (12:40) = current state (remap + skip-blur + nearest
sampler + RAWDUMP code). `/tmp/baseline_same.jpg` unchanged. Diff: wedge
band −(gone), past-cut band +0.0 (still lit). Acceptance NOT met.

## round 7 (task 6) — EVSM rawdump port + baseline capture + wedge desync — PARTIAL

### Done
- RAWDUMP probe (ENGINE_SHADOW_EVSM=frame) is in the baseline build (ShadowDiligent.cpp): dumps raw DSV + filterable atlas per slice as PGMs; ran clean in both states. Baseline pinned captures: /tmp/base_raw.{0,1,2} (+ farpad_raw/flt, evsmraw).
- UV-footprint diff (baseline vs far-pad, all cascades, CPU mask): far-pad ⊇ baseline everywhere (base-only texels = 0 in c0/c1/c2). No culling/xy shift. The far-pad state is a strict z-compression of the same content. The "suspect xy scale/bias" desync hypothesis is REFUTED.
- Staging fix: SplatTerrainDiligent.cpp:992 was staging f4ShadowFade.z = 0.0f; now shadowDiligentFarPadS() (0.4). Necessary (GPU read 0.0 at screenshot frame), but NOT sufficient.

### What the wedge desync actually is (renderdoc frame-300 cbuffer+atlas reads + PS instrumented runs)
- GPU atlas (R32G32B32A32 2048^2x3, ResourceId::1089) matches CPU readback within noise at the wedge uv in all slices. PS uv in [0,1] (wedge c2 uv ~ (0.47, 0.32)), cascade pick = 2 in both states (identical cascade maps).
- Far-pad state is UNSTABLE OVER TIME in the pinned run:
  - screenshot frame 100: wedge LIT, whole scene mostly lit (fixed_pn.jpg). PS measured f4ShadowFade.z = 0.0 and receiver z unscaled at that frame.
  - frame 300 (rdc): cbuffer correct (pad 0.4, scale/bias z scaled x0.4, caster W2LP x0.4, atlas z-tilde verified) — yet scene is 63% dark (1.2M dark px vs baseline 368K) = OVER-shadowed.
  - frame 1000: LIT again (fixed_f1000.jpg, 285K dark ~ frame 100).
- So the failure is not one steady-state desync but an interaction of the far-pad z-machinery (caster z x0.4 + receiver scale/bias z x0.4 + z-tilde warp 2z/pad-1 + skip-blur point-mass moments) with per-frame state (passReady warmup, caster chunk streaming 3/16 -> 2/16, focus band). In baseline the z window [0,1] + E+40 warp is self-consistent; in far-pad the wedge receiver's Chebyshev mean/m1 relation flips to LIT in the early/late frames and to over-shadowed mid-run.
- A temporary PS v-flip test (cascadeUV y flip) "restores" the wedge in far-pad AND extends the baseline wedge past any cut (flipv_baseline tip x=2578 vs baseline 1781) — so the flip is a convention change, NOT the fix; it was reverted. Do not ship it.
- PS debug instrumentation used: submode 6 (first-tap m1), 7/8/9 (forced slice), 10 (z, log-w, pad), 11 (raw uv) — all in /tmp ps copies only; repo PS is clean round-6.

### Files / artifacts
- /tmp/round7_fixed.diff = full fix state (round 6 + staging line); repo is in this state now.
- rdc frame-300 captures: fixed state atlas slices saved /tmp/rdc_atlas_s{0,1,2}.npy; the current /tmp/RenderDoc/c-game_frame300.rdc is the BASELINE capture (13:32, ~950MB) — delete when done. /tmp/rdc-frame/unnamed_eid594_out0.png = baseline frame-300 output.
- RenderDoc SWIG replay API notes: ReplayController_GetTextureData(rid, Subresource{mip,slice}) works for full-slice RGBA readback; GetConstantBlocks(ShaderStage.Pixel, false)[i].descriptor -> GetBufferData(rid, byteOffset, byteSize); splat cbuffer = 2608B (prefix 1376 + anchor 16 + ShadowMapAttribs 1200 + shadowFade 16); Cascades[] at ShadowMapAttribs+64, 64B each {scale, scaledBias, startEndZ, margin}; f4ShadowMapDim at +1120.

### Remaining (precise)
1. Pin the far-pad time dependency: add a one-line per-frame log (frame, passReady, caster chunks, tier, farPadS, shadowFade.z staged, c2 scale.z/bias.z) for frames 50..1000 step 50 in both states; identify which variable flips between the LIT (100/1000) and over-shadowed (300) windows.
2. Once the flipping input is named: fix its warmup/ordering (likely passReady-gated state that the cbuffer/caster path uses at different frames — the 100/300/1000 pattern smells like a one-frame-stale or re-init race in shadowDiligentUpdateFrame vs splatFrameFill).
3. Re-capture /tmp/fixed.jpg after the fix; acceptance: wedge visible AND extending past the old straight cut (baseline tip x=1781 orig px at y~870).
