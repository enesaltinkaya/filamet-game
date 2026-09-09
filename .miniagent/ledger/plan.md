# Plan: tree leaves look blocky / like squares

Root causes identified in code, all fixable without touching the render path:
(1) the leaf sprite region of `c-game/data/pak_1/images/leaf-textures/ash.png` is filled with large rounded-square/hexagonal leaflet blobs — deciduous leaf cards sample this whole region, so every quad shows a square-ish patch; (2) deciduous `CARDS` geometry emits only 1–2 large flat quads per branch tip, so the quad outline and full-rect UV map are visible; (3) non-deciduous species are equally blocky: `addCone` hardcodes 4 radial sides (conifer cones render as square pyramids) and BLOBS/FAN leaves are 3-seg x 2-ring spheres (angular ellipsoids).

Approach: first capture a baseline screenshot and OBJ dumps (`ENGINE_AZGAAR_PROPS_MESH_DUMP=1`) to confirm; then regenerate the ash leaf sprite with small, irregular, overlapping leaflets and per-leaflet color/alpha variation (extend `scripts/make_leaf_sprite.py`, repack the pak via `scripts/build.sh`); then tune the geometry — more, smaller, more-jittered cards per tip in `configDeciduous`/`emitCards`, 6–8-sided cones, and higher-segment blobs for conifer/acacia/shrub, all within the existing per-species `maxTris` budgets; finally re-screenshot and compare side by side, using RenderDoc props-pass capture if the screenshot is inconclusive.

Verification: scripts/build.sh && TERM=xterm ENGINE_SCREENSHOT=/tmp/ledger_leaf.jpg ENGINE_LOG_TIMEOUT=20 ./build/c-game/c-game
