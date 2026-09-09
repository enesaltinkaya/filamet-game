# notes

## Reference (invariant): ez-tree "Ash Medium"

tree.png = the ez-tree demo main tree: /tmp/ez-tree/src/app/scene.js:46 -> /tmp/ez-tree/src/lib/presets/ash_medium.json (seed 36330, deciduous).

Structure semantics (src/lib/tree.js, RNG src/lib/rng.js):
- Deciduous = 3 side-branch levels + TWIG layer. Each branch's own path is only length/2: a TERMINAL branch continues straight out of every branch tip (same dir, radius = parent tip radius) while side children spawn along the parent. Chain = trunk 0.55H + L1 0.34H + L2 0.12H + twig 0.06H (raw 43.47/27.14/9.51/4.6, H~40).
- Trunk is ~20x its radius (very tall/thin); side child radius = radius[level] * parent's LOCAL radius at attach (level value is a multiplier, not absolute).
- Child placement: stratified along parent from start[level] to 1.0 (jittered slots); radial angle stratified 2pi/count, shuffled slots + jitter. Child direction = parentOrientation * rotY(radial) * rotX(angle[level]).
- Leaves: ONLY on terminal twigs — `count` double-billboard quads (2 crossed quads, W=L=size, UV (0,1)(0,0)(1,0)(1,1), rounded normals = quad normal + vertex dir), stratified along the twig from leaves.start, oriented parentOrientation * rotY(radial) * rotX(55deg), plus 1 extra at the tip. Size = size*(1 +/- 0.72). count 16 => 112 leaf twigs * ~17 leaves ~= 1900 quads; crown = ~112 distinct small clusters.
- Taper per section: radius * (1 - taper*(i/sectionCount)); twist = quaternion about branch Y per section.
- Raw preset values (for reference; ratios below are what matters): branch angle 48/75/60 deg, children 7/4/3, length 43.47/27.14/9.51/4.6, radius 2/0.63/0.76/0.7, sections 12/8/6/4, start 0.23/0.33/0, taper 0.7 all levels, twist 0.09/-0.07/0/0, bark tint 0xCFA82E.

Reference ratios (fraction of total height H — our configs normalize to unit height, so only ratios matter):
- trunk path 0.55H, radius 0.05H
- L1 path ~0.34H, relRadius ~0.63x parent, start 0.23 (of trunk), angle 48 deg
- L2 path ~0.12H, relRadius ~0.76x, start 0.33, angle 75 deg
- twig path ~0.06H, relRadius ~0.70x, angle 60 deg
- leaf quad ~0.067H, 55 deg tilt; crown envelope ~0.35H radius, centered ~0.75-1.0H
- first fork at ~0.12H

Leaf sprite: reference uses /textures/leaves/ash.png which IS A GIT-LFS POINTER in the local repo — no real texture available. Decision: generate a lookalike green leaflet-cluster sprite with PIL (available) and ship it in the game pak.

## Pipeline facts + chosen approach (decisions)

Pipeline constraint: one (species, variant) = one draw range = ONE texture + ONE flag set (Game.cpp:86-94, AzgaarProps.cpp:1516) — trunk and leaf cards cannot have different materials in one mesh. Solution (decided): bake a solid opaque brown patch (>= 10% alpha-opaque padding against filter bleed) into the generated sprite; remap trunk UVs into the patch rect (addBand currently writes angle/t UVs — trivial to clamp), leaf cards sample the leaflet region. DECIDUOUS flags = ALPHA_TEST | DOUBLE_SIDED. Trunk verts white (brown comes from the patch), leaves white too so biome tint multiplies cleanly. Trunk back faces are hidden inside the closed band (negligible overdraw).

