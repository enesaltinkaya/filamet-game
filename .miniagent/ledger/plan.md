# Plan — make the ported deciduous tree's BRANCH STRUCTURE match ez-tree "ash medium"

## Task

The leaves were already fixed in the previous round (dense cluster cards, no square
silhouettes). The user now says the **branch structure** (trunk + limb skeleton) does not
look like ez-tree's default *ash medium* (reference image
`/var/home/enes/Downloads/tree.png`, geometry ground truth `/var/home/enes/Downloads/tree_LOD0.glb`).
Colors are fine — do not touch materials/texture.

## Diagnosis

`configDeciduous()` in `c-utils/treegen/TreeGen.cpp` was a hand-tuned port, not a faithful
conversion of the ez-tree `ash_medium` preset (`/tmp/ez-tree/src/lib/presets/ash_medium.json`).
ez-tree works in preset world units (trunk radius 2.0 at total height ~66.4, twist in
radians/section, gnarliness scaled by `max(1, 1/sqrt(r_meters))`); `treegen::generate`
normalizes to unit height and applies gnarliness as `g / max(0.15, sqrt(r_unit))` per section.
Transcribing the raw preset numbers therefore produced:

- a stocky trunk (`baseRadius` 0.046 vs the correct 2.0/66.4 ≈ 0.030 of unit height),
- helical twist (ez twist is per-section: 0.09×12 sections = 1.08 total on the trunk),
- over-gnarled mid/upper branches (2-3× too strong in unit space),
- crown reading as a "candelabra with pom-poms" instead of the reference's slender oval.

The leaf cards also had the wrong size: the reference's measured card size is
2.67/82 ≈ 0.033 of tree height (measured from the GLB leaf-mesh triangles), but the port
used 0.055, so clumps were chunky and sparse rather than the reference's fine tangle.

## Fix (all in `configDeciduous()`, no generator/texture/renderer changes)

- `baseRadius` 0.046 → **0.030** (trunk radius / total height).
- gnarliness converted to unit space, per level: 0.005 / 0.035 / 0.028 / 0.010
  (was 0.008 / 0.053 / 0.063 / 0.042 — too gnarled on the thin twigs).
- `cardSize` 0.055 → **0.040**, `cardCountMin/Max` 16 → **30** (denser, smaller clumps,
  matches the reference's fine leaf tangle).
- `maxTris` 20000 → **30000** (the higher card count silently re-clipped the crown via
  `Gen::room()`; fingerprint = build log showing the variant at exactly `maxTris`).

## Verification

1. `ENGINE_AZGAAR_PROPS_MESH_DUMP=1` run → per-variant OBJs to `/tmp`.
2. Headless Blender A/B: import the reference GLB (already Z-up after
   `import_scene.gltf`), scale both to unit height, render side-by-side. Compare the branch
   skeleton silhouette (trunk length, limb angles, crown oval) — see `/tmp/tree_cmp.png`
   and `/tmp/tree_branch_cmp.png`.
3. Pinned in-game screenshot (frame 1200) → compare against
   `/var/home/enes/Downloads/tree.png`. `azgaarPropMesh build` must show validation PASS
   with sane deciduous tri counts (~28k, under the 30000 cap) and the other species
   (conifer/acacia/shrub/dead_tree) unchanged.
