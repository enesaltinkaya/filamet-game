# Near/far tree LOD (scatter-time selection, generated far meshes)

Give conifer and deciduous trees a low-detail far LOD, selected per
instance at scatter time from the build-time camera. This resurrects the
old engine's near/far LOD double-instances (dropped at port time,
`AzgaarProps.cpp:22-25`) in a form that fits our cull model: we have no
per-frame per-instance GPU cull, so the LOD side is baked per tile at
scatter time and kept fresh by the existing 100 m camera-stale re-scatter.
No PNG imposters (see decision note at the end).

Reference: ez-tree's `Tree.defaultLODLevels` (`/tmp/ez-tree/src/lib/tree.js`):
LOD2 ≈ 20 % of tris via `sectionStride: 6`, `segmentFactor: 0.4`,
`leafStride: 2`, `leafScale: 1.3` (deliberately under-compensated leaf
size). Old engine constants: `PROPS_LOD_SWITCH 100.0f`,
`PROPS_LOD_CULL_MARGIN 16.0f` (hard switch, hysteresis margin; the old
cross-fade band was replaced by the hard switch upstream).

## Why this shape

- **Generated, not exported.** `treegen` is seeded and deterministic, so a
  far LOD is just a second low-detail config per species — no GLB import
  path. ez-tree exports LODs because it bakes static assets; we don't.
- **Scatter-time, not per-frame.** Instances are grouped per
  `(species, variant)` range and culled per range per frame
  (`PropsRenderDiligent.cpp:1480`); there is no per-instance per-frame
  cull. So the LOD choice must be baked into which species an instance
  gets. The camera is already a scatter input (`in.camPos`), and the
  re-scatter fires when the camera moves > `AZGAAR_PROPS_RESCATTER_DIST`
  (100 m, the old `PROPS_LOD_DIST`) and the tile is near the current or
  old camera — so the baked LOD tracks the live camera, and the failure
  mode is conservative (a tree keeps NEAR geometry when stale, never far).
- **Two levels, not three.** One switch point fits the tile-granular
  re-scatter. A mid LOD would need a second species pair (more ids,
  tables, flags) for a band where fog already dominates; revisit only if
  a mid-distance profile shows it pays.

## Scope

- Species: **conifer + deciduous only** — the only two with existing
  `_far` rows, and the two 0.008/m² forest species. Acacia (0.008) is the
  likely phase-2 addition; dead_tree (0.0002), shrub (0.004, 400 m cull,
  0.4-1.0 m tall) are negligible.
- Far meshes: **generated** (replace the hand-built sphere/cone
  placeholders). At 100-300 m a 12 m tree is 40-130 px tall; a 68-tri
  sphere crown reads as a blob, a low-detail skeleton keeps the species
  silhouette.

## Hard constraints (do not break)

- **Unit height / lateral cap.** Far meshes must pass the existing
  unit-height validation (`AzgaarPropMesh.cpp` `validateMesh`: y in
  [-0.5, 1.25]) and clamp to the species' `kCanopyFactor` so the overlap
  gate stays conservative. NOTE: `kSpeciesLateralCap[CONIFER_FAR]` and
  `[DECIDUOUS_FAR]` are currently `0.0f` (= no clamp in
  `treegen::generate`); they must be set to the near values (0.55 / 0.80)
  as part of this change.
- **Scatter determinism.** The LOD pick is a pure function of
  `(tileSeed, tx, tz, in.camPos)` — camera is already an input, so
  eviction + regeneration stays bit-identical. No new RNG salts: the far
  species has 1 variant, so the existing `0xF8` variant pick degenerates
  to 0 (`vc > 1 ? ... : 0`).
- **Overlap gate keyed on the NEAR species.** `placedTooClose` /
  `placedHashInsert` keep the near `sp` (a far instance is the same tree;
  `kMinDist` is 5.0 for both, and the near canopy factor 0.80 ≥ far 0.70
  keeps the gate conservative).
- **Renderer: no structural changes.** Far instances flow through the
  existing `(species, variant)` → range → instanced-draw path, per-range
  frustum cull, per-range sway (`kSpecies` already has sway rows for the
  `_far` species), and the shadow pass.

## Design