Lemmas (decided):
- Canopy appearance = f(cluster count, envelope fill, cluster size), not quads-per-cluster. Downsample 112 twigs x 16 leaves -> ~48 twigs x 5-6 leaves, leaf size x~1.4: clustered silhouette preserved. Leaf tris ~ 48*6*4 ~ 1150.
- Tri budget fine at ~3000/variant: merged mesh built once, drawn instanced per (species, variant); 500 on-screen trees x 3000 tris ~ 1.5M tris is trivial. The current 600-tri cap is the thing to raise, not the engine.
- DECIDUOUS = 4 seeded variants + 1 cheap FAR variant (~<= 400 tris, no full fidelity).
- treegen Vertex already carries uv[2] (TreeGen.h:12; addBand writes them) — no mesh-format change for textured cards.

Rejected/deferred:
- Leaf swap with FLOWER-style radial alpha only: reads as round discs, NOT leaflet clusters — need a real sprite.
- Split trunk/leaf into two draw ranges: cleanest material split but touches Range struct, per-species flag/texture model, scatter variant indexing — fallback only if the brown-patch UV trick fails (trunk UV bleed/halos at distance).

Working pipeline (unchanged facts): species DECIDUOUS = 4 treegen variants, one draw per (species,variant); instance color tints white vertices (col != white keeps baked brown). Props renderer (c-engine/renderer/PropsRender.h) already has ALPHA_TEST (bit0) and FLOWER (bit2); grass cards (crossed quads + alpha-tested PNG, buildGrassCard, c-game/data/pak_1/images/grass-textures/*.png) prove the crossed-card-with-texture path works — a leaf card is the same pattern, sprite at c-game/data/pak_1/images/leaf-textures/ash.png. validateMesh() (AzgaarPropMesh.cpp) warns on unit-height break (y in [-0.5, 1.25]) / index spans; generate() already normalizes to unit height. kSpeciesLateralCap[DECIDUOUS] = 0.80 (AzgaarPropMesh.cpp:465) — check it doesn't squash the crown. Debug: ENGINE_AZGAAR_PROPS_MESH_DUMP -> /tmp/azgaar_props_<species>_<v>.obj; screenshot: ENGINE_SCREENSHOT=/tmp/shot.jpg ./build/c-game/c-game.

## round 1

Task 3 done (branch skeleton). Decisions/deviations vs task string (kept verbatim — do not "fix" back):
- Child counts: task said L1 "4+cont" / L2 "3+cont" but that yields 160 twigs /
  ~3400 branch tris, contradicting its own verification ("~48 twig tips", "branch
  tris <= ~1300"). Chose 1+cont / 2+cont giving EXACTLY 8 L1 / 16 L2 / 48 twigs
  per variant (deterministic, matches plan note "downsampled to ~48 twigs").
- "start 0.23" is a fraction of the trunk (reference raw value); trunk is 0.50 of
  total path ~0.97, so first forks land at ~0.12H, not 0.23H — this matches the
  reference's actual geometry (its lowest fork is also ~0.12H), the task's "0.23H"
  wording was a unit mix-up.
- Per-level gnarliness values (0.008/0.045/0.03/0.01) are NOT the raw reference
  numbers (0.03/0.25/0.2/0.09): our formula divides by sqrt(local radius) without
  the reference's max(1, .) normalization, and our radii (0.002-0.05) are ~40x the
  reference's radius units (0.3-2), so raw values made the trunk gnarl 5x too much.
  These values reproduce the reference's effective per-section rad: trunk 0.03,
  L1 0.25, L2 0.24, twig 0.15.
- angleJitter reduced to 0.1 (was ±30%): at 75+60+60 deg cumulative tilt, ±30%
  jitter let twig chains point past straight-down; reference angles are fixed.

TreeGen.h changes:
- LevelCfg: `relRadius` (>= 0 = child radius is relRadius x parent local radius at attach, i.e. j.radius*(1-taper*t); < 0 = legacy absolute `radius`), per-level `startFrac`/`twist`/`gnarliness` (sentinel -1 = Config global), `continuation` (one tip child: same dir, tip radius, child-level length, ±10% jitter).
- Config: `upBias` (default 0.35 = old hard-coded +Y bias; 0 = reference-style fixed tilt) and `angleJitter` (default 0.3 == old; deciduous 0.1).
- `lev[]` widened 3 -> 4 (levels=4 was an OOB before).

TreeGen.cpp grow(): caps lifted nsec 6->12, sides 8->12; `origins` static array -> std::vector (11-sec trunk OOB'd the old [7]); side children now FIXED counts (removed +0..1 random and cap-7) at stratified slots start + (i+rand)*step in [startFrac, 1); azimuth stays stratified 2pi/n + jitter; attach offset uses local radius; +Y bias only when upBias > 0. twistSign removed: per-level twist applies directly, same sign for all branches of a level (reference has signed per-level twist) — conifer/acacia/dead/shrub spot-checked, fine.

configDeciduous() (all per-level, baseRadius 0.05, baseLength 0.50):
- L0 trunk: 11 sec / 10 sides, taper 0.7, 7 side children, startFrac 0.23, angle 48 deg, twist +0.09, gnar 0.008, continuation on.
- L1: 6 sec / 4 sides, len 0.30, relRadius 0.63, 1 side child, startFrac 0.33, angle 75 deg, twist -0.07, gnar 0.045, continuation on.
- L2: 4 sec / 3 sides, len 0.12, relRadius 0.76, 2 side children, startFrac 0, angle 60 deg, gnar 0.03, continuation on.
- L3 twig: 3 sec / 3 sides, len 0.055, relRadius 0.70, angle 60 deg, gnar 0.01, terminal -> BLOBS stand-in (1-2 spheres r 0.05-0.08, flat 1.0).
- lift 0.6 (upward force, replaces reference force(0,1,0,0.01); without it one twig per variant drooped to ground level), maxTris 2950.

Verification invariants (ENGINE_AZGAAR_PROPS_MESH_DUMP, 4 variants, all validateMesh PASS):
- deciduous tris/variant: 2688 / 2700 / 2763 / 2732 (avg 2720) <= ~3000 budget.
- Branch-only tris ~ 1040 (trunk ~240, L1 ~400, L2 ~400) <= 1300; +864 twig-layer bands ~ 1900 incl twigs; rest blobs.
- 48 twig tips per variant (deterministic); crown fills y 0.3-1.0H, max crown radius 0.40-0.57H; no vertices below y=0.08H outside the trunk; 0.80 lateral cap never reached.
- DECIDUOUS_FAR untouched (baked mesh, 68 tris); conifer/acacia/dead/shrub tri counts identical to before.
- Silhouette renders: /tmp/tree_deciduous.png, /tmp/tree_conifer.png, /tmp/tree_others.png, /tmp/tree_dead.png (matplotlib front+side from the OBJs).

Notes for task 5/6: crown is widest at y~0.55-0.8 (v1/v3 reach R 0.57-0.53); if the
final compare shows a too-wide canopy, levers are L2 startFrac (0 -> ~0.3 to lift
crowns), L1 angle jitter, and leaf card size (task 6). Blob tips are at twig ends
(r ~0.05-0.08H) — leaf cards (task 6) should stratify along the twig, not just tip.

## round 2

Task 6 done (leaf cards). Files: c-utils/treegen/TreeGen.h, TreeGen.cpp, c-game/game/azgaar/AzgaarProps.cpp, c-game/game/azgaar/AzgaarPropMesh.cpp (OBJ dump now emits vt lines + f v/vt faces; vertex index == vt index).

- New LeafStrategy::CARDS. Per terminal twig: 5-6 crossed-card leaves (cardCountMin/Max) STRATIFIED along the twig (t = cardStartFrac + (i + rng)*(1-start)/count, per-segment axis from the branch origins) + 1 extra card exactly at the tip. Each leaf = 2 crossed quads (4 tris), 55 deg tilt (cardTilt), azimuth stratified 2pi/(count+1) slots +/- 0.6 rad jitter, size = cardSize*(1 +/- 0.6) with cardSize 0.068 raw units (~0.07 of the ~0.97H pre-normalized height).
- Card math mirrors ez-tree exactly (tree.js #meshLeaf): local frame x=w (radial), y=axis, z=zref=cross(w,axis); leaf up d = axis*cos55 + zref*sin55 (tilt toward zref, NOT toward w - rotX(55) tilts toward local Z); quad A normal = zref*cos55 - w*sin55, width = w; quad B = rotY(90) => normal = w, width = zref. Rounded normal = normalize(quadNormal + (corner - center)), unweighted like the reference. Card verts white (tintable per props_ps step(0.99, min(PC))).
- UV remap (canonical rects in TreeGen.h, task 7 must match):
  - kBarkUvRect = {0.20, 0.10, 0.80, 0.22} - trunk bands sample this; addBand now distributes u by circumference angle (was u=0 for all) and maps v 0/1 into the rect; cap centers use rect u-center.
  - kLeafUvRect = {0.02, 0.34, 0.98, 0.99} - card quads (u = width coord, v = leaf-up, same layout as reference uv (0,1)(0,0)(1,0)(1,1)).
  - SPRITE LAYOUT for task 7 (overrides its task string): the props base sampler WRAPS, so the brown strip must be >= 10% texel padding around the bark rect on ALL FOUR SIDES including the bottom texture edge. Ship: fully opaque brown strip full-width over v in [0, 0.32] (41px of 128), leaflet cluster in v in [0.32, 1.0]. A 0.25-tall strip is NOT enough (bark rect needs v up to 0.22 + 13px padding).
  - configDeciduous trunkColor is now {1,1,1}: brown comes from the sprite patch. TRANSITIONAL STATE: until task 7 lands the texture, DECIDUOUS is untextured and renders fully biome-tinted (no brown trunk) - not a bug.
- Flags: azgaarPropsSpeciesRenderFlags now returns ALPHA_TEST | DOUBLE_SIDED for AZGAAR_PROP_DECIDUOUS (DECIDUOUS_FAR left at 0). ALPHA_TEST discard is inside the textured branch of props_ps, so it is inert until the texture exists. Shadow pass (props_shadow_ps) binds the same per-variant g_BaseTex (PropsRenderDiligent.cpp:1628), so the cutout shadow works too.
- configDeciduous maxTris 2950 -> 3300.

Verification (ENGINE_AZGAAR_PROPS_MESH_DUMP, 4 variants, validateMesh PASS):
- Tris/variant: 3120/3124/3147/3132 (avg ~3130) - branch ~1892-1899, card tris 1224-1248 (612-624 quads = 48 twigs x 13 quads). BUDGET NOTE FOR TASK 8: this is >3000; the task's "5-6 per twig + 1 tip" is what pushes it; levers if the audit demands <=3000: cardCountMax 6->5 saves ~240 tris, or cardSize down (no tri saving).
- All 612-624 leaf quads per variant lie within 0.12 of a branch (bark-uv) vertex - clustered on twigs, no floating cards.
- Card sizes: mean 0.069-0.081, max 0.110-0.133 (== 0.068*1.6 after normalization). Leaf verts y in [0.145, 1.0]; crown max radius 0.652 (v1) - 0.80 lateral cap NOT reached (was 0.57 without cards; cards add their half-size outward).
- 0 mixed-uv triangles (bark/leaf regions disjoint); DECIDUOUS_FAR (68 tris) and conifer/acacia/dead/shrub counts unchanged (373-397 / 244-256 / 223-226 / 175-193).
- Known droop carries over: v0/v2 have twigs at y~0.13-0.16 (leaf y min 0.145/0.159) - the round-1 lever (L2 startFrac) still applies in task 5.
- Silhouette: /tmp/tree_cards.png (matplotlib front+side of all 4 variants, green=card verts, brown=branch verts) - cards read as dense clusters at twig ends like the reference.
- Full visual check (solid brown trunk, no halos at alphaTest 0.5, no card shimmer) is blocked on task 7's sprite; the UV side is set up to make it pass: bark rect fully inside the opaque strip with >=10% padding all around (incl. bottom, since the sampler wraps).

## Open questions

- (a) brown-patch UV trick must render the trunk as solid, bleed-free brown at alphaTest 0.5 (no edge halos, cards don't shimmer);
- (b) downsampled cluster field must fill the reference crown envelope (~0.35H radius) — check the OBJ silhouette, not just the count;
- (c) after tasks 6/7, final screenshot compare vs /var/home/enes/Downloads/tree.png, tuning levers per "notes for task 5/6" above.

## final

Tasks 5/7/8 done; run signed off.

Task 7 (sprite + wiring):
- scripts/make_leaf_sprite.py (PIL, seed 36330) -> c-game/data/pak_1/images/leaf-textures/ash.png (128x128): rows 0-40 (v [0,0.32]) fully opaque streaked brown; 15 small leaflets (16-24 x 20-30 px, 5x3 grid + jitter, per-leaflet green 80-122/136-168/52-84, blurred 0.9) in v [0.34,0.99]; in-script checks: strip + kBarkUvRect 100% opaque, leaf fill 55%.
- Game.cpp: v.texturePath now falls back to "images/leaf-textures/ash.png" for AZGAAR_PROP_DECIDUOUS (grass branch first, as before).
- IMPORTANT: runtime reads PACKED paks (zip_open on dirs named *pak under build/c-game/data), not c-game/data directly - after touching any data file run `cd c-game && BIN_DIR=$(pwd)/../build/c-game bash ../scripts/data.sh` or the game keeps using the old pak. ash.png is in pak_1.pak now.
- UV convention confirmed: V=0 = image TOP row (no flip; Diligent CreateTextureLoaderFromMemory, same as the grass cards).

Task 5 (visual):
- Trunk was rendering GREEN, not brown: textured-branch shader is `albedo = lerp(1, Tint, tintable) * tex.rgb` with tintable = all vertex-color channels >= 0.99 - white trunk verts (round-2 "brown from sprite" intent) still get the biome tint multiplied in. Fix: configDeciduous trunkColor 0.9 (any value <0.99; PartColor is otherwise ignored in the textured branch) -> trunk now solid brown, no tint, no halos. Leaves keep biome tint (white verts) - intended.
- First sprite pass (7 large leaflets) read as flat oval "fish-plate" leaves vs the reference's fine clumps -> final sprite uses 15 smaller leaflets; cards now read as leaf clusters.
- Angle tightening: lev[1] 75->70 deg (1.22), lev[2] 60->55 deg (0.96), lev[2].startFrac 0->0.25. Effect small: crown_r 0.612->0.599, leaf y min 0.147->0.158. If the crown still reads too wide in later play, the real levers are L1 (lev[0]) 48 deg and L1 length 0.30 - the 48 deg trunk forks are what set the crown width (70/55 tweaks alone move <2%).
- Screenshots: /tmp/tree-verify3.jpg (full), zoom crops /tmp/zoom3_tree.jpg. Trees now read as the reference: thin brown trunk, high 7-fork, leaflet-cluster crown. Known per-variant droop (low twig clumps ~0.3H on some variants) remains, present in the reference too.

Task 8 (budget):
- cardCountMin 5->4, cardCountMax 6->5 (the "~240 tris" estimate in the task string was off; measured save ~96). Result tris/variant 2928/2932/2955/2940 (worst case if all twigs maxed: 3044, within the "~3000" tilde), DECIDUOUS_FAR 68, validateMesh PASS, crown max radius 0.599 < 0.80 cap, 0 mixed-uv tris, cards within 0.125 of a branch.
- Audit script pattern: /tmp/audit_deciduous.py (OBJ v/vt 1:1, f lines tri or quad, classify bark u[0.195,0.805] v[0.095,0.225] vs leaf v>=0.335, mixed-tri count, leaf-to-branch distance, crown radius at y>0.3).

Open (cosmetic, not blocking): crown radius 0.43-0.60 vs reference ~0.35-0.40; trunk tan vs reference greener-gray bark (sprite colors easy to tune in scripts/make_leaf_sprite.py); v0/v2 drooping low clumps.
