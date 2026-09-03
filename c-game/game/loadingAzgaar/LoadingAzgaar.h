#pragma once

#include "azgaar/AzgaarHeightmapSource.h"
#include "azgaar/AzgaarWorld.h"

namespace game {

// Phase-2 loadingAzgaar: owns the retained AzgaarWorld.  Loads the .map from
// the pak on world-enter, exposes it to later phases (heightmap, climate
// textures, rivers/settlements/landmarks), and releases it on world-exit.
// No GUI, no heightmap, no rmlui in this phase.

// Loads and parses the Azgaar .map (synchronous, one line of stats on success).
// Idempotent: returns true immediately if the world is already loaded.
bool loadingAzgaarLoad();

// The loaded world, or nullptr before load / after release.
const AzgaarWorld* loadingAzgaarGetWorld();

// The AzgaarHeightmapSource backing the active world (nullptr before the map
// is loaded / after release). Its heightAt is the CANONICAL terrain surface
// (FMG macro heights + seeded fBm detail + settlement plateau) — the same
// function the heightmap pass bakes into its textures and the Jolt
// heightfields collide against. Consumers that need the exact ground surface
// must sample through it, never a private height function. It is file-static
// here and outlives any HeightmapTerrain built from it; the terrain must be
// destroyed (heightmapTerrainDestroyData + setActive(nullptr)) and the
// settlement plateau grid cleared (azgaarSettlementsPlateauClear) BEFORE
// loadingAzgaarReleaseWorld frees the world behind it.
// (Non-const: the HeightmapSource vtable passes userData as void*; heightAt
// is a pure function and never mutates the source.)
AzgaarHeightmapSource* loadingAzgaarGetHeightmapSource();

// Frees the retained world.  Safe to call when nothing is loaded.
void loadingAzgaarReleaseWorld();
}  // namespace game