### 1. Far mesh configs (`c-utils/treegen/TreeGen.{h,cpp}`)

Two new configs derived from the near ones with ez-tree-LOD2-style
thinning (fewer sections, fewer radial segments, fewer but larger leaf
cards, lower tri cap). Exact starting values — tune against the OBJ
dumps + screenshot:

- `configConiferFar()`: from `configConifer()`:
  - `lev[0]`: sections 4→2, radialSegs 4→3
  - `lev[1]`: sections 3→2, radialSegs 3
  - `coneCountMin/Max` 1/2 → 1/1
  - `maxTris` 400 → 150
  - target: ≤ ~150 tris, still a readable cone stack.
- `configDeciduousFar()`: from `configDeciduous()`:
  - `lev[0]`: sections 12→4, radialSegs 12→4
  - `lev[1]`: sections 8→3, radialSegs 6→4
  - `lev[2]`: sections 6→2, radialSegs 4→3
  - `lev[3]`: sections 4→2, radialSegs 3
  - `cardCountMin/Max` 16/16 → 6/6, `cardSize` 0.055 → 0.08
    (under-compensated for 16→6 cards; ez-tree deliberately uses ~1.25-1.3×
    rather than the area-preserving ~1.63× — "balloon leaves" read worse
    than a slightly sparser canopy)
  - `maxTris` 20000 → 1200
  - target: ≤ ~1200 tris (near is ~4500 measured, so ≈ 25-27 %).
- Keep the `continuation` flags and all angular/twist/gnarliness params
  identical so the far skeleton keeps the near tree's character.
- **Acceptance:** `ENGINE_AZGAAR_PROPS_MESH_DUMP=1` →
  `/tmp/azgaar_props_conifer_far_0.obj` + `/tmp/azgaar_props_deciduous_far_0.obj`:
  silhouette reads as the species (cone stack / rounded crown), trunk
  taper intact, no flipped faces, tops at y ≤ 1, tris within budget.

### 2. Mesh build (`AzgaarPropMesh.cpp`)

- `speciesUsesTreeGen()`: add `AZGAAR_PROP_CONIFER_FAR`,
  `AZGAAR_PROP_DECIDUOUS_FAR` (they currently fall through to the
  hand-built `builders[]` table).
- `treeGenConfig()`: route the two `_far` species to the new configs.
- `kSpeciesLateralCap`: `CONIFER_FAR` 0.0 → 0.55, `DECIDUOUS_FAR`
  0.0 → 0.80 (see hard constraints).
- `kSpeciesVariantCount[_far]` stays 1 (one far silhouette per species is
  enough at distance; fewer draws).
- `buildConiferFar` / `buildDeciduousFar` become dead code — delete them
  (and the `builders[]` entries), or keep as `nullptr` with a comment.
  The far variant seed is automatic: `seed ^ (s * 0x9E3779B9u) ^ (v *
  0xC2B2AE35u)` with the far species id.
- Update the `buildMesh` comment ("*_FAR rows are never scattered") —
  false after this change.
- **Acceptance:** `azgaarPropMesh build` log line shows
  `conifer_far/0=~1.. deciduous_far/0=~1...` within budget, validation
  PASS, build time still < 50 ms.

### 3. Scatter-time LOD pick (`AzgaarProps.cpp`)

- New constants next to `AZGAAR_PROPS_RESCATTER_DIST`:
  - `AZGAAR_PROPS_TREE_LOD_SWITCH 100.0f` (old `PROPS_LOD_SWITCH`)
  - `AZGAAR_PROPS_TREE_LOD_MARGIN 16.0f` (old `PROPS_LOD_CULL_MARGIN`)
- In `scatterTile`, after the overlap gate and before filling `inst`
  (~line 683):
  ```
  u32 spFinal = sp;
  if (sp == AZGAAR_PROP_CONIFER || sp == AZGAAR_PROP_DECIDUOUS) {
      float dx = wx - in.camPos[0];
      float dz = wz - in.camPos[2];
      float m  = AZGAAR_PROPS_TREE_LOD_SWITCH + AZGAAR_PROPS_TREE_LOD_MARGIN;
      if (dx * dx + dz * dz > m * m)
          spFinal = sp == AZGAAR_PROP_CONIFER ? AZGAAR_PROP_CONIFER_FAR
                                              : AZGAAR_PROP_DECIDUOUS_FAR;
  }
  ```
  XZ distance, matching the existing XZ cull-cap convention
  (`AzgaarProps.cpp:818-825`). The margin band [100, 116] m resolves to
  NEAR (conservative: never under-detail a tree that may be approached
  before the next re-scatter).
