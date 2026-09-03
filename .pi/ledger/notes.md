# notes

## brainstorm

Core difficulty: The source adapter is tiny (~160 lines) but its settlement-plateau dependency is entangled with phase-7/8 machinery (building instances, props pass, Vulkan) that doesn't exist here yet; the plateau query is backed by lazily-built global mutable state whose construction is what must be extracted, while keeping `heightAt` a pure, thread-safe function of (x,z) callable from the background builder thread.

Key context verified on disk:

- Old source: game-001-cpp `c-game/game/azgaar/AzgaarHeightmapSource.{h,cpp}` — includes
  `azgaar/AzgaarSettlements.h` solely for `azgaarSettlementsPlateauY`.
- Old `AzgaarSettlements.cpp` couples the plateau grid to building-cluster generation +
  `azgaarPropsRegisterGlobal` (props pass, Vulkan) — none of which exist in the new engine.
- New engine: `AzgaarWorld.settlements` (flatY, radiusM, wx/wz) already parsed and immutable
  after `azgaarWorldLoad` (phase 2 done). No `AzgaarSettlements` module exists yet.
- `heightmapTerrainInit` copies only the 2-pointer `HeightmapSource` vtable; the
  `AzgaarHeightmapSource` object must outlive the terrain (old engine keeps it file-static in
  LoadingAzgaar.cpp).
- `MainMenuGui.cpp:27` already adds `heightmapTerrainSystem`; its update anchors the window
  to the camera via `rendererCameraGet` and no-ops when no active terrain is set.
- CMake is GLOB_RECURSE — no build-file edits needed.

Reductions / key lemmas:

1. The plateau grid is buildable at source-init time: settlements data is immutable after
   load, so building the 1024 m bucket grid inside `azgaarHeightmapSourceInit` satisfies the
   old engine's "grid live before any tile generates" ordering without a separate init or the
   concurrent grid-swap safety dance (that was only needed because the old init ran while
   tiles streamed for props).
2. Lifetime rule: adapter + its heap data outlive the terrain (file-static in LoadingAzgaar);
   `heightmapTerrainDestroyData` + `setActive(nullptr)` must run BEFORE
   `loadingAzgaarReleaseWorld()`, else `heightAt` dereferences a destroyed world.
3. `heightAt` is stateless once the grid exists, so "call twice, bit-identical" is a valid
   runtime determinism probe; border-identity/eviction-regen is already covered by the
   engine's self-test (analytic source).
4. Wiring is init + setActive in Game::loadWorld and clear on teardown; no system-add work.

Candidate approaches:

A. Near-verbatim port of source + minimal plateau-only AzgaarSettlements (~120 lines: bucket
   grid + D8 blend + ENGINE_AZGAAR_SETTLE_DISABLED kill switch). Risk: slight module-split
   deviation from the old engine (phase 8 absorbs it later). Effort: low (~1.5 h).
B. Linear-scan plateau inline in heightAt (no new module). Risk: O(N) per texel x 262k
   texels/tile x 25 tiles; settlement count unknown until runtime; adds seconds per
   background tile build and throttles streaming; rework in phase 8. Effort: minimal.
