# notes

## brainstorm

## Core difficulty

The task is mostly mechanical, but two constraints collide: the generator must be pure/terrain-agnostic AND its output must satisfy the existing pipeline's per-species invariants (unit height, lateral extent vs `kCanopyFactor`, fixed 52 B layout, per-range sway) — with the plan's own clamp-to-`kCanopyFactor` rule being physically impossible for dead_tree (factor 0.12) and shrub (factor 0.0), so the clamp must become an explicit per-species cap table in the adapter instead.

## Reductions / key lemmas

1. **Determinism lemma (mesh):** the generator is a pure function of `(seed, cfg)`. Make each branch's PRNG state a function of its *genealogy* — `branchSeed = hash(parentSeed, level, childIndex)` — so output is independent of queue traversal order; then same map seed → bit-identical variants across reloads for free (mesh is built once per world in `azgaarPropsInit`, which already has `s_mapSeed` at the call site, line 1249).
2. **Determinism lemma (scatter):** `variant = (u32)(propsRand(tileSeed, tx, tz, 0xF8) * vc)` is a pure function of tile inputs; salt 0xF8 is verified unused, and the pick happens after (not instead of) existing rolls, so position/scale/color bits never move. Eviction + regeneration stays bit-identical; only the grouping arrays' internal layout shifts (`base[]` accumulates `vc` per species — an internal detail, never emitted).
3. **Draw-identity lemma:** if the mesh contains exactly `kSpeciesVariantCount[s]` ranges per (s, v) and the scatter only emits `variant < vc[s]`, then `rangeFor(s, v)` is non-null for every emitted instance → the render bridge (Game.cpp → `PropsRenderMeshVariant` rows) needs zero changes. The table must be shared (or duplicated verbatim) between `buildMesh` and `scatterTile`; a mismatch is the only way a draw goes missing or out of bounds.
4. **Unit-height lemma:** generate freely, then normalize *after* generation — translate minY to 0, uniformly scale so maxY = 1 — rather than constraining mid-skeleton. A post-hoc rescale keeps the skeleton math simple and guarantees `validateMesh`'s y ≤ 1.25 check passes with margin.
5. **Clamp-constraint resolution:** per-species lateral cap = `kCanopyFactor` where that is generous (conifer 0.55, deciduous 0.80, acacia 0.70); for dead_tree and shrub use a documented per-species cap table in the adapter (e.g. dead_tree ~0.45, shrub ~0.9) because their factors (0.12/0.0) describe the old thin placeholders and are tuned gate parameters — changing them would alter placement density. Accept that the overlap gate stays *less* conservative for these two species (visual branch interpenetration), not less *deterministic*.
6. **Winding/normal lemma:** the old engine burned two bugs in `mbCone` (clockwise winding → back faces; both-diagonals quad split → sky wedges). The treegen cone band must replicate the *fixed* conventions exactly: CCW-from-outside winding, single `b0->t1` diagonal per quad, one flat weighted normal per segment (radial part ∝ height span, vertical part ∝ taper).

## Candidate approaches

A. **Plan as written: standalone `c-utils/treegen/` module + thin adapter** (PRNG + config + genealogy-seeded skeleton + internal cone-band/blob/cone-stack primitives emitting its own 48 B layout; adapter copies into `AzgaarPropVertex` → `buildRangeInto`). Risk: the module's internal primitives duplicate `mbCone` logic, so the two winding conventions must be kept in sync — mitigated by copying the fixed rules verbatim and OBJ-dump eyeballing. Effort: medium (~3–5 h across 4 subtasks).
B. **Skip the module; generate in `AzgaarPropMesh.cpp` with the existing `MeshBuilder`.** Less code and no layout adapter, but violates the task's explicit terrain-agnostic constraint and entangles the generator with azgaar's 52 B vertex type — un-usable by a future terrain system or GLB exporter. Risk: rework if the module requirement is enforced. Effort: small.
C. **Per-instance geometry at scatter time (fully unique trees).** Violates "variant = draw identity": instanced draws require shared mesh, and per-instance geometry means buffer uploads per instance. Not viable — listed only to rule it out.
D. **Staged rollout inside A:** land conifer + deciduous first (highest visual payoff in forests), verify the full pipeline (determinism, draws, sway, screenshots), then add acacia/dead_tree/shrub configs. Risk: none real; the generator core is shared, only configs and leaf-strategy enums extend. Effort: same total, smaller blast radius per dispatch.

## Recommended approach

A, staged as D. The task spec explicitly requires the terrain-agnostic module, and the adapter is ~30 lines; staging by species means the risky integration points (adapter layout, salt pick, grouping generalization, draw-identity check) get verified on the two most visible species before the other three configs pile on. Must be true: the 52 B copy is byte-exact (verify with `static_assert(sizeof(treegen::Vertex) == 48)` and the existing `validateMesh` PASS), `kSpeciesVariantCount` is identical in build and scatter paths, and the per-species clamp caps are explicit (not silently "kCanopyFactor" for dead_tree/shrub).

## Proposed tasks

