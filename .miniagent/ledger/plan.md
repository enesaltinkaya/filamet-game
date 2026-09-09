# Plan — remove Azgaar map rendering

Drop the Azgaar `.map`-driven world entirely (phase 4 of `plans/blender-terrain.md`,
pulled forward of the splat pass): the game renders the player model + sky in the
void until the Blender Oghuzlands splat world lands.

## Scope (delete)

- `c-game/game/azgaar/` (World, HeightmapSource, PropMesh, Props, Settlements)
- `c-game/game/loadingAzgaar/`
- `c-engine/ecs/system/heightmap/` (streaming core + Jolt heightfield + render-look API)
- `c-engine/renderer/PropsRender.{h,cpp}` + `diligent/PropsRenderDiligent.{h,cpp}`
- `c-engine/renderer/diligent/HeightmapTerrainDiligent.{h,cpp}`
- 7 .hlsl (heightmap_terrain_* ×3, props_* ×4) in `renderer/diligent/shaders/`
  + their pak_1/materials copies + the CMake copy rule
- pak data: `pak_1/azgaar/*.map`, `pak_1/images/grass-textures/`,
  `pak_1/images/leaf-textures/`
- `scripts/make_leaf_sprite.py`
- Game.cpp world wiring: props bridge/perf/acceptance, look registration,
  world spawn/camera vantages derived from map data; PlayerGui cell text;
  PlayerActionsGui Azgaar-cell teleport; MainMenuGui heightmapTerrainSystem add
- Player ground-snap / waitingForGround gate (no heightfield anymore)
- Shadow-pass terrain/props draws + env flags + props wind cbuffer (player
  shadow receiver cascade pick switches to playerGetFootPos)
- docs/env.md Azgaar sections, renderdoc-capture.md pass table, plans status notes

## Keep

- c-utils/treegen (terrain-agnostic generator, planned reuse for the new world)
- pak_1/models/terrain + images/terrain (future splat world), blender scripts
- player/flyingCamera/physics systems, gltf pass, TAA/SSAO/bloom, GUI, menus

## Verification

1. `scripts/build.sh` clean.
2. Headless run (`ENGINE_NO_RMLUI=1` auto-enters world) exits clean, no new
   VUIDs/warnings vs baseline; screenshot shows sky + player model.
3. No stray references: `grep -ri azgaar` clean outside docs/lessons + plans.
