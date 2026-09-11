## user

can you see azgaar map related code in the git?

## assistant



Yes — it's in git history, but removed from the working tree.

**Timeline:**
- Present from `f337aca` (init) through `1b00ed3` (2026-09-09 16:53) — that's the last commit with the full code.
- Deleted in `e2491be` (2026-09-10 02:38), the big "azgaar removal" commit that also ripped out the `c-engine` heightmap/props pipeline (`HeightmapTerrain*`, `PropsRender*`, `HeightmapTerrainDiligent*`, their HLSL shaders).

**What was removed** (all recoverable from any commit ≤ `1b00ed3`):
- `c-game/game/azgaar/` — `AzgaarWorld`, `AzgaarProps`, `AzgaarSettlements`, `AzgaarHeightmapSource`, `AzgaarPropMesh`
- `c-game/game/loadingAzgaar/` — `LoadingAzgaar`
- `c-game/data/pak_1/azgaar/Chilerel 2026-08-11-15-35.map` (the map data)
- `c-engine/ecs/system/heightmap/`, `c-engine/renderer/PropsRender*`, `.../diligent/HeightmapTerrainDiligent*`, props/terrain HSL shaders
- `c-engine/data/pak_0_engine/shaders/pass/azgaar_{props,river,water,weather}/`
- grass texture PNGs

**Still in HEAD:** `plans/azgaar-terrain.md`, `docs/azgaar-terrain/` reference screenshots, and azgaar mentions in `docs/lessons*` and `.pi/ledger/task.md`.

To restore: `git checkout 1b00ed3 -- c-game/game/azgaar c-game/game/loadingAzgaar` (plus the c-engine heightmap/props paths and shaders if you want the render side). Note the lessons doc flags an unresolved clip-transform issue found during the removal verification (PBR lit pass clipping the character near the camera), so worth re-reading `docs/lessons.md` before resurrecting it.

