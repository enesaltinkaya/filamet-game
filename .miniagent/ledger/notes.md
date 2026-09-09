# notes

- Trees: `c-utils/treegen/TreeGen.cpp` (ported from the ez-tree idea, /tmp/ez-tree is a three.js reference app, useful only as a look reference). Wired in `c-game/game/azgaar/AzgaarPropMesh.cpp` (`treeGenToBuilder`, `buildMesh`); per-species config via `treeGenConfig`.
- Leaf strategies: DECIDUOUS=CARDS (flat quads, UV rect {0.02,0.34,0.98,0.99} of `images/leaf-textures/ash.png`, alpha-tested via AZGAAR_PROPS_FLAG_ALPHA_TEST), CONIFER=CONES (addCone hardcodes 4 radial sides -> square pyramids), ACACIA=FAN / SHRUB=BLOBS (3-seg x 2-ring spheres).
- Only DECIDUOUS gets a texture path (ash.png); other species sample the base props texture with white vertex colors (Game.cpp ~line 89).
- The ash.png leaf region currently holds large rounded-square/hex leaflet blobs -> direct source of the "square/plane look". `scripts/make_leaf_sprite.py` is the existing sprite generator (writes /tmp/ash_leaf_sprite.png; ash.png is packed in `c-game/data/pak_1`, repacked by scripts/build.sh).
- Debug aids: `ENGINE_AZGAAR_PROPS_MESH_DUMP=1` dumps per-species OBJs to /tmp/azgaar_props_*.obj; `ENGINE_SCREENSHOT=path` one-shot JPEG; `ENGINE_LOG_TIMEOUT` auto-quit; RenderDoc pass is labeled `props`.
- Constraints: no code comments (AGENTS.md), stay within per-species maxTris budgets, validateMesh unit-height rules (y within [-0.5, 1.25]).

## brainstorm

### Core difficulty

The "blocky/square leaf" look is two compounding causes: every deciduous card quad UV-maps the *entire* ash.png leaf region, which is a 5x3 grid of ~20-30px rounded-square leaflet blobs (opaque fill only ~55%, verified on /tmp/ash_leaf_sprite.png), so each quad visibly shows a square sheet of big blobs; and geometry-wise each tip gets only 1-2 large flat crossed quads (cardCountMin=1/max=2, cardSize 0.055), so even the *outline* of the quad is visible. The fix must make leaves read as small irregular shapes on top of that, without touching the render path, while staying inside per-species maxTris budgets and the shared 128px sprite layout (bark strip rows 0-40 must stay opaque — trunk UVs sample it).

### Reductions / key lemmas

1. **Silhouette follows the alpha, not the quad.** With AZGAAR_PROPS_FLAG_ALPHA_TEST, the visible edge of each card is the texture's alpha edge. If the leaf region becomes a dense scatter of small, irregular, alpha-edged leaflets with transparent gaps between them, *any* quad samples it as a tuft of leaves and the square outline vanishes. The texture is therefore the dominant lever for the deciduous look; geometry tuning is secondary.
2. **Per-card UV decorrelation.** All cards currently share the identical UV rect {0.02,0.34,0.98,0.99} -> every card shows the same blob grid in the same phase, which reads as a repeating stamp. Jittering a per-card sub-window inside the leaf region breaks the pattern. (Only safe once the whole region is uniformly "leafy", including near its borders.)
3. **Triangle-cost model (all verified in TreeGen.cpp):** card = 4 tris; cone(s sides) = 3s tris (2s band + s top cap, and the room() check hardcodes 12); blob sphere = 2*seg*ring tris. Deciduous cards are ~96-130 of a 3300 budget -> ~8-10x headroom. Conifer budget is tight: 5 tips x 1-2 cones x 12 + trunk ~120 ≈ 400/400; moving cones 4->8 sides costs 24 tris each (~+120) and needs explicit arithmetic or a budget bump.
4. **Invariants that must not break:** bark strip rows 0-40 opaque and bark rect (rows 13-29, cols 25-103) opaque (make_leaf_sprite.py asserts both); kLeafUvRect maps to sprite rows ~44-127, so the new leaf design only owns rows ~44-127, cols ~3-126; mesh must satisfy validateMesh unit-height rules (y in [-0.5, 1.25]).
5. **Out of scope for the main complaint but same family:** DECIDUOUS_FAR (mbSphere 6x4 + base props texture, not ash.png) and non-deciduous species sample the base props texture with white vertex colors, not ash.png — their blockiness is geometry-only (4-side cones, 3x2 blobs, 4-side trunks).

### Candidate approaches

A. **Texture-first:** rewrite make_leaf_sprite.py so the leaf region holds ~25-40 small (10-16px) irregular leaf shapes with per-leaflet hue/lightness/size/rotation variation, crisp-but-soft alpha edges and transparent gaps; repack pak; screenshot. *Risk:* at 128px, close-up pixelation of leaf edges and alpha-test banding; needs the leaf shapes to avoid the region border for later sub-UV. Effort: low (1 script run + repack).
B. **Geometry-only:** more, smaller, more-jittered cards per tip; 6-8-side cones; 5x3 blobs. *Risk:* cards are still flat quads sampling the *same* square-filled texture, so small quads still show square texture patches — likely insufficient alone; also conifer budget math is tight. Effort: medium.
C. **Combined (recommended):** new leaf texture (A) + per-tip card count up to ~5-8 with per-card size/tilt/azimuth jitter + per-card UV sub-window jitter (extend emitCards to take a per-card UvRect inside kLeafUvRect) + conifer cones to 6-8 sides with corrected room() cost + blobs to 5seg x 3ring, all within budgets. *Risk:* most moving parts; conifer/shrub budget arithmetic must be done per-species before changing counts; sub-window jitter must keep cards from sampling bark rows. Effort: medium-high, but each piece is small and independently verifiable.
D. **Tuft-sprite cards:** redesign the region as a few pre-composed leaf *tufts* and have each card sample one tuft (per-card sub-UV of a tuft cell). *Risk:* essentially C's sub-UV idea with less per-leaflet variation; harder to make tufts non-repeating at 128px. Effort: medium.

