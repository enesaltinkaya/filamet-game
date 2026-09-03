# Azgaar .map terrain — port plan

Replace the removed Blender-splat terrain with the old engine's (game-001-cpp)
Azgaar Fantasy Map Generator heightmap world: `.map`-driven biomes/props,
streaming runtime-generated heightmap tiles, CPU/physics/render surface parity.

Source of truth for all ports: `/home/enes/Projects/c/game-001-cpp`
(`c-engine/ecs/system/heightmap/`, `c-game/game/azgaar/`,
`c-game/game/loadingAzgaar/`, `c-engine/renderer/vulkan/pass/azgaar_*` +
`heightmap_terrain`).

## Architecture notes (carry over verbatim — these are hard-won invariants)

- **The surface is a pure function of (x, z).** `HeightmapSource::heightAt`
  must be deterministic for the process lifetime; evicted tiles regenerate
  bit-identical. CPU grid, physics grid and rendered lattice are all the same
  tensor-product bilinear surface.
- **Geometry detail band:** only fBm octaves with wavelength >= 64 m go into
  geometry (2 octaves: 128/64 m). Shorter wavelengths (32/16/8/4 m) are
  fragment-shader normal perturbation only — putting them in geometry aliases
  on the render lattice and grass/player float above the ground.
- **Settlement plateaus blend LAST** in heightAt, after the detail band, so
  ground under buildings is exactly flatY.
- **CPU precomputed lattice:** tile corners (world pos + border-aware stencil
  normal) are generated on the CPU at height-upload time; the vertex stage is
  a thin transform. This is what makes the pass portable to Filament (no VS
  texture fetches, `vertex()` stays empty) — do not reintroduce implicit
  lattice enumeration in the VS.
- Tile grid: 2048 m edge, 512² heights (4 m texels, endpoints shared for
  watertight borders), 256² physics (~8 m). 5×5 window, LRU eviction,
  background builder thread, `readyStamp` as GPU re-upload cache key.

## Phases

### 1. Remove legacy splat terrain ✅ (done)

Removed: `c-engine/terrain/`, `scripts/build-terrain.py` + `ktx2bc7.c`,
`tools/terrain-chunker/`, pak_1 terrain data, CMake matc target,
`gltfEntitiesNamed` + `NameComponentManager` plumbing, Game.cpp wiring.

### 2. Azgaar world model (.map parse)

Port `AzgaarWorld` + `LoadingAzgaar` into `c-game/game/azgaar/` /
`c-game/game/loadingAzgaar/`. The new engine's `ecs/System` base matches the
old one, so this is near-verbatim:

- `.map` JSON parse: cells (section 1), biomes with colors + habitability +
  icon lists/density (section 3), climate temp/precip/coast/feature
  (sections 8–11), rivers (section 32 + section 5 SVG centerlines),
  settlements (section 15), states/provinces (section 14), landmarks,
  roads (corridors + decals).
- Ship a test map: `c-game/data/pak_1/azgaar/*.map` (old engine ships
  `Chilerel 2026-08-11-15-35.map` — reuse).
- Height helpers: `azgaarWorldSampleHeightSmooth`, `azgaarHeightToMeters`,
  `metersPerPixel`.
- **Acceptance:** log parsed world stats (cells, biomes, river count,
  settlement count, size in metres) after load.

### 3. HeightmapTerrain streaming core

Port `c-engine/ecs/system/heightmap/` (HeightmapSource vtable, HeightmapTile,
window/LRU/eviction, background builder thread, snapshot/copy APIs,
`heightmapTerrainSample`) into the new engine's `c-engine/ecs/`.
Phase-1 scope: CPU grids only; `gpuData`/`physicsData` pointers stay
reserved and unused until phases 5/9.

### 4. AzgaarHeightmapSource

Port the adapter: FMG macro height + seeded (FNV-1a of map name) 2-octave
value-noise detail band + settlement plateau blend. Keep the seabed detail
fade (-10 m → 0).

### 5. Filament terrain render pass

- Per READY tile: generate 256² lattice corners (world pos + border-aware
  normal) on the CPU, upload VBO + shared 255-segment IBO.
- Shader10 material (`terrain.mat`): all look work in `surface()`:
  ground texture blend, micro-band normal perturbation (32/16/8/4 m),
  per-world biome color / climate blend textures (registered at world load),
  waterline. `vertex()` empty.
- Register climate/biome textures from the Azgaar world at load.
- **Acceptance:** `ENGINE_SCREENSHOT` shows streaming terrain following the
  camera; borders watertight (no seams); height ramp debug view matches CPU
  grid.

### 6. Diligent terrain render pass

HLSL mirror of phase 5 (pre-compiled at build time per
`plans/diligent-migration.md`). Same CPU lattice, same surface invariant.

### 7. Props / vegetation

Port `AzgaarProps` scatter (biome `icons` × repetition weight × `iconsDensity`,
Delaunay jitter, samples the *physics* grid for ground height — never the
finer CPU grid) + billboard render pass, both backends.

### 8. Water, rivers, roads, settlements

`AzgaarWater` (global water plane + shoreline), `AzgaarRivers` (centerline
ribbons), `AzgaarRoadCorridor` + `AzgaarRoadDecals`, `AzgaarSettlements`
(flatY plateaus already in heightAt; buildings/props). Weather is deferred —
not needed for the terrain cut.

### 9. Gameplay hooks + physics

- Camera framing from world bounds (replace the TODO in `Game::loadWorld`).
- Ground height query for the future player (`heightmapTerrainSample` /
  physics-tile copy).
- Collision: Jolt heightfields per tile (old engine phase 3) or a custom
  bilinear-grid raycast — decide when the player lands; the grids are ready
  either way.

## Out of scope (for now)

Per-ring LOD ladder (needs per-ring CPU corner sets, not VS math),
TAA/HiZ/motion-vector pipeline on the Filament path (scene-wide, separate
workstream), `AzgaarWeather`.
