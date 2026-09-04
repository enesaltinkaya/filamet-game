# notes

## FINAL STATE — phase 7 (props / vegetation) CLOSED 2026-09-04

Verifier PASS on the pinned Verification command; phase 7 marked ✅ in
plans/azgaar-terrain.md with a full acceptance report. All 16 ledger tasks
done (2 skipped-as-dead: Delaunator, and 12 absorbed into 15).

Acceptance numbers (also in the plan): static props cost 0.005 ms/frame vs
1.5 ms budget (worst dolly 0.014); terrain+props window 56.9 MB peak vs
150 MB; peak 141k instances = 2.8% of old engine's 5M cap; scatter gates
green every run (bit-identical re-scatter, Y-on-surface maxErr 0.0 m);
dolly 100/400 m/s claims/rescatters/evictions logged, worker ~120-135
ms/tile never saturates, no leak while evicting; verification run log has
ZERO WARN/ERROR lines. Acceptance shots: docs/azgaar-terrain/
props-woodland-oblique.jpg + props-ground-view.jpg.

## the three root causes that ate this phase (all fixed + in docs/lessons.md)

1. **camera_at_origin**: this Filament defaults
   `Engine.debug.view.camera_at_origin = true` → every renderable
   transform (and `material.worldPosition`) is CAMERA-RELATIVE. props.mat
   wrote absolute world coords → geometry at ~2x map offset → zero pixels
   while draws emitted. Fix: `worldPos -= getUserWorldFromWorldMatrix()[3].xyz`
   (props.mat). Same lesson: `Builder::boundingBox` is OBJECT-LOCAL and is
   transformed by the renderable transform — passing a world-space box
   culled probes at ~350 km.
2. **BasisU symbol collision**: c-game linked KTX-Software's newer-ABI
   basisu, which won `basist::` symbols over Filament's → every UASTC KTX2
   "transcode failed" → white terrain. Assets were never broken. Fix:
   drop KTX-Software from the link (one-binary-one-BasisU rule). Diagnostic
   trap: ktxreader quiet=true swallowed the real error inside colored
   [INFO] lines.
3. **Filament no-copy uploads**: setBufferAt/setBuffer do NOT copy — probe
   uploads from a local std::vector were use-after-free garbage (same rule
   the phase-5 terrain VBOs learned).

Smaller real bugs fixed en route: CUSTOM0 must be read via `getCustom0()`
(declared variables are NOT auto-filled — parts were black); TBN repack
NaN for exact ±X/±Z normals (fallback branch); "transform set before
build() is discarded" quirk (set transforms AFTER build); ENGINE_LOG_TIMEOUT
unit is MILLISECONDS; 600-frame screenshots are NOT md5-stable (wind
accumulates real-dt) — gates are exit code + log lines + diff magnitude.

## design facts that carry to phase 8 (water/rivers/roads/settlements)

- Only 2 of 25 resident tiles emit prop draws BY DESIGN: per-species XZ
  ground-plane cull caps (grass 440 m, trees 840 m) vs 2048 m tiles → only
  the camera tile + a border-adjacent tile can hold survivors. Pre-cull
  grass is exactly 4×262144/tile (~0.25 tufts/m²); scatter ~150 ms/tile on
  the worker.
- Camera framing: `worldDensestPropsPoint` (Game.cpp) picks a TREE-density-
  weighted point per 2048 m tile (tree mix is the biome-telling signal);
  ENGINE_CAMERA=props = 40° oblique ~100 m out; ENGINE_CAMERA=propsground
  = 7 m eye-height (vantage-matched to the old-engine reference).
- Re-scatter fires per 100 m of camera travel; (tile, readyStamp) dedup on
  the GPU side tolerates a camera-stale scatter via the 100 m excursion
  margin.
- Instrumentation kept: PropsRenderStats/AzgaarPropsStats +
  propsRenderStats()/azgaarPropsStats(), ENGINE_PROPS_PERF=1 periodic line,
  frame-1120 one-shot acceptance log (needs a later-frame run — autotest
  exits after the frame-600 shot), ENGINE_AZGAAR_PROPS_DEBUG=1 claim/evict
  event lines.
- Props vertex = 52 B (pos3/tang-quat4/uv2/color4-custom0-4), matrix-packed
  instanceTransform texture (3 RGBA32F texels/instance: colX/colZ/pos+color
  +wind phase[15]); one InstancedDraw per (tile,species,variant) range;
  instances chunked at 32767.
- Filament 1.x bridge API facts (still true): no NORMAL/TEXCOORD0 slots;
  lit shading forces TANGENTS (props reconstruct yaw basis from
  instanceTransform in VS); materials are prebuilt .filamat via matc
  (-a vulkan -l 2); geometry() indexOffset = IBO-relative; engine teardown
  panics if any MaterialInstance is still alive — destroy GPU state before
  the engine.
- Accepted visual gaps (deferred, not props bugs): flat sky/no horizon fog
  (phase 8 weather), procedural flat-shaded tree art vs textured alpha-cut
  canopies, grass-card alpha reads washed-out in a few tufts.

## assets / repo follow-ups (untracked-but-needed)

- c-engine/data/pak_0_engine/ restored from game-001-cpp (23 MB; repo dir
  was missing entirely after a build wipe) — build.sh regenerates the
  engine pak from it. Commit decision left to the human (git is off-limits
  to agents).
- NEW sourced assets: c-game/data/pak_1/images/button.png (recomposited
  from old-engine ETC1S ktx2: basisu -unpack gives SEPARATE rgb/a PNGs,
  auto-composite has flat alpha — recombine manually) +
  c-engine/data/pak_0_engine/fonts/montserrat{Light,Black}.ttf
  (static instances wght 300/900 from the shipped VF via fontTools
  varLib.instancer; VF default instance is Thin/100). Zero gui warns now.
- Main menu render proof: /tmp/r8-menu.jpg (frame 120).

## round index (details were in the pre-compaction notes)

- r3/task16: recovered the unledgered round; flipped round-2's "merged
  bytes fail" verdict (D19/D19b world-baked+identity SHOW).
- r4/task15: root cause #1 (camera_at_origin); fix in props.mat;
  multi-slot fetch exonerated (D36); props-on ≠ props-off first time.
- r5/task9: root cause #2 (BasisU collision); KTX2 loads, real albedo;
  ISO1600 hack removed; pak_0_engine restored.
- r6/task13: worldDensestPropsPoint reframe; 2-of-25 tiles closed (XZ caps
  by design); judgeable woodland shot.
- r7/task6: propsground vantage match vs old-engine reference; dolly
  follow/evict; stats instrumentation; budgets met.
- r8/task7: probe strip (2360→821 lines, 14 artifacts deleted), fonts/
  button sourced, phase-7 acceptance + ✅ in plans/azgaar-terrain.md,
  lessons entries; final verify exit 0 zero-WARN.
- Verifier verdicts: PASS after each code-changing round (4 PASSes total).
