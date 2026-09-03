# plan — azgaar-terrain phase 4: AzgaarHeightmapSource

Phase 4 ports the CPU height-source adapter from the old engine (game-001-cpp,
`c-game/game/azgaar/AzgaarHeightmapSource.{h,cpp}`, ~160 lines) into this
engine. The new engine already has the `HeightmapSource` vtable
(c-engine/ecs/system/heightmap/HeightmapSource.h) and the loaded `AzgaarWorld`
(c-game/game/azgaar/), and Game.cpp has a TODO at the wiring point
(`// TODO(azgaar): build heightmap terrain from loadingAzgaarGetWorld()`).

Approach: port `AzgaarHeightmapSource` near-verbatim into `c-game/game/azgaar/`
(FMG macro height via `azgaarWorldSampleHeightSmooth` + FNV-1a(map-name)-seeded
2-octave value-noise detail band on the 128/64 m wavelengths + settlement
plateau blend LAST + seabed detail fade -10 m → 0). CMake uses GLOB_RECURSE so
no build-file edits are needed. Wire it into the load path (LoadingAzgaar or
Game::loadWorld) so a HeightmapTerrain with this source is actually created,
and log a few sampled heights vs the CPU grid after load as the acceptance
check (surface must be a pure, deterministic function of (x,z)).

Verification: ./scripts/build.sh
Baseline commit: f8eb1e37560a99b389706f20b146f190f9849e54 (dirty)
