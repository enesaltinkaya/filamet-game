# notes

## brainstorm

## Core difficulty

The per-frame cost is per-variant tris × instances, re-rasterized twice (color + CSM shadow), and the near-LOD deciduous mesh (28,344 tris/variant) is the only variant family whose per-variant count is 2–90× the others — so the task is a ≥3× per-mesh cut that must keep the canopy silhouette, and the silhouette can only be checked by screenshot A/B, not by any numeric gate.

## Reductions / key lemmas

1. Exact, seed-independent tri formula below the cap. Per branch band: 2·sections·radialSegs tris + radialSegs for the top cap (+ radialSegs base cap on the trunk). Per tip: cardCount × 4 tris (cardCountMin=cardCountMax, so exactly 30 near / 6 far per tip). Tips = Π per level of (children + continuation). All rng-driven terms (tilt, azimuth, size, jitter, length scale) affect only positions, never counts. Check against baseline: full-model near ≈ 39.8k, far ≈ 8.1k — both exceed their maxTris caps (30,000 / 7,200), so the recorded 28,344 / 6,688 are TRUNCATED meshes: `g.room()` stops emission mid-canopy at the cap. Consequence: the baseline screenshots already show partially missing canopies at some tips, and "keep the look" must be judged on silhouette extent, not interior pixel match.
2. Last-level continuation spines are invisible waste. Radius chain: 0.0225 × 0.63 × 0.76 × 0.70 ≈ 0.0056 → clamped to 0.0015 (sub-pixel at any viewing distance), yet each continuation job inherits its parent's full resolution (12 sections × 12 sides = 300 tris) and there are 40 of them at the final level of the near tree ≈ 12k of the 28.3k tris. Setting `lev[2].continuation=false` deletes the whole block (final level becomes children-only jobs at 4×3 resolution, 27 tris each).
3. Cards are the second block: pre-truncation near = 160 tips × 30 cards × 4 = 19.2k tris; tips = (7+1)(4+1)(3+1) = 160. Cutting tips (children 4→3 at level 2) and cards (30→~6) plus lemma 2 lands near at ~9,870 with children [7,3,3], continuation [T,T,F], cards 6 — computed exactly, not estimated.
4. GPU load scales 1:1 with per-variant tris (instances 35,957 and draw count 21 are set by the scatter, unaffected by mesh work; the shadow pass re-rasterizes the same cards). Alpha-card overdraw is an extra multiplier but secondary; shadow-pass work is a separate lever.
5. LOD switch is a one-constant change: far variant is assigned at tile build when dist > LOD_SWITCH+MARGIN; RESCATTER_DIST=100 must stay ≥ new SWITCH+MARGIN (100 ≥ 76 for a 60 m switch) — no logic change needed.
6. Far LOD has exactly 1 variant (no diversity to hide a silhouette change); near has 4. The far A/B at 100–200 m is the gate for both the far cut and any switch-distance pull.

## Candidate approaches

A. **Structural config cut in TreeGen.cpp only** — `lev[2].continuation=false` + cards 30→~6 + `lev[2].children` 4→3 + cardSize bump (~0.040→0.055) near; same shape far (cards 6→~3, children 7→5, continuation off); lower maxTris to sit above the new totals (30k→~12k, 7.2k→~4.5k). Main risk: canopy reads as sparse/thin after the cut — mitigate with cardSize and judge on A/B silhouette. Effort: small (iteration loop is seconds with a counting harness, 1–2 screenshot rounds).
B. **Pull LOD switch 100→~60 m** (one constant, lemma 5) to put more trees on the cheap far variant, on top of A's far cut. Risk: popping across the 60–76 m band and far LOD looking too blobby at 60 m — check in A/B. Effort: ~30 min + one A/B.
C. **Shadow-pass reduction**: shadow-only low-tri mesh or solid-quad shadow cards (skip crossed-card alpha in the depth pass). Bigger GPU-time win than tri count alone (kills shadow overdraw), but touches the DiligentRenderer props/shadow passes. Effort: medium; only if the profile after A+B still shows the props pass dominant.
D. **GPU overdraw mitigation** (depth sort, alpha-to-coverage, card pre-pass). High effort, renderer-level risk; not worth it while 28k→~10k tris is on the table.

## Recommended approach

A first: it removes the two provable waste blocks (≈12k invisible spines + ≈14k of 19.2k cards) with pure config changes in one file, and because tri counts are exact and seed-independent below the cap (lemma 1), every iteration is a seconds-long compile instead of a full game run. What must be true for it to work: the harness must compile the identical TreeGen.cpp (so counts match the in-game build bit-for-bit), the A/B shots must use the same cell/camera/seed, and the canopy silhouette — judged against the baseline's (truncated) canopy, i.e. silhouette extent rather than interior detail — must hold at near and far range. If the profile after A still shows the props pass dominant, B (small) before C (medium).

## Proposed tasks

