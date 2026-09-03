#pragma once

#include "azgaar/AzgaarWorld.h"

/*
 * AzgaarSettlements (minimal phase-4 port)
 * ----------------------------------------
 * CPU side of the settlement system, phase 4: only the D8 terrain plateau
 * query, which AzgaarHeightmapSource needs so towns sit on level ground
 * (y += (flatY - y) * (1 - smoothstep(0.55r, r, d))).  The full module from
 * the old engine (deterministic building clusters, azgaar_props upload,
 * zone-banner nearest query) lands in phase 8 and folds this in.
 *
 * The plateau is backed by a lazily-built spatial grid over world->settlements
 * (immutable after azgaarWorldLoad).  Build it once in
 * azgaarHeightmapSourceInit, before any tile generates.
 *
 * Kill switch: ENGINE_AZGAAR_SETTLE_DISABLED (any value) disables the
 * plateau — azgaarSettlementsPlateauY returns naturalY unchanged.
 */
namespace game {
// Build the plateau grid from world->settlements (1024 m bucket grid over
// the map AABB) and activate the plateau.  No-op (empty grid) for a null or
// settlement-free world.  May be called again with a different world; the
// old grid is replaced in place.
void azgaarSettlementsPlateauInit(const AzgaarWorld* world);

// Drop the grid.  After this, azgaarSettlementsPlateauY is a no-op.
void azgaarSettlementsPlateauClear(void);

// D8 plateau: given the natural terrain height (meters) at world (wx, wz),
// returns the height blended toward each nearby settlement's flat centre.
// Stateless once the grid exists — a pure function of (grid, wx, wz), safe
// to call from the background tile-build thread.  No-op when the kill
// switch is set, the grid is empty, or no settlement is within its radius.
float azgaarSettlementsPlateauY(const AzgaarWorld* world, float wx, float wz, float naturalY);
}  // namespace game