C. Skip the plateau in phase 4. Violates the plan invariant ("settlement plateaus blend
   LAST") and the old header's float-houses warning. Rejected.
D. Full AzgaarSettlements port with building clusters + props upload. Pulls in phase 7/8
   deps that don't exist. Rejected for scope.

Recommended approach: A. Keeps the port near-verbatim, restores the invariant the old code
guarantees (flat ground under future settlements), and the ~120 extra lines get folded into
the full module in phase 8. Must be true for it to work: AzgaarWorld.settlements fully
populated by azgaarWorldLoad (verified: flatY sampled at parse time), and the grid built
before any tile generation (guaranteed by building it in azgaarHeightmapSourceInit, which
runs before heightmapTerrainInit in Game::loadWorld).

Proposed tasks:

1. Port the source + minimal plateau: create `c-game/game/azgaar/AzgaarHeightmapSource.{h,cpp}`
   near-verbatim from game-001-cpp; create minimal `c-game/game/azgaar/AzgaarSettlements.{h,cpp}`
   exposing only `azgaarSettlementsPlateauY(world, wx, wz, naturalY)` (1024 m bucket grid over
   the map AABB, 3x3 bucket query, D8 blend y += (flatY-y)*(1-smoothstep(0.55r, r, d)),
   ENGINE_AZGAAR_SETTLE_DISABLED=1 kill switch), grid built once in azgaarHeightmapSourceInit.
   Verify: ./scripts/build.sh clean.
2. Source lifetime in LoadingAzgaar: file-static AzgaarHeightmapSource in LoadingAzgaar.cpp,
   azgaarHeightmapSourceInit right after azgaarWorldLoad (seed from s_world.mapName), new
   loadingAzgaarGetHeightmapSource() accessor; release path documents that the terrain must be
   destroyed before loadingAzgaarReleaseWorld. Verify: build + load log unchanged.
3. Wire the terrain in Game::loadWorld: static engine::HeightmapTerrain,
   heightmapTerrainInit(&terrain, &src->vtable, HEIGHTMAP_TILE_SIZE_M, HEIGHTMAP_WINDOW_SIZE)
   + heightmapTerrainSetActive(&terrain), replacing the TODO; on menu-return/removed():
   heightmapTerrainDestroyData + setActive(nullptr) BEFORE loadingAzgaarReleaseWorld().
   Leave camera framing for phase 9. Verify: build; run shows tile-generation logs, no crash,
   ESC/menu teardown clean.
4. Acceptance sampling log: one-shot in Game::update when terrain.tilesReady > 0 — at 5 fixed
   points near world center log src->vtable.heightAt called twice (assert bit-identical) and
   heightmapTerrainSample with absolute difference (expect < ~1 m: sample is bilinear over 4 m
   texels of the same surface, not bit-equal at arbitrary (x,z)); if two adjacent tiles are
   READY, assert their shared border column is bit-identical. Verify: brief run, log diffs
   small and determinism holds.

## round 1

Tasks 1 + 4 done. Created in c-game/game/azgaar/:
- AzgaarHeightmapSource.{h,cpp}: near-verbatim port (macro height via
  azgaarWorldSampleHeightSmooth + FNV-1a(map name)-seeded 2-octave value noise
  128/64 m + seabed fade -10m->0 + plateau LAST). Deviation: dropped the old
  include of ecs/system/heightmap/HeightmapTerrain.h (unused). Added
  azgaarSettlementsPlateauInit(world) at the end of azgaarHeightmapSourceInit
  (plan lemma 1: grid live before any tile generates).
- AzgaarSettlements.{h,cpp}: minimal plateau-only module. API:
  azgaarSettlementsPlateauInit(world) (reads ENGINE_AZGAAR_SETTLE_DISABLED
  getenv!=nullptr, builds 1024 m bucket grid, count published last),
  azgaarSettlementsPlateauClear(), azgaarSettlementsPlateauY(world,wx,wz,y)
  (snapshot-at-entry + D8 3x3 bucket query, verbatim from old engine).
  Old header's azgaarSettlementsInit/Clear/Nearest + building clusters NOT
  ported (phase 8).

Build: ./scripts/build.sh clean (GLOB_RECURSE picked both files up, no
build-file edits). Source is NOT wired in yet — no caller of
azgaarHeightmapSourceInit exists (tasks 2/3). No dead code warnings because
the symbols are non-static public API.

Notes for task 2/3 workers:
- g_grid is file-static in AzgaarSettlements.cpp; azgaarSettlementsPlateauClear()
  must be called on world release (or before, same ordering rule as the
  terrain destroyData) — it is public in AzgaarSettlements.h.
- vtable.userData == src, so the AzgaarHeightmapSource object + world must
  outlive every heightAt call (file-static in LoadingAzgaar per plan).

## round 2

Tasks 2 + 3 done (wiring; tasks 1/4 already provided the ported files).

- LoadingAzgaar.cpp: `static AzgaarHeightmapSource s_source = {}` (file-static,
  outlives the terrain). `azgaarHeightmapSourceInit(&s_source, &s_world,
  s_world.mapName)` runs right after `s_loaded = true` in loadingAzgaarLoad
  (old-engine ordering: world load → source init). New accessor
  `loadingAzgaarGetHeightmapSource()` (declared in LoadingAzgaar.h with the
  lifetime contract documented) returns `s_loaded ? &s_source : nullptr`.
- Game.cpp: `static engine::HeightmapTerrain s_terrain = {}` (file-static).
  loadWorld restructured: world-load block now guarded by `if (!worldLoaded)`
  (gltfInit + loadingAzgaarLoad + optional self-test, early-return on load
  failure); AFTER the guard, every entry (re-)inits:
  `heightmapTerrainInit(&s_terrain, &src->vtable, HEIGHTMAP_TILE_SIZE_M,
  HEIGHTMAP_WINDOW_SIZE)` + `heightmapTerrainSetActive(&s_terrain)` — replaces
  the TODO. Init is idempotent (frees prior tiles) so re-entry after
  menu-teardown works.
- Teardown: ESC/menu-return in preUpdate does `heightmapTerrainDestroyData`
  + `setActive(nullptr)` (world + source stay retained; re-entry re-inits).
  GameSystem::removed() does destroyData + setActive(nullptr) +
  `azgaarSettlementsPlateauClear()` BEFORE gltfDestroy +
  loadingAzgaarReleaseWorld() (heightAt dereferences the world; plateau grid
  indexes world->settlements).
- DELIBERATE: plateau grid cleared ONLY at world release, not on menu-return.
  The grid is built inside azgaarHeightmapSourceInit which runs once per world
  load (loadingAzgaarLoad is idempotent), so clearing it on ESC would leave
  re-entered worlds plateau-less. Matches the old engine (its
  azgaarSettlementsClear was in loadingAzgaarReleaseWorld, not on menu-exit).

Verified: ./scripts/build.sh clean. Runtime (ENGINE_AUTOTEST=enter, headless):
map loads, source inits, terrain window @ tile(0,1) resident=25, multiple
tiles READY (~57 ms each, i.e. heightAt + plateau grid execute without
crash), clean shutdown ("game: removed", exit 0). Screenshot at capture
moment is pre-terrain (2 tiles ready, taken a few frames after startup) —
expected timing, not a wiring fault.

For task 5 (acceptance log): the app auto-exits quickly in headless runs
right after startup, so a one-shot "when tilesReady > 0" probe in
Game::update fires within the first second or two; probe points should be
near the window anchor (camera default eye (500,1590,2700), anchor (500,2700),
tiles around origin). ENGINE_CAMERA=topdown/close exist for validation shots.

## round 3

Task 5 done (acceptance log + headless run, PASS).

- Game.cpp: `heightmapAcceptanceLog()` (file-static, called from Game::update via
  `s_acceptanceRan` one-shot flag). Gated per frame until ALL 5 probe tiles are
  READY (found in a `heightmapTerrainSnapshotTiles` view — the thread-safe
  snapshot, not `heightmapTerrainGetTile`) AND at least one adjacent READY pair
  exists; first headless run without the gate fired at ready=1 and correctly
  reported the missing pair as FAIL. Checks:
  1. `src->vtable.heightAt` twice at 5 fixed points, `memcmp` bit-compare
  2. `heightmapTerrainSample` vs heightAt, threshold diff < 1 m
  3. shared border of first adjacent READY pair (east: per-row a[x=TEX-1] vs
     b[x=0]; south: contiguous row memcmp), float bit-equality
- Probe points (all in tile (0,1) = window anchor tile, window covers
  x∈[-4096,6144) z∈[-2048,8192)): (400,2600) (500,2700) (560,2740)
  (340,2820) (620,2580).
- Verified headless (TERM=xterm ENGINE_AUTOTEST=enter
  ENGINE_LOG_TIMEOUT=30000 ./build/c-game/c-game): all 5 probes heightAt
  bit-identical, sample diffs ≤ 0.0001 m (well under 1 m), border tiles
  (0,0)/(0,1) bit-identical, summary "heightmap acceptance: PASS", clean
  shutdown (exit 0). Fired at ready=2 (tile(0,0) + anchor READY) — ~1.5 s in.
- Cross-run determinism: probe heights identical between two runs
  (-33.281174 / -32.832672 / -32.428520 / -36.834591 / -30.797935).
- Build clean via ./scripts/build.sh (added `#include <cstring>` in Game.cpp).
