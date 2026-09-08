# Plan

## Strategy
Implement `plans/proc-veg-variants.md`: give 5 tree/shrub species procedural
mesh variants instead of single placeholders, for per-tree silhouette
diversity. The generator becomes a new terrain-agnostic module
`c-utils/treegen/` (auto-globbed, no CMake edits): seeded mulberry32-style
PRNG, ez-tree-style recursive branch skeleton (per-section taper/twist/gnarl
perturbation scaled by 1/sqrt(radius), upward force pull), with internal
geometry primitives (cone band, sphere blob, cone stack) emitting its own
neutral 48 B vertex layout — no azgaar/renderer dependencies. `AzgaarPropMesh.cpp`
gets a thin adapter copying `treegen::Mesh` into the 52 B `AzgaarPropVertex`
layout and feeds the existing `buildRangeInto`; the generator clamps each
variant to unit height (top y=1) and lateral extent within the species'
`kCanopyFactor` so the scatter overlap gate and per-instance scale stay
unchanged. `AzgaarProps.cpp` scatter picks a variant from a new
`propsRand` salt (0xF8), and the grass-specific variant grouping/counting
is generalized to a `kSpeciesVariantCount` table; eviction stays
bit-identical because the pick is a pure function of (tileSeed, tx, tz).
The render bridge (Game.cpp) needs zero changes — verification confirms
draw counts and sway only.

## Approach
- Phase 1: `c-utils/treegen/{TreeGen.h,TreeGen.cpp}` per the plan's API
  (Config with per-level children/angle/length/radius/taper/twist/gnarliness,
  leaf strategy enum NONE/BLOBS/CONES/FAN, trunk color baked, leaf verts
  white); per-species configs for conifer (4), deciduous (4), acacia (3),
  dead_tree (2), shrub (4).
- Phase 2: `azgaarPropMeshBuild(u32 seed)`, `kSpeciesVariantCount` loop in
  `buildMesh`, existing builders untouched for the rest; extend
  `validateMesh` log with per-variant tri counts + build wall-time.
- Phase 3: variant pick in `scatterTile` + generalize the grass exception in
  grouping/counting loops.
- Phase 4: extend `ENGINE_AZGAAR_PROPS_MESH_DUMP` to per-(species,variant)
  OBJs under /tmp for eyeballing; determinism spot-check (evict tile,
  regenerate, diff instance arrays bit-exact) + `ENGINE_SCREENSHOT` over a
  forest biome; confirm no render-side changes.
- Follow `docs/lessons.md` before touching anything vertex/buffer-related;
  no code comments; no git.

Verification: bash scripts/build.sh