1. **Tri-counting harness**: standalone main that compiles `c-utils/treegen/TreeGen.cpp` (stubbing/pointing at its Utils.h include path), calls `treegen::generate` for every config with the exact variant seeds `seed ^ (s*0x9E3779B9) ^ (v*0xC2B2AE35)` used in `AzgaarPropMesh.cpp` buildMesh (seed from the `azgaarPropMeshBuild(seed)` call site in AzgaarProps.cpp), and prints per-variant tri totals with a bands-vs-cards split. First run must reproduce 28,344 / 6,688 and confirm the last-level-continuation ≈12k figure (task 1 baseline capture still stands for GPU time).
2. **Near deciduous cut** in `configDeciduous()`: start at lev[2].continuation=false, lev[2].children 4→3, cardCountMin/Max 30→6, cardSize 0.040→0.055, maxTris 30,000→12,000; iterate in the harness until all 4 variants ≤9,999 (analytic start point: ~9,870), then one full build + screenshot A/B at the baseline cell.
3. **Far deciduous cut** in `configDeciduousFar()`: start at lev[2].continuation=false, lev[1].children 7→5, cards 6→3, maxTris 7,200→4,500 (analytic start point ~2,900); harness gate ≤3,500, then screenshot A/B at 100–200 m (single-variant species: silhouette must hold on its own).
4. **Verify**: RenderDoc re-capture, compare `props` pass GPU time and total tri load vs the baseline recorded in task 1, confirm no new VUIDs, run the pinned verification command (must print VERIFICATION PASS). Optional follow-up: pull AZGAAR_PROPS_TREE_LOD_SWITCH to 60 if the far A/B holds.

## round 1

Task 6 (tri-counting harness) done: `c-tools/treegen_tricount/` (main.cpp + stub Utils.h + build_tricount.sh), compiled standalone against the unmodified `c-utils/treegen/TreeGen.cpp` with the same clang++/-std=c++23/-O0 as the game Debug build (bit-identical FP behavior; c-tools is NOT referenced by any CMakeLists, game build untouched).

Usage: `c-tools/build_tricount.sh && build/c-tools/treegen_tricount <seed|0xhex|mapname> [--spine]`. Seed = the map seed buildMesh gets (FNV-1a of the map NAME, e.g. `Chilerel` → 0x5d28ace1; NOT the filename — FNV of the filename gives a different seed). `--spine` adds the A/B measurement (full vs lev[levels-2].continuation=false).

Baseline reproduced EXACTLY (seed 0x5d28ace1, matches /tmp/base_run.log line 156): conifer 307/310/316/352, conifer_far 195, deciduous 28344×4, deciduous_far 6688, acacia 292/292/268, dead_tree 220/226, shrub 169/196/193/193. Deterministic across runs.

Two brainstorm corrections (affect tasks 2/3 math):
1. Baseline is NOT truncated. 28344 < maxTris 30000 and is the FULL model: cards = exactly 19200 = 160 tips × 30 × 4 (no room() drop anywhere), bands = 9144 (includes a small rng-dependent top-cap term). Lemma 1's "full ≈ 39.8k, truncated" is wrong; the no-spine model is 21048 < 30000 too. Consequence: the "analytic ~9870" start point for task 2 must be re-verified in the harness — spine-off + cards 6 gives ~10.5k (bands 6648 + cards 3840), slightly ABOVE the 9999 gate; tips must also drop (lev[2].children 4→3 → 128 tips → 3072 cards).
2. Last-level continuation spine block in near deciduous = EXACTLY 7296 tris/variant (measured A/B, full=28344, no-spine=21048), not ~12k. Only 1 of the 40 level-3 spines is 12×12 res; 7 are 8×6, 32 are 6×4 (continuations inherit their immediate parent's resolution, not the trunk's). Split: 2496 band + 4800 card tris. Far deciduous spine = 1544.

Split labels: CARDS species split by UV (bark rect v∈[0.10,0.22] vs leaf rect v≥0.34 — exact); other treegen species (CONES/FAN/BLOBS) split by vertex colour (white {1,1,1} = leaves, trunk-colour = bands).

## round 2 (curation)

- Task 6 marked done (harness + exact baseline reproduction + the two corrections above). No duplicates merged; no new sub-tasks needed — the round-1 corrections refine tasks 2/3 in place, and the optional LOD-switch pull already lives in task 3.
- /tmp/RenderDoc/ is EMPTY: no RenderDoc capture has happened yet, so task 1 is still fully outstanding (the headless numeric baseline in /tmp/base_run.log is recorded in plan.md; the ~07:4x-08:5x screenshots in /tmp predate this task and are not a usable A/B reference; the 17:3x lod_*.jpg have no recorded cell/camera/seed). Task 2/3 descriptions updated with the round-1 corrections (old "analytic ~9870/~2900" targets are void).
- Next: task 1 - RenderDoc baseline profile + baseline screenshot; unblocks the A/B gates in tasks 2/3 and the task-4 GPU-time comparison.

## round 2 (task 1)

Task 1 done: RenderDoc baseline capture + A/B reference shot at the tree-dense cell, props-pass profile recorded. Tooling for the task-4 comparison: `scripts/rdc_props_pass.py [capture]` (replay-API: draw-level breakdown + per-pass EventGPUDuration/RasterizedPrimitives/SamplesPassed counters; run with `PYTHONPATH=/home/enes/Apps/renderdoc/build/lib`).

**Cell/camera/seed (all runs below):** map `azgaar/Chilerel 2026-08-11-15-35.map` (only map; mapSeed 0x5d28ace1), camera = saved free-cam state `(-303.1, 5.0, -427.2) yaw -28 pitch -17` (HUD "Cell: 5050") — a dry-savanna woodland: many near deciduous + far-trees on horizon + dense grass/shrubs. Player parked at densest-props point (28450,7,-10450) but the SCRIPTED camera owns the view; automated runs never fly, so the saved camera state is stable and the scene is deterministic across runs. World state identical to /tmp/base_run.log: 8 resident tiles, 2 emitting 35957 instances, 21 draws.

