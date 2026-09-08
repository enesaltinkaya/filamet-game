# Procedural vegetation variants (ez-tree approach)

Give each tree/shrub species a handful of procedurally generated mesh
**variants** instead of one hand-built placeholder, so forests show real
per-tree silhouette diversity. Approach ported from the ez-tree library
(`/tmp/ez-tree/src/lib/tree.js`): seeded recursive branch skeletons with
per-section random perturbation, taper, twist and tip leaf-blobs — as a
standalone `c-utils/treegen/` module, not via GLB import.

Reference: ez-tree's core loop (`#growBranch` / `generateChildBranches`):
each branch is a chain of sections; per section the branch tapers, twists,
gets a random orientation perturbation scaled by `1/sqrt(radius)`
(gnarliness), bends slightly toward an upward growth force, then spawns
child branches at randomized heights/radial offsets. Leaves are recorded
at tips/end sections. Same seed → same tree, bit-stable.

Module placement: `c-utils/treegen/` — the generator is a terrain-agnostic
pure-function library (its own vertex layout, seeded config in, mesh out),
so a different terrain/props system (or a GLB export tool) can reuse it
without azgaar. `c-game/game/azgaar/AzgaarPropMesh.cpp` only adapts its
output into the props pipeline's 52 B vertex layout.

## Hard constraints (from the existing pipeline — do not break)

- **Unit height.** Every variant base at y=0, top y=1. Instance `scale`
  is metres, and the overlap gate uses `kCanopyFactor[species] * scale`.
  The generator must clamp each variant's lateral extent to the species'
  `kCanopyFactor` (canopy radius ≤ factor at unit height) so the scatter's
  overlap gate stays conservative without per-variant tables.
- **Variant = draw identity.** `(species, variant)` is one
  `AzgaarPropRange`/`AzgaarPropMeshRange` = one instanced draw. The render
  bridge (Game.cpp) already builds `PropsRenderMeshVariant` rows from every
  mesh range, so the renderer needs **no changes** — as long as the scatter
  only emits variants that exist in the mesh.
- **Vertex layout fixed** (52 B, 13 floats; `color.w` in use, `normal.w` = 0
  rotated as a basis). No per-vertex wind factor in this plan — sway stays
  the existing height-weighted per-variant model (`swayFactor` per range).
- **Scatter determinism.** Tile instances are a pure function of
  (mapSeed, tile, camera). Variant pick must come from the existing
  `propsRand(tileSeed, tx, tz, salt)` stream with a **new unused salt**.
  Eviction + regeneration stays bit-identical.
- **Mesh determinism.** The generator is a pure float pass over
  `(mapSeed, species, variant)`; same map → same variants, bit-stable
  across rescatters/reloads (mesh is built once per world in
  `azgaarPropsInit`).

## Geometry budgets (target, per variant)

| species   | variants | skeleton                                        | leaves                                                                                               | tris/variant |
| --------- | -------- | ----------------------------------------------- | ---------------------------------------------------------------------------------------------------- | ------------ |
| conifer   | 4        | 3 levels, 3–5 children, 4 sect                  | flattened blob per tip, OR 3-cone stack (classic) randomized (cone count 3–5, radii/angles jittered) | ≤ 400        |
| deciduous | 4        | 3 levels, 3–5 children, 4 sect                  | 3–5 sphere blobs at tips (0.2–0.45 r, squashed)                                                      | ≤ 600        |
| acacia    | 3        | 2–3 levels, 3–4 children, wide angles           | small blobs, flat-wide canopy (y-clamped ≤ 0.9)                                                      | ≤ 400        |
| dead_tree | 2        | 3 levels, 4–6 children, high angles, taper → ~0 | none                                                                                                 | ≤ 300        |
| shrub     | 4        | 1–2 levels, 4–7 children, low                   | dense small blobs (0.1–0.2 r) covering the crown                                                     | ≤ 200        |

Total ≈ 17 variants ≈ +13 instanced draws over today's baseline, ~6 k tris
of unique geometry. Palm/cactus/rock/flower/reed stay single-variant
placeholders in this plan.

## Phases

### 1. Branch-skeleton generator core (terrain-independent module)

New module `c-utils/treegen/` (`TreeGen.h` + `TreeGen.cpp`, namespace
`treegen`) — auto-picked up by c-utils' `GLOB_RECURSE`, no CMake edits. The
generator is pure math: no azgaar/world/renderer includes, only `Utils.h`
(u32). It must survive a terrain-system swap unchanged, so it emits its own
neutral output — NOT `AzgaarPropVertex` and NOT a `MeshBuilder`:

```cpp
namespace treegen {
// 12 floats / 48 B: pos3 @ 0, normal3 @ 12, uv2 @ 24, color4 @ 32.
struct Vertex { float pos[3]; float nrm[3]; float uv[2]; float col[4]; };
struct Mesh { std::vector<Vertex> verts; std::vector<u32> idx; float aabbMin[3], aabbMax[3]; };
// trunkColor baked into trunk verts, leaf verts white (tintable by caller).
Mesh generate(const Config& cfg, u32 seed);
}
```

`MeshBuilder` stays in `AzgaarPropMesh.cpp`; a ~30-line adapter there copies
`treegen::Mesh` rows into the 52 B `AzgaarPropVertex` layout (inserting the
`normal.w`/`color.w` paddings) and feeds `buildRangeInto`. If a future
terrain/props system wants the trees, it uses `treegen::generate` directly.