1. **treegen module core** — `c-utils/treegen/{TreeGen.h,TreeGen.cpp}`: 32-bit LCG mirroring ez-tree's `rng.js`, `TreeGenConfig`, genealogy-seeded recursive skeleton (taper/twist/gnarliness `1/sqrt(radius)`, upward force pull, child spawning), internal cone-band/sphere-blob/cone-stack primitives with the fixed mbCone winding+normal rules, post-gen normalize (minY→0, maxY→1) + per-species lateral cap. Verify: a throwaway main (or the adapter's dump path) prints AABBs and determinism results — same seed twice → bit-identical vertex/index arrays; different seeds → different; all specs' budgets (≤400/600/400/300/200 tris) met.
2. **Mesh wiring** — `azgaarPropMeshBuild(u32 seed)` (seed from `s_mapSeed` at the call site), `treegen::Mesh` → `AzgaarPropVertex` adapter (paddings `normal.w=0`, `color.w=1`), `kSpeciesVariantCount` table, loop variants in `buildMesh` (existing builders untouched), extend `ENGINE_AZGAAR_PROPS_MESH_DUMP` to per-variant OBJs + `validateMesh` per-variant tri-count/wall-time log. Verify: build, `ENGINE_AZGAAR_PROPS_MESH_DUMP=1` run → `validateMesh ... PASS`, ~17 new ranges, build < 50 ms, eyeball 17 OBJs (distinct silhouettes, upright, no flipped faces, tops y≤1, lateral within cap).
3. **Scatter variant pick** — `inst.variant` from salt 0xF8 in `scatterTile`, replace the hardcoded `inst.variant = 0`, generalize the grass-only `grassVc` exceptions in the `base[]`/`totalV`/`pairs` loops to `kSpeciesVariantCount`. Verify: code-level determinism check (pick is pure in tile inputs; all other salts untouched) + a forest run with `ENGINE_SCREENSHOT` showing mixed silhouettes, brown trunks, tinted canopies.
4. **Render pass audit (expect zero changes)** — confirm Game.cpp rows exist for every (s, v) range, log the props instanced-draw count in a forest tile (≈ Σ non-empty (species, variant) ranges per tile, +13 over baseline), confirm height-weighted sway reads correctly on tall trees (range AABBs now come from real geometry, not placeholders).

## final

Findings:

- Implemented plans/proc-veg-variants.md end-to-end; all 5 tasks.json entries marked done.
- `c-utils/treegen/{TreeGen.h,TreeGen.cpp}` (auto-globbed): mulberry32 PRNG; each branch's seed is a pure function of its genealogy (`mixSeed(parentSeed, child)`) so output is traversal-order independent; cone-band/sphere/cone-stack primitives replicate the fixed mbCone conventions (frame with u x v = -axis, single b0->t1 diagonal per quad, one weighted flat normal per segment = radial*len + axis*(r0-r1)); per-tip BLOBS/CONES/FAN emitters; a `maxTris` budget guard; post-gen normalize (minY->0, maxY->1) then optional xz clamp to the caller-supplied lateral cap.
- Gotcha hit: `vOrthonormal(+Y)` with a parallel reference vector degenerated to (0,1,0) and, on one seed, collapsed an entire dead tree onto the Y axis (ext=0). Fixed the reference choice (|n.y|>=0.9 -> use Z) plus a per-section re-orthonormalization guard.
- Gotcha hit: the `treeGenConfig` switch in AzgaarPropMesh.cpp initially omitted the SHRUB case, so all 4 shrub variants silently built from the default Config (32-tri trunk, no leaves) while every other species looked fine. In-game tri-count sanity checks per species caught it.
- `kSpeciesLateralCap` is an explicit per-species table in the adapter: conifer 0.55 / deciduous 0.80 / acacia 0.70 / dead_tree 0.45 / shrub 0.90. dead_tree (old factor 0.12) and shrub (0.0) get documented caps because their kCanopyFactor values are overlap-gate params, not mesh extents — the overlap gate itself is untouched, so those two species gate slightly less conservatively (visual interpenetration, not determinism).
- `utils::elapsedEnd` already returns MILLISECONDS (nanos diff / 1e6) — an early log multiplied by 1000 and showed a bogus 1.4 s "build"; real buildMesh time is ~1.4 ms (3.7 ms with the OBJ dump env on).
- Determinism verified three ways: (a) standalone 17-variant bit-exact repeat + 200-seed sweep; (b) two full game runs -> all 34 OBJ dumps byte-identical per file; (c) in-game one-shot acceptance re-scatter: 59615 instances bit-identical (variant pick is a pure function of tile inputs, salt 0xF8 unused before).
- Render path: zero changes. Game.cpp builds a `PropsRenderMeshVariant` row per mesh range (now 31) and `applyTile`/`findVariant` are generic; a missing row would only skip a range (scatter can only emit variants that exist because both sides use the shared `kSpeciesVariantCount` table; grass entry is the runtime texture count set at build time).
- Verification artifacts: /tmp/azgaar_props_*.obj (34 dumps), /tmp/props_forest.jpg (ENGINE_CAMERA=props over the densest-tile forest: distinct trunks, brown trunk vertex colour, biome-tinted canopies), /tmp/propsrun{1..6}.log. `ENGINE_PROPS_PERF` logs the props draw count (12 draws/frame over the props camera).
- Conifer config uses a 2-level skeleton (plan said 3) — the <=400-tri budget leaves no room for a third level plus per-tip cone stacks; silhouette variety still comes from per-tip 3-5 cone stacks over a randomized skeleton.
- Trunk gnarl/twist were toned down once (0.05-0.12 -> 0.035-0.09) after the first screenshot read as over-twisted boxes; structure (tri counts) is unaffected since it's RNG-sequence driven.

Remaining steps:

- None. Optional polish if the manager wants it: higher-res leaf spheres (leafSeg 4) would cost ~2x blob tris and needs a budget rebalance; palm/cactus/rock/flower variety is out of scope per the plan.