**Artifacts:**
- Capture: `/tmp/RenderDoc/c-game_frame300.rdc` (598 MB). Reproduce: `ENGINE_RENDERDOC_CAPTURE=1 ENGINE_RENDERDOC_CAPTURE_FRAMES=300 ENGINE_HIDDEN_WINDOW=1 ENGINE_LOG_TIMEOUT=600000 ./scripts/run.sh renderdoc` (run.log /tmp/rdc_base_capture.log).
- **Baseline A/B shot: `/tmp/treeopt_baseline_frame300.jpg`** (frame 300, 2880x1627, HUD: fps 60, frame 16.67 ms, cpu 1.24 ms, gpu 7.29 ms). Reproduce (separate run, same cell/camera/seed; 1-frame off vs the capture — TAA jitter only, camera static): `ENGINE_SCREENSHOT=/tmp/treeopt_baseline_frame300.jpg ENGINE_SCREENSHOT_FRAME=300 ENGINE_HIDDEN_WINDOW=1 ENGINE_LOG_TIMEOUT=300000 ./build/c-game/c-game` (log /tmp/treeopt_base_shot.log). Tasks 2/3 A/B shots MUST use these exact env vars/paths.
- Props-pass color output dump: `/tmp/rdc-dump/props_eid349_out0.png` (HDR R16F → looks dark on save, expected).

**Frame 300 structure** (185 draws, 0 dispatches): shadow eid 49-233 (65 draws), player 238-247 (1), terrain 249-302 (25), **props 304-349 (21 draws)**, taa_resolve 351-654 (39), rmlui 657-767 (34).

**Props pass (color) — draw-level** (21 draws, 35957 inst, submitted tri load 5,760,567; idx counts corroborate task-6 harness: 85032 idx = 28344 tri, 20064 = 6688):
- grass: 14 draws (eid 309-321, 333-345), 35413 inst, 141,652 tris (2%). Per draw ~4500 inst (7) + ~520 (7).
- deciduous near: 4 draws (eid 323,325,327,329), 93 inst (25/22/24/22), 2,635,992 tris (46%). Rasterized per draw: 198.5k / 282.9k / 268.0k / 152.0k.
- deciduous_far: 2 draws (eid 331: 355 inst, 347: 91 inst), 446 inst, 2,982,848 tris (52%). Rasterized: 1.114M / 74.8k — the single biggest rasterized-tri draw in the frame.
- reed: 1 draw (eid 349), 5 inst, 75 tris.
- Pass totals: rasterized 2,150,864 tris (37% of submitted — frustum/viewport clipping), SamplesPassed ≈ 3.2M (viewport 4.68M px). Overdraw hot spot: eid 325 (deciduous 22 inst) = 1.475M samples for 282.9k tris ≈ 5.2x; grass draws up to 28x (small on-screen cards); far LOD at 100-200 m ≈ 0.17x.

**Shadow pass re-rasterizes the same cards 3x (3 cascades):** props draws inside shadow = deciduous 12 draws/279 inst/7.91M tris, deciduous_far 4/1156/7.73M, grass 28/98859/0.40M, reed 1/5/75; plus 6 terrain (390150-idx) + 13 player draws (2.28M tris). Tree-card load in shadow = 15.64M tris vs 5.62M in color. Shadow pass rasterized 6,352,500; terrain 832,484; player 23,111; frame total 9,359,730.