- Small seeded PRNG (mulberry32-style, 32-bit; mirror of ez-tree's `rng.js`
  shape — pure, no platform floats). Seed = `mapSeed ^ (species * C) ^ v`.
- `TreeGenConfig` struct: per-level `levels, children[2..3], angleSpread,
length[2..3], radius[2..3], taper, twist, gnarliness, sections[2..3],
radialSegs[2..3], startFrac`, trunk color (baked into branch verts),
  and a leaf strategy enum (`NONE / BLOBS / CONES / FAN` for the five
  species above).
- All geometry primitives live INSIDE the module (small internal
  triangle-soup builder writing `treegen::Vertex`): cone band between two
  section rings at arbitrary origin/orientation, sphere blob, cone stack.
  Winding/normal rules copied from the fixed `mbCone` in
  `AzgaarPropMesh.cpp` (CCW-from-outside winding, per-segment weighted
  normals — the old engine burned a bug on both there). No
  `MeshBuilder` dependency: azgaar gets zero new builder helpers.
- Skeleton loop: branch queue; per section: advance origin along
  orientation, radius × (1 − taper·t), random perturb
  `gnarliness / max(1, sqrt(radius))`, +twist about local Y, +small pull
  toward world +Y (ez-tree's `force`, strength ∝ 1/radius, so trunks stay
  upright and twigs splay). At a branch's last section: if level < max →
  enqueue children (count from config, start height randomized in
  [startFrac·len, len], radial offset randomized, angle = base ± jitter);
  if level == max → record leaf placement.
- Leaf emitters (module-internal): sphere blobs at tips with per-blob
  radius jitter + slight position jitter; CONES = cone stacks for the
  classic conifer look; FAN = blobs squashed to y≤0.9 for acacia.
- Branch verts carry the config's trunk color, leaf verts stay white
  (tintable by the caller's per-instance tint).
- **Acceptance:** `ENGINE_AZGAAR_PROPS_MESH_DUMP` (existing env) extended to
  dump per `(species, variant)` OBJ to `/tmp/azgaar_props_<species>_<v>.obj`;
  eyeball 17 dumps — distinct silhouettes, upright trunks, no flipped
  faces (back-face cull test in-game), tops at y≤1, lateral extent within
  the species' `kCanopyFactor`.
  The dump is driven by a tiny `treegen`-side test main or the adapter's
  OBJ writer in AzgaarPropMesh.cpp (existing `propsDumpBuilder` pattern) —
  the module itself stays std-only.

### 2. Wire variants into the merged mesh

(azgaar-side adapter: `treegen::generate` → `AzgaarPropVertex` rows →
`buildRangeInto`.)

- `azgaarPropMeshBuild(void)` → `azgaarPropMeshBuild(u32 seed)` (call site:
  `azgaarPropsInit`, which has the map seed).
- `kSpeciesVariantCount[AZGAAR_PROP_COUNT]` table (the 5 species above,
  1 everywhere else). `buildMesh` loops `v < kSpeciesVariantCount[s]` and
  calls the tree generator for those species, the existing builders for
  the rest; each goes through `buildRangeInto(..., s, v, ...)` as grass
  already does.
- `validateMesh` (existing) already cross-checks ranges — extend its log
  to per-variant tri counts + build wall-time (ms) in the init log line.
- **Acceptance:** `validateMesh ... PASS` with 5 species × N variants;
  init log shows mesh build < 50 ms and total vertex count.

### 3. Scatter: variant pick + grouping

In `AzgaarProps.cpp` `scatterTile`:

- New salt (e.g. `0xF8`) variant pick:
  `inst.variant = (vc > 1) ? (u32)(propsRand(tileSeed, tx, tz, 0xF8) * vc) : 0;`
  replacing the hardcoded `inst.variant = 0` for non-grass.
- Generalize the hardcoded grass exception in the grouping/count code
  (`totalV`, `counts`, `pairs` loops) to use `kSpeciesVariantCount` for all
  species instead of `(s == GRASS_TUFT) ? grassVc : 1`.
- No change to the overlap gate: generator already clamps to
  `kCanopyFactor`.
- **Acceptance:** determinism spot-check — load a forest-biome map, walk
  the same tile twice (evict + regenerate), diff the instance arrays
  (species, variant, pos, scale bit-exact); screenshot with
  `ENGINE_SCREENSHOT` over a forest: visible per-tree silhouette diversity,
  trunks stay brown (vertex colour), canopies take the biome tint.

### 4. Render pass check (expect zero changes)

- Game.cpp already iterates all mesh ranges → `PropsRenderMeshVariant` rows
  (bounds per range, `swayFactor` per species), and the scatter bridge
  pushes per-range instance sub-arrays. Verify only: (a) no variant is
  drawn whose row is missing, (b) draw count ≈ variants × ranges in a
  forest tile (log the props draw count once), (c) wind sway still reads
  correct on tall trees (height-weighted from the per-range AABB).

## Out of scope (deliberate)

- ez-tree's trellis growth guide, texture maps (we tint vertex-colour
  geometry), per-branch LOD cross-fade (range-level cull + distance caps
  already exist), per-vertex windFactor (needs the `normal.w` slot — a
  follow-up that also requires a VS change).
- Palm/cactus/rock/flower variety, and using the skeleton for palms
  (curved trunk + fronds is a different generator — possible follow-up).
- Bumping `kSpeciesDensity`/`kMinDist` to make forests denser.