- Everything after uses `spFinal` for `inst.species` and the variant
  count (`azgaarPropSpeciesVariantCount(spFinal)` → 1 → variant 0);
  `instScale`, `inst.color`, `inst.phase`, `inst.yaw` are unchanged (far
  mesh is unit-height, same metres scale, same biome tint).
- `placedTooClose` / `placedHashInsert` keep the near `sp`.
- `outPerSpecies[spFinal]++` (counts the species that actually renders).
- Update the file-header "dropped relative to the old system" comment:
  near/far LOD double-instances are back (scatter-time baked, not
  per-frame GPU-culled).
- **Acceptance:** determinism spot-check (same map, same camera →
  identical instance arrays across two loads / an evict+regenerate);
  a forest tile scattered from two cameras 100 m apart along an approach
  line shows the boundary trees flipping near→far only once, monotonically.

### 4. Bridge + flags (two one-liners)

- `Game.cpp:90`: bind the leaf texture for the far species too —
  `r.species == AZGAAR_PROP_DECIDUOUS || r.species ==
  AZGAAR_PROP_DECIDUOUS_FAR ? "images/leaf-textures/ash.png" : nullptr`.
- `azgaarPropsSpeciesFlags` (`AzgaarProps.cpp:~1524`): add
  `case AZGAAR_PROP_DECIDUOUS_FAR: return AZGAAR_PROPS_FLAG_ALPHA_TEST |
  AZGAAR_PROPS_FLAG_DOUBLE_SIDED;` (textured cutout cards, same as near).
  `CONIFER_FAR` stays default 0 (untextured cones).
- **Acceptance:** no `props: texture load failed` in game.log; far
  deciduous cards alpha-discard (no dark quads around the canopy in a
  screenshot).

## Verification (end to end)

1. **Mesh:** OBJ dumps (phase-1 acceptance) + build log line.
2. **Visual A/B:** forest-biome map, `ENGINE_SCREENSHOT` frame ~1200
   (props scatter needs ~20 s; camera auto-frames the densest tree tile).
   Before/after: near trees (< 100 m) pixel-identical in character
   (same variants — near path untouched); distant trees keep species
   silhouette (no blobs, no square cards, trunks brown, canopies tinted).
   Second shot with the camera offset ~50 m to check the LOD boundary
   band has no visible pop cluster.
3. **Perf:** dense-forest flyby, compare `statInstancesThisFrame` /
   `statDrawsThisFrame` (props stats exist, `PropsRenderDiligent.cpp:1670`)
   and frame time before/after. Expectation: trees beyond 100 m drop to
   ~25 % of their vertex load; within the 800 m cull disc that is the
   large majority of instances, so total prop vertex throughput should
   fall by roughly 70-80 % on a forest map (measure, don't assume).
4. **Regression:** non-forest map (desert/rock) unchanged; grass,
   acacia, shrub, dead_tree, palm, cactus paths untouched; shadow pass
   still draws props (renderdoc pass `shadow` if in doubt).

## Out of scope (deliberate)

- **PNG imposter billboards.** Baked renders conflict with the
  vertex-colour trunk + per-instance biome tint, the cutout-only props
  pass (imposters need an alpha-blended sorted pass), and the
  height-weighted wind sway. A generated cross-billboard sprite (grass-
  card style) is the fallback if a > 400 m profile ever justifies it.
- **Mid LOD (3 levels).** Needs a second species pair and pays off only
  in a band fog already softens.
- **Acacia / shrub / dead_tree far rows** (new species ids: enum, ~10
  tables, flags, texture routing) — phase 2 if the profile says so.
- **Cross-fade band.** Hard switch, as the old engine settled on; the
  16 m margin + near-biased tiebreak keeps the pop off the approach path.
- **Per-frame per-instance GPU cull** (the old engine's mechanism that
  made true double-instances cheap) — a separate, larger project.