**Props-pass GPU time** (replay `EventGPUDuration` counter, per-pass event sums — method fixed for task 4 via scripts/rdc_props_pass.py): props 11.9-12.7 ms, shadow 5.0-7.0, terrain 2.6-6.6, taa_resolve 1.0-1.5, rmlui 0.17, player 0.03; whole-frame sum 21.5-27.2 ms (replay-to-replay jitter ±25%). CAVEATS for task 4: (1) the sum EXCEEDS the wall-frame GPU time (engine's own QUERY_TYPE_DURATION: 7.29 ms at frame 300 in the shot run) — per-event durations overlap under RADV pipelined timestamps, so absolute ms are inflated; compare PASS SHARES and A/B deltas with the identical method, not absolute ms; (2) per-draw times are noisy (not workload-monotonic) — use the deterministic RasterizedPrimitives/SamplesPassed columns for draw-level conclusions; (3) grass variants (idx 12) and the 4 near deciduous variants (idx 85032) are not disambiguatable in this replay build (no firstIndex on ActionDescription) — species-level numbers are exact, variant-level is not.

**Verdict for tasks 2/3/4:** the props pass is the biggest per-pass event-time share (≈44%) AND the frame's rasterization is dominated by deciduous (near 46% + far 52% of submitted props tris; + the 3x shadow re-rasterization of the same cards). Both the tri cut (tasks 2/3) and the shadow-pass lever (approach C) are on the table — the profile does NOT show a pure fill-bound frame (rasterized = 37% of submitted), so triangle count is a valid primary metric; overdraw on near cards (≤5.2x) is a secondary multiplier that shrinks with the same card cut.

## round 3 (curation)

- Task 1 marked done (capture + A/B ref shot + full props-pass profile recorded in round 2; artifacts verified on disk: /tmp/RenderDoc/c-game_frame300.rdc, /tmp/treeopt_baseline_frame300.jpg, /tmp/rdc-dump/, build/c-tools/treegen_tricount, scripts/rdc_props_pass.py). No duplicates merged.
- No new sub-tasks: the round-2 profile verdict keeps the plan's ordering — the frame is tri-bound (not fill-bound), so the tri cut (tasks 2/3) goes first; the LOD-switch pull already lives in task 3 as an option; shadow-pass approach C stays deferred to the task-4 re-capture (only if props pass still dominates). Task 2/4 descriptions updated in place (A/B ref path + no-absolute-ms comparison rule).
- Next: task 2 — near-LOD deciduous cut 28344 → ≤9999 tris ×4 variants. It is the largest per-variant count, 46% of submitted props tris, and the 5.2x overdraw hot spot sits on these cards (eid 325). Iterate in the task-6 harness (gate: all 4 variants ≤9999 with margin), then one full build + A/B vs /tmp/treeopt_baseline_frame300.jpg with the exact env vars from round 2.

## round 3 (task 2)

Task 2 done: near-LOD deciduous cut, all 4 variants 28344 → 9144 tris (3.10x cut, ≤9999 with ≥800 margin on any seed).

**Final configDeciduous (c-utils/treegen/TreeGen.cpp, only function touched):** lev[2].children 4→3, lev[2].continuation true→false, cardCountMin/Max 30→9, cardSize 0.040→0.070, maxTris 30000→12000.

**Structure math (corrects the round-1 tip estimate):** lev[2].children is consumed by level-1 jobs (clv = lev[level+1]), so 4→3 drops level-2 jobs 8×5=40 → 8×4=32; with lev[2].continuation off the tips are 32×3 = **96** (not the 120 the round-1 correction assumed — that still used 40 level-2 jobs). Baseline 160 tips = 8×5×4. Card tris are fully deterministic (min=max, tips fixed by structure); bands 5688 ±48 across seeds (only level-1 top caps flip, ≤8×6; level-2/3 tips can never re-earn a cap since r1 ≤ 0.0039 < 0.004). So per-variant total ∈ [9144, 9192] on ANY seed — structurally under 9999.

**Why 9 cards @ 0.070 (deviation from dispatch starting point 6 @ 0.055):** the 96-tip structure freed budget the dispatch's 120-tip estimate did not have. Screenshot A/B (same cell/camera/seed, exact round-2 env vars, frame 300): 6 cards @ 0.055 (7992 tris) preserved silhouette EXTENT but the near canopy read visibly sparser than baseline AND sparser than its own unchanged far-LOD counterpart — rejected. 9 cards @ 0.070 (9144 tris) restores dense-canopy look, per-tip nominal card area ≈92% of baseline (30×0.04²=0.048 vs 9×0.070²=0.0441); A/B shows matching canopy extent and density, cards slightly larger/chunkier but same tree. C=10 (9528) would be 102% per-tip area but only 2.98x total — short of the plan's ≥3x target; C=9 is the density maximum that keeps ≥3x (3.099x). Verified across 8 seeds in the harness: 9144 all variants.

**In-game confirmation:** build log `deciduous/0..3=9144` (harness bit-exact); mesh validation PASS; no new VUIDs (the VUID-...00754 depthBiasClamp lines are pre-existing — same 4 occurrences in /tmp/treeopt_base_shot.log and /tmp/base_run.log). Other species bit-identical (conifer 307/310/316/352, far 6688, acacia, dead_tree, shrub all unchanged). HUD gpu 7.29→4.94 ms at frame 300.

**A/B artifacts:** new shot `/tmp/treeopt_near_frame300.jpg` (+ crops `/tmp/ab2_treeopt_near_frame300_tree.png` vs `/tmp/ab2_treeopt_baseline_frame300_tree.png`); run log `/tmp/treeopt_near_shot2.log`, build logs `/tmp/treeopt_near_build{,2}.log`.

**Pinned verification command status:** FAILS ONLY on `deciduous_far/0=6688 > 3500` — that gate is task 3's (far LOD cut not yet made, out of task-2 scope); the deciduous-near gate (the task-2 criterion) passes with no FAIL lines for deciduous/0..3. VERIFICATION PASS will only print after task 3 (task 5 runs it last).

**Remaining for later rounds:** task 3 far cut (note: far LOD at 6 cards/tip 0.058, 160 tips/3840 card-tris — its spine block is 1544 per round 1, and the near/far density parity achieved here (9@0.070 vs 6@0.058) is a useful anchor for the far A/B); task 4 RenderDoc re-capture (expect props-pass submitted tris 5.76M → ~3.9M and shadow tree-card load 15.64M → ~10.5M, same method as round 2); task 5 verification + lessons.

## round 4 (curation)

- Task 2 marked done (result recorded; artifacts verified: /tmp/treeopt_near_frame300.jpg + crops, /tmp/treeopt_near_shot2.log). No duplicates merged; no new sub-tasks — the round-3 worker findings only refine task 3 in place, which was updated with the exact baseline breakdown (6688 = 1304 non-spine bands + 1544 spine + 3840 cards) and the density-parity anchor (near 9@0.070 = per-tip area 0.0441 vs far 6@0.058 = 0.0202 → cardSize bump or 4-5 cards/tip needed in the far A/B).
- Next: task 3 — far-LOD cut 6688 → <=3500 tris. It unblocks the pinned verification command's `deciduous_far/0=6688 > 3500` FAIL (the only remaining gate), keeps the plan's 2→3→4→5 ordering, and its optional LOD-switch pull stays inside it (no separate task). Iterate in the task-6 harness (gate <=3500 with margin), then one A/B shot at 100-200 m vs /tmp/treeopt_baseline_frame300.jpg with the exact round-2 env vars.

## round 4 (task 3)

Task 3 done: far-LOD deciduous cut 6688 → 3152 tris (2.12x, ≤3500 with ≥328 margin on any seed) + LOD switch pull 100 → 70 m.

**Final configDeciduousFar (c-utils/treegen/TreeGen.cpp, only function touched):** lev[1].children 7→5, lev[2].continuation true→false, cardCountMin/Max 6→4, cardSize 0.058→0.071, maxTris 7200→4500.

**Structure math (re-derived, matches harness exactly):** tips = (5+1)×(4+1)×3 = 90 (was 160); card tris = 90×4×4 = 1440 (deterministic, min=max); bands = 1712 + [0,4] cap flips: 40 L0 (incl. both caps) + 5×24 L1 children + 32 L1 cont + 24×12 L2 children + (5×24+32) L2 cont + 90×12 L3 = 1712; only L1-child top caps can flip (spawnT lower bound is lev[0].startFrac=0.23, so rad max = 0.63×0.0225×0.839×1.2 = 0.01430 > 0.01333 cap threshold; ≤5×4 tris). Total ∈ [3152, 3172] on ANY seed. Baseline spine block per round 1 = 1544 = 584 band + 960 card tris (40 final-level continuation tips, each also a card tip).

**Why 4 cards (deviation from dispatch starting point "cards 6→3"):** 3 @ 0.082 (exact per-tip area parity: 3×0.082²≈6×0.058²=0.0202) passed the ≤3500 gate at 2792 but the 100-200 m A/B showed the clumps visibly more open than baseline at 250% zoom (3 large cards leave holes the baseline's 6 small cards fill; at 1x it was borderline OK). 4 @ 0.071 (per-tip 4×0.071²=0.0202, same parity) is indistinguishable from baseline at 1x and much closer at zoom for +360 tris. 5 @ parity (0.0635) does not fit 90 tips under 3500 (3512) and 6 @ 0.058 needs children 7→4 (75 tips, 3236) — not needed. Density-parity anchor from round 3 confirmed: cardSize bump alone is not enough for clump SOLIDITY; card count is the solidity lever, cardSize the area lever.

**A/B (exact round-2 env vars, frame 300, same map/camera/seed):** shot /tmp/treeopt_far4_frame300.jpg; horizon band at 1x is indistinguishable from /tmp/treeopt_baseline_frame300.jpg (far trees keep silhouette + density); 250% zoom crops (far4_z1..z3) show clumps slightly more open but silhouette holds. Pixel-diff method: TAA run-to-run noise control = 1.8% px>30 (identical-geometry pair), so changes below that are noise — the far-band geometry change is real and localized to the tree band rows.

**LOD switch pull (optional, taken):** AZGAAR_PROPS_TREE_LOD_SWITCH 100→70 (AzgaarProps.cpp, one constant; RESCATTER_DIST 100 ≥ 70+16=86 ✓). FAR is assigned at dist > SWITCH+MARGIN, so the 86-116 m annulus flips near(9144)→far(3152), 2.9x cheaper per tree. A/B at the shortest affected distance: /tmp/treeopt_far70_frame300.jpg vs /tmp/treeopt_far4_frame300.jpg (same frame; the 86-116 m trees are NEAR in the former, FAR in the latter) — pixel-diff heatmap located ~15 individual flipped trees in the upper band; per-tree zoom A/B (sw70_t1..t4) shows the far LOD holding at 86 m (slightly more open at 2x zoom, negligible at 1x). Not pulled to the plan's 60 m: no A/B evidence at 76-86 m in this fixed view; leave to task-4 profile. Note: the switch also affects conifers (same code path; conifer_far 195 vs near 307-352) — acceptable, it is their designed LOD pair.

**In-game confirmation:** build log `deciduous_far/0=3152` (harness bit-exact); mesh validation PASS; VUID lines identical to baseline (5 pre-existing depthBiasClamp 00754 lines); other species bit-identical (conifer 307/310/316/352, far 195, acacia, dead_tree, shrub; near 9144×4 untouched).

**Pinned verification command: VERIFICATION PASS** (all gates: deciduous ≤9999 @9144×4, deciduous_far ≤3500 @3152, conifer ≤999 @307-352).

**For task 4 (RenderDoc re-capture, same method as round 2):** expect props-pass submitted tris 5.76M → ~2.4M (near 2.64M→0.85M: 9144×93 inst from task 2; far 2.98M→1.41M: 3152×446 inst), with the instance split shifting toward far for the 86-116 m annulus (a few instances move near→far; per-tree total unchanged). Shadow-pass tree-card load 15.64M → ~6.2M (round-2 shadow instance counts: near 279, far 1156 → 9144×279≈2.55M + 3152×1156≈3.64M). Compare PASS SHARES / A/B deltas, never absolute ms. If the re-capture still shows the props pass dominant, the remaining levers are: switch 70→60 (plan approach B, needs a view with 76-86 m trees or acceptance on principle) and shadow-pass approach C.

**Artifacts:** shots /tmp/treeopt_far4_frame300.jpg (chosen far), /tmp/treeopt_far70_frame300.jpg (chosen switch), /tmp/treeopt_far_frame300.jpg (rejected 3-card version); comparisons /tmp/far4_side_1x.png, /tmp/far4_z1..z3.png, /tmp/sw70_mid_1x.png, /tmp/sw70_t1..t4.png, /tmp/sw70_diff_heat.png, /tmp/far_diff_heat.png; run logs /tmp/treeopt_far{,4,70}_shot.log; build logs /tmp/treeopt_far_build{,2}.log, /tmp/treeopt_far70_build.log, /tmp/treeopt_verify_build.log.

## round 5 (curation)

- Task 3 marked done (round-4 worker findings: far cut 6688 -> 3152, switch 100 -> 70 m, VERIFICATION PASS). No duplicates merged; no new sub-tasks — the round-4 expectations (props submitted 5.76M -> ~2.4M, shadow tree cards 15.64M -> ~6.2M) are already folded into task 4's description, and the residual levers (switch 70->60, shadow-pass approach C) remain task-4 decisions, not tasks.
- Next: task 4 — RenderDoc re-capture of the props pass with the identical method (scripts/rdc_props_pass.py: PASS SHARES and A/B deltas, never absolute ms; RasterizedPrimitives/SamplesPassed for draw-level), plus no-new-VUIDs check. It validates the full A+B change (tri load + LOD-split shift), and its profile decides whether the 70->60 switch pull or shadow-pass approach C is warranted before the final task-5 run (re-running the pinned command in case task 4 changes code).

## round 5 (task 4)

Task 4 done: RenderDoc re-capture after the mesh changes + identical-method comparison + no-new-VUIDs check + decisions on the two residual levers.

**Method (identical to round 2):** re-capture with the exact round-2 env vars (`ENGINE_RENDERDOC_CAPTURE=1 ENGINE_RENDERDOC_CAPTURE_FRAMES=300 ENGINE_HIDDEN_WINDOW=1 ENGINE_LOG_TIMEOUT=600000 ./scripts/run.sh renderdoc`, TERM=xterm-256color, same map/camera/seed — draw eids 304-349 and pass layout bit-identical to baseline: 185 draws, props 21, shadow 65). One script change: `scripts/rdc_props_pass.py` LAYOUT gained 2 rows (`deciduous` 9144×4, `deciduous_far` 3152) so species labels resolve in BOTH captures; the comparison method itself (pass shares, A/B deltas, deterministic rpr/sps columns) is untouched. Baseline capture renamed to `/tmp/RenderDoc/c-game_frame300_base.rdc` (capturer writes a fixed filename) — both captures kept for the record (1.2 GB total; /tmp has 26G free; manager may clean up after task 5).

**Capture-run state check:** `deciduous/0..3=9144`, `deciduous_far/0=3152`, all other species bit-identical, validation PASS; re-scatter bit-identical; 35,957 instances, 2 emit tiles. (Cosmetic: "7 resident tiles" vs baseline "8" — tile-streaming timing for zero-instance culled tiles; same 2 emit tiles, same instance total, same draw structure — no GPU effect.)

**Deterministic counters A/B (the reliable numbers):**
| metric | baseline | new | delta |
|---|---|---|---|
| props submitted tris | 5,760,567 | 2,128,271 | −63.1% (2.71x) |
| props rasterized | 2,150,864 | 819,761 | −61.9% (2.62x) |
| props SamplesPassed | 3,305,896 | 2,520,522 | −23.8% |
| shadow submitted tris | 18,314,998 | 8,061,662 | −56.0% |
| shadow tree-card tris | 15,639,304 | 5,385,968 | −65.6% (2.90x) |
| shadow rasterized | 6,352,500 | 2,360,500 | −62.8% (2.69x) |
| frame rasterized | 9,359,730 | 4,036,627 | −56.9% (2.32x) |
| terrain rasterized (control) | 832,484 | 832,484 | identical |
| grass rpr (control) | 60,470 | 60,470 | identical |

**Draw level (rpr/sps):** near deciduous 93→48 inst, submitted 2,635,992→438,912 (−83.3%), rpr 901,370→149,606, sps 2,197,265→1,370,170 (−37.6%); far 446→491 inst, submitted 2,982,848→1,547,632 (−48.1%), rpr 1,188,949→609,610, sps 191,044→233,714 (+22.3%, by design: +12.7% inst at per-tip-area parity); grass 141,652 submitted unchanged. eid 325 (near) still the single biggest fill contributor: 972,090 sps = 38.6% of props-pass samples.

**LOD-switch effect isolated (100→70 from task 3):** 45 trees flipped near→far at this cell (93→48 / 446→491). No-switch mesh-only prediction: color 2,397,911 (round-4 est ~2.4M ✓), shadow 6,194,888 (~6.2M ✓). Actual: 2,128,271 / 5,385,968 → the switch itself saved an extra 269,640 color tris (= 45×(9144−3152) exactly) + 808,920 shadow tris. The 100→70 pull demonstrably paid.

**Pass shares / A/B deltas (EventGPUDuration per-pass event sums — overlapping, per round-2 caveat; shares + deltas only, never absolute ms):** props 45.4%→40.7% (Δ −7.64 ms, −64%), shadow 26.8%→19.1% (Δ −5.02, −72%), terrain 21.2%→25.4% (workload bit-identical per rpr — pure replay jitter), taa 5.8%→12.9% / rmlui 0.6%→1.7% (share rise is denominator shrink + jitter; deltas −11%/+2%), whole-frame sum 26.123→10.393 (Δ −60%). Deterministic frame-rpr delta (−57%) corroborates the event-time deltas — no anomaly.

**Wall clock (HUD at frame 300, densest-props cell):** gpu 7.29 ms (baseline) → 4.94 (near cut, round 3) → **3.87 ms** (far cut + switch, /tmp/treeopt_far70_frame300.jpg) = 23% of the 16.67 ms 60fps budget.

**Run-log VUID/warning check:** NEW capture log: 0 VUID lines, 6 WARN lines — WARN set byte-identical to the baseline capture log (same 6). Screenshot-run logs (round-2/3/4 method): 5 VUID lines in /tmp/base_run.log and 5 in /tmp/treeopt_far70_shot.log, line-for-line identical (all pre-existing VUID-...00754 pDynamicStates depthBiasClamp, timestamps only). No new VUIDs/warnings anywhere.

**Decisions (from this profile):**
1. **LOD-switch pull 70→60: NOT warranted.** 100→70 already measured its win (45 flips here). 70→60 only flips the 76–86 m band, whose population this capture cannot quantify (no per-instance world coords); all 48 remaining near trees are ≤86 m, so no color-fill relief either way; and it would push the 4-card far LOD (A/B-validated at 86 m, round 4) to 76 m — untested territory that needs a new closer A/B, not a profile call.
2. **Shadow-pass approach C: NOT warranted.** Shadow tree cards already cut 2.90x submitted (15.64M→5.39M) and shadow rasterized 2.69x (6.35M→2.36M); shadow pass share is now #3 (19.1%) behind props (40.7%) and terrain (25.4%, jitter). C's remaining headroom (5.39M submitted → ~1.2-1.5M with shadow-only meshes) would shave a fraction of the already-cascade-culled 2.36M rasterized — inside the ±25% replay-jitter band of the metric itself.
3. **Context for the manager:** the frame is no longer tree-dominated (frame rpr −57%, wall gpu 3.87 ms in the densest cell). The remaining dominant GPU work is color-pass alpha-card fill: props SamplesPassed 2.52M = near trees 54% + grass 36% + far 9%. Neither residual lever addresses that; if it ever becomes critical it is approach D (depth sort / alpha-to-coverage / card pre-pass) — renderer-level, out of scope for this task.

**Artifacts:** `/tmp/RenderDoc/c-game_frame300.rdc` (new, 616 MB) + `/tmp/RenderDoc/c-game_frame300_base.rdc` (baseline, 598 MB); run log `/tmp/rdc_new_capture.log`; analyses `/tmp/rdc_props_base.txt`, `/tmp/rdc_props_new.txt` (regenerate: `PYTHONPATH=/home/enes/Apps/renderdoc/build/lib python3 scripts/rdc_props_pass.py <capture>`).

**Remaining steps (for task 5):** re-run the pinned verification command (no engine code changed since task 3's VERIFICATION PASS — this task touched only `scripts/rdc_props_pass.py` LAYOUT data, which is not part of the build; expect PASS unchanged), plus the lessons entry.

## round 6 (curation)

- Task 4 marked done (round-5 findings: identical-method re-capture, full A/B table in notes.md round 5; artifacts verified on disk: /tmp/RenderDoc/c-game_frame300.rdc + c-game_frame300_base.rdc, /tmp/rdc_props_{base,new}.txt, /tmp/rdc_new_capture.log). No duplicates merged; no new sub-tasks — the round-5 decisions rule OUT both residual levers (switch 70->60, shadow-pass C) with evidence, so they are closed, not deferred, and the frame is no longer tree-dominated (frame rpr -57%, wall gpu 3.87 ms at the densest cell).
- Task 5 description updated in place: pinned command expected to PASS (no engine code changed since task 3), lessons entry scoped to the four rule-first lessons (invisible continuation-spine waste; cardCount=solidity vs cardSize=area levers; TAA 1.8% noise floor for A/B; RADV overlapping per-event durations - shares/deltas only), and cleanup of the two ~600 MB /tmp/RenderDoc captures folded in.
- Next: task 5 - final verification run (expect VERIFICATION PASS, all gates already green in round 4) + lessons entry + capture cleanup. Last task; nothing else is outstanding.

## final

Task 5 done — run signed off.

**Verification (the pinned command, verbatim from plan.md):** printed `VERIFICATION PASS`, exit 0. Triage before the run: `TreeGen.cpp` still at the exact round-3/4 finals (near: lev[2].children 3, lev[2].continuation false, 9 cards @ 0.070, maxTris 12000; far: lev[1].children 5, lev[2].continuation false, 4 cards @ 0.071, maxTris 4500), AzgaarProps constants SWITCH 70 / RESCATTER 100 / MARGIN 16, and the task-6 harness reproduces all counts (deciduous 9144×4, far 3152, conifer 307/310/316/352, all other species baseline-identical) → no regression, no re-work. Build step was a no-op (`ninja: no work to do`) — expected, no engine code changed since task 3.

**PASS is not vacuous:** the pinned awk would also print PASS if the mesh-build line never matched, so a second headless run captured the raw line: `deciduous/0..3=9144 deciduous_far/0=3152 conifer/0..3=307..352 ... validation PASS` — every gated token present and in range (≤9999 / ≤3500 / ≤999).

**No new VUIDs/warnings:** the verification run log has 5 VUID lines (all pre-existing VUID-VkGraphicsPipelineCreateInfo-pDynamicStates-00754 depthBiasClamp) and 16 WARN lines; the WARN set is byte-identical to /tmp/base_run.log (10× Undefined-Value-ShaderOutputNotConsumed-DynamicRendering + rmlui shutdown/thread lines) and the VUID lines match the baseline line-for-line (round-5 check). Build log clean.

**Lessons:** 4 rule-first entries appended to `docs/lessons/2026-09-09.md` (final-level continuation spines at sub-pixel radius = invisible tri waste, measured 7296/28344 near; cardCount = canopy-SOLIDITY lever vs cardSize = AREA lever, ≤3 cards/tip fails solidity even at area parity; TAA run-to-run noise floor ≈1.8 % px Δ>30 sets the A/B-change threshold — always run a control pair; RADV per-event GPU durations overlap — compare pass shares/A-B deltas and use RasterizedPrimitives/SamplesPassed, never absolute ms) with matching index lines in `docs/lessons.md` under the 2026-09-09 heading.

**Cleanup:** deleted `/tmp/RenderDoc/c-game_frame300_base.rdc` (598 MB) + `/tmp/RenderDoc/c-game_frame300.rdc` (616 MB); `/tmp/RenderDoc/` is now empty. A/B reference shots (/tmp/treeopt_baseline_frame300.jpg etc.) and the run logs remain in /tmp (small).

**Artifacts this round:** /tmp/treeopt_build.log (build, no-op), /tmp/treeopt_t5_verify_run.log (headless run w/ raw mesh line), /tmp/treeopt_t5_harness_build.log (harness build).

**State:** all 6 tasks done (tasks.json updated); nothing outstanding — ready for manager sign-off. The task itself ("optimize tree rendering, too expensive on the GPU") is closed: props-pass submitted tris −63 %, frame rasterized tris −57 %, shadow tree-card load −66 %, wall GPU 7.29→3.87 ms at the densest-props cell, canopy silhouette A/B-validated at near and 100-200 m far.

# task 7 — remove azgaar map rendering

## round 1 (removal + verification)

**Scope:** full Azgaar `.map`-driven world (blender-terrain.md phase 4, pulled forward of the splat pass). Deleted: `c-game/game/azgaar/` (10 files), `c-game/game/loadingAzgaar/`, `c-engine/ecs/system/heightmap/` (8 files), `renderer/PropsRender.{h,cpp}`, `renderer/diligent/PropsRenderDiligent.{h,cpp}`, `renderer/diligent/HeightmapTerrainDiligent.{h,cpp}`, 7 .hlsl (heightmap_terrain_* ×3, props_* ×4) + pak copies, pak data (`pak_1/azgaar/*.map`, `images/grass-textures/`, `images/leaf-textures/`), `scripts/make_leaf_sprite.py`. Kept: c-utils/treegen (terrain-agnostic, planned reuse), `models/terrain/` + `images/terrain/` (future splat world), blender scripts.

**Edited:** Game.cpp (props bridge/perf/acceptance/look-reg/world-points all out; loadWorld = gltf + spawn + camera only; camera modes land/landtop/props/propsground out, character feetY = spawnPt[1]); MainMenuGui (heightmapTerrainSystem add out); PlayerGui (cell text + worldToMap out); PlayerActionsGui (Azgaar-cell teleport out, origin teleport kept); both html docs; c-engine Player.cpp (heightmap ground-snap + waitingForGround gate out — no collision bodies exist yet, capsule just falls); DiligentRenderer (terrain/props passes + updates + destroys out); ShadowDiligent (terrain/props shadow draws, ShadowPassAttribs cbuffer, ENGINE_SHADOW_NO_TERRAIN/PROPS/CULL out; PBR receiver cascade pick now sources player state from engine::playerGetFootPos); CMakeLists (2 SKIP_PCH entries + 9-copy hlsl rule → 2-copy pak1_hlsl); docs (env.md azgaar sections + ENGINE_CAMERA modes, renderdoc-capture.md pass table); plans status notes (azgaar-terrain.md marked REMOVED, blender-terrain.md phase 4 updated).

**Build:** clean (`scripts/build.sh`, one fix: leftover `groundY` in Player.cpp spawn log). pak_1 re-zipped 99 M, `unzip -l` confirms zero azgaar/grass/leaf/props/heightmap entries. Grep sweep: only intentional comment mentions of "azgaar" remain (Game.cpp removal note, Player.h/GuiManager.cpp).

**Verification (headless, VK radeon ICD, hidden window):**
- `ENGINE_NO_RMLUI=1` world run: ENTER WORLD → world loaded → player spawn (0,0,0) → clean shutdown, 0 VUID lines, 2 pre-existing diligent warnings only.
- `ENGINE_AUTOTEST=enter` (rmlui ON): menu → world, edited GUI docs load (no Cell row / no Azgaar teleport), clean RML shutdown, 0 new warnings. Screenshot `/tmp/menu_shot.jpg` shows the worldless state: sky + Player/Player Actions/Camera readouts, player falling (y −7.16 — expected, no ground).
- RenderDoc frame 100 (character vantage): pass set now `shadow` (38 draws) / `player` (1) / `taa_resolve` (incl. ssao+bloom) — 73 MB capture vs ~600 MB azgaar-era. Character rasterizes in the SHADOW pass (rpr 23111, sps 171/2 cascades) and in the LIT pass at 60 m+ (sps 273/28, visible).

**Pre-existing quirk found (not from this change, left open):** the PBR LIT pass clips the character when it sits within ~20 m of the camera — rpr=0 (VSInvocations 17015, offscreen RT uniform clear) at 2/3.4/20 m, rpr=23111 at 60/200 m; shadow pass unaffected. PBR/camera/placement/TAA paths untouched by the removal. `ENGINE_CAMERA=character` (≈1.4 m vantage) therefore shows an empty frame — unusable for validation until understood. Lesson entry in `docs/lessons/2026-09-10.md`.

**Artifacts:** /tmp/azgaar_rm_build{,2}.log, /tmp/azgaar_rm_run{,2,3,4}.log, /tmp/sweep_{20,60,200}.log, /tmp/menu_shot.jpg, /tmp/azgaar_rm_frame100.jpg (all small). /tmp/RenderDoc/*.rdc deleted after the probes.

**State:** task 7 done — worldless intermediate state as intended by plans/blender-terrain.md (player + sky until the splat pass lands); nothing outstanding.