### Recommended approach

C, sequenced as: texture first (A), because lemma 1 says it removes the visible square patches by itself; then card geometry + per-card sub-UV jitter (lemmas 2-3) for silhouette variety; then the conifer/blob geometry fixes as a separate, budget-checked change. It works if: the alpha-test threshold crisply clips the new leaflet edges at 128px, the whole leaf region (including borders) is usable leaf for sub-UV jitter, and per-species tri counts after the geometry changes still fit maxTris (conifer is the binding constraint). If the screenshot after A alone already reads as natural, stop the geometry work at a light card-count bump.

### Proposed tasks

1. **Baseline capture (confirm before changing):** build, run with ENGINE_SCREENSHOT=/tmp/ledger_leaf_before.jpg ENGINE_LOG_TIMEOUT=20 and ENGINE_AZGAAR_PROPS_MESH_DUMP=1; inspect /tmp/azgaar_props_*.obj and record per-species tri counts as the budget baseline. Verify the before-screenshot actually contains close-up deciduous trees (camera position matters for a one-shot capture).
2. **Regenerate the leaf texture:** rewrite scripts/make_leaf_sprite.py — leaf region rows ~44-127: 25-40 small irregular leaflet shapes (10-16px, random ellipsoid/teardrop silhouettes, 15-30 deg size/rotation jitter, per-leaflet green hue/lightness spread, slight edge softness), transparent gaps, nothing within 2px of the region border; keep the opaque bark strip + rect assertions passing; repack via scripts/build.sh; screenshot to /tmp/ledger_leaf_after_tex.jpg and compare side by side.
3. **Deciduous card geometry + per-card UV jitter:** in emitCards (TreeGen.cpp) bump cardCount to ~5-8 per tip, add per-card tilt/azimuth/size jitter, and pass a per-card sub-UV window (random sub-rect, inset inside kLeafUvRect) instead of the full rect; confirm total tris stay under 3300 and validateMesh rules still pass (OBJ dump + build).
4. **Conifer/acacia/shrub blockiness (budget-checked):** cones 4->6-8 sides with the room() cost changed from hardcoded 12 to 3*sides (addCone passes side count); blobs leafSeg 3->5, leafRing 2->3; consider conifer trunk radialSegs 4->6 only if budget allows; verify per-species OBJ tri counts vs maxTris and screenshot.

## final

- **Task completed.** All 5 tasks done; tree leaves no longer read as squares/planes.
- **Baseline (before):** /tmp/ledger_leaf_before.jpg; OBJ tri counts conifer_0 241, deciduous_0 2352, acacia_0 196, shrub_0 115.
- **Sprite (task 2):** rewrote `scripts/make_leaf_sprite.py` — 70 clipped 8-15px irregular leaflets (dual-ellipse, per-leaflet HSV hue 78-158 / sat / val jitter + per-pixel dither, GaussianBlur 0.55) composited into region rows 46-123/cols 5-122 (2px clear of the v=0.34..0.99 leaf-region border so per-card sub-UV jitter never samples the border). Bark strip rows 0-40 and bark rect rows 13-29/cols 25-103 stay opaque (assertions pass). Region fill 0.42. Saved to both /tmp/ash_leaf_sprite.png and c-game/data/pak_1/images/leaf-textures/ash.png; repacked via scripts/build.sh (zipping pak_1 size:69M).
- **Deciduous (task 3):** configDeciduous cardCountMin/Max 1-2 -> 5-7; emitCards now per-card tilt jitter (cfg.cardTilt +- 0.45 rad) and a per-card UV sub-window (45-90% of kLeafUvRect, random offset) so cards no longer share one identical full-rect stamp.
- **Non-deciduous (task 4):** addCone takes a `sides` param; emitCones rolls 6-8 sides per cone and room(3*sides) (was hardcoded 12/4-sided). configAcacia + configShrub leafSeg 3->4, leafRing 2->3 (blob cost 12->24 tris).
- **After:** /tmp/ledger_leaf_after.jpg + 1200x1000 zooms (/tmp/before_zoom.jpg vs /tmp/after_zoom.jpg). Leaves read as small irregular tufts with color/alpha variation; uniform flat quads gone.
- **Post-change tri counts (within budgets):** deciduous 3204/3208/3247/3228 (<=3300), conifer 316-334 (<=400), acacia ~268 (<=400), shrub 175-193 (<=200).
- **Env gotcha (re-discovered):** `ENGINE_LOG_TIMEOUT` is MILLISECONDS (docs/lessons.md) — a value like 25 = 25ms, the engine exits before the frame-100 screenshot fires. Use e.g. 120000. `ENGINE_SCREENSHOT` default start frame is 100 (no env set).
