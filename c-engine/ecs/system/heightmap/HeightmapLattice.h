#pragma once
#include "ecs/system/heightmap/HeightmapTerrain.h"

/*
 * HeightmapLattice
 * ----------------
 * CPU-side render lattice for one heightmap tile: (HEIGHTMAP_LATTICE_SEG+1)^2
 * corners (world position + border-aware one-sided stencil normal) over
 * HEIGHTMAP_LATTICE_SEG segments, plus the shared index buffer.
 *
 * This is the canonical geometry both render backends build their tile
 * meshes from (Filament phase 5, Diligent phase 6): the corner math is a
 * port of the old engine's implicit-lattice vertex stage (texel-centre
 * bilinear heights, border-aware normals), so the rendered surface is
 * bit-identical to the CPU/physics bilinear surface and adjacent tiles
 * emit identical border vertices (watertight, no seams, no T-junctions).
 *
 * Pure functions, no GPU, no lock: the caller passes a tile's published
 * height grid (HeightmapTileView from heightmapTerrainSnapshotTiles is
 * safe for this — see the snapshot docs).
 */

namespace engine {

// One render-lattice corner: world-space position + world-space geometric
// normal (the normal a heightfield stencil gives at that point).
struct HeightmapLatticeCorner {
    float pos[3];
    float normal[3];
};

// Lattice tessellation: 256^2 corners / 255 segments (8 m per segment),
// bilinearly sampled from the 512^2 height grid (4 m texels). Uniform for
// every tile/ring (a per-ring segment ladder would need per-ring corner
// data and cracks at ring boundaries — see the plan's out-of-scope notes).
// 8 m segments comfortably cover the >= 64 m geometry detail band, so no
// aliasing; the 32/16/8/4 m octaves stay in the fragment normal pass.
#define HEIGHTMAP_LATTICE_SEG 255

// Number of corners per tile: (SEG+1)^2 (256^2 = 65536).
u32 heightmapLatticeCornerCount(void);

// Number of indices of the shared lattice index buffer: 6 * SEG^2.
u32 heightmapLatticeIndexCount(void);

// Fill outIndices (at least heightmapLatticeIndexCount() u32s) with the
// shared lattice topology. Winding is CCW from above (+Y), matching the
// standard back-face-culled convention of both backends.
void heightmapLatticeBuildIndices(u32* outIndices);

// Fill outCorners (heightmapLatticeCornerCount() corners) from the tile's
// CPU height grid. Replicates the old implicit vertex shader float
// arithmetic corner-for-corner (texel-centre bilinear fetch, border-aware
// one-sided stencil normal) so both backends get identical geometry.
void heightmapLatticeBuildCorners(const float* heights,
                                  float originX,
                                  float originZ,
                                  float sizeMeters,
                                  HeightmapLatticeCorner* outCorners);
}  // namespace engine
