#include "ecs/system/heightmap/HeightmapLattice.h"

#include <cmath>

namespace engine {

u32 heightmapLatticeCornerCount(void) {
    const u32 cols = (u32)HEIGHTMAP_LATTICE_SEG + 1u;
    return cols * cols;
}

u32 heightmapLatticeIndexCount(void) {
    return 6u * (u32)HEIGHTMAP_LATTICE_SEG * (u32)HEIGHTMAP_LATTICE_SEG;
}

void heightmapLatticeBuildIndices(u32* outIndices) {
    u32* p        = outIndices;
    const u32 cols = (u32)HEIGHTMAP_LATTICE_SEG + 1u;
    for (u32 pz = 0; pz < (u32)HEIGHTMAP_LATTICE_SEG; pz++) {
        for (u32 px = 0; px < (u32)HEIGHTMAP_LATTICE_SEG; px++) {
            u32 a = pz * cols + px; // (x, z)
            u32 b = a + 1;          // (x+1, z)
            u32 c = a + cols;       // (x, z+1)
            u32 d = c + 1;          // (x+1, z+1)
            *p++ = a;
            *p++ = c;
            *p++ = d;
            *p++ = a;
            *p++ = d;
            *p++ = b;
        }
    }
}

// Bilinear fetch of the CPU height grid in TEXEL SPACE (s in [0, dim-1],
// texel centres at integers), replicating a linear sampler's spec formula
// (1-t)*S(u) + t*S(u+1) per axis.
static float latticeBilinear(const float* grid, u32 dim, float sx, float sy) {
    float lim = (float)dim - 1.0f;
    if (sx < 0.0f) sx = 0.0f;
    if (sy < 0.0f) sy = 0.0f;
    if (sx > lim) sx = lim;
    if (sy > lim) sy = lim;
    i32 ix = (i32)sx;
    i32 iz = (i32)sy;
    if (ix > (i32)dim - 2) ix = (i32)dim - 2;
    if (iz > (i32)dim - 2) iz = (i32)dim - 2;
    float fx   = sx - (float)ix;
    float fy   = sy - (float)iz;
    const float* r0 = grid + (size_t)iz * dim;
    const float* r1 = r0 + dim;
    float h00 = r0[(u32)ix], h10 = r0[(u32)ix + 1];
    float h01 = r1[(u32)ix], h11 = r1[(u32)ix + 1];
    float x0  = (1.0f - fx) * h00 + fx * h10;
    float x1  = (1.0f - fx) * h01 + fx * h11;
    return (1.0f - fy) * x0 + fy * x1;
}

void heightmapLatticeBuildCorners(const float* heights,
                                  float originX,
                                  float originZ,
                                  float sizeMeters,
                                  HeightmapLatticeCorner* outCorners) {
    const u32   tex    = HEIGHTMAP_TEX;
    const float size   = sizeMeters;
    const float seg    = (float)HEIGHTMAP_LATTICE_SEG;
    const float cell   = size / seg;
    const float invTex = 1.0f / (float)tex;
    const float k      = 1.0f - invTex; // (TEX-1)/TEX
    const float cellTexel = size / ((float)tex - 1.0f);
    const float* g = heights;

    for (u32 pz = 0; pz <= (u32)HEIGHTMAP_LATTICE_SEG; pz++) {
        for (u32 px = 0; px <= (u32)HEIGHTMAP_LATTICE_SEG; px++) {
            HeightmapLatticeCorner* o = outCorners + pz * ((u32)HEIGHTMAP_LATTICE_SEG + 1u) + px;

            float localX = (float)px * cell;
            float localZ = (float)pz * cell;

            // Texel-centre addressing, same formula as the old implicit VS.
            float uvsX = (localX / size) * k + 0.5f * invTex;
            float uvsY = (localZ / size) * k + 0.5f * invTex;
            float sx = uvsX * (float)tex - 0.5f; // texel space
            float sy = uvsY * (float)tex - 0.5f;

            float h = latticeBilinear(g, tex, sx, sy);

            // Border-aware one-sided stencil (an outward fetch would
            // clamp-repeat to the opposite tile edge): same logic as the
            // old implicit VS.
            float spanX = 2.0f * cellTexel;
            float spanZ = 2.0f * cellTexel;
            float hL, hR, hD, hU;
            if (uvsX < invTex) {
                hL = h;
                hR = latticeBilinear(g, tex, sx + 1.0f, sy);
                spanX = cellTexel;
            } else if (uvsX > 1.0f - invTex) {
                hR = h;
                hL = latticeBilinear(g, tex, sx - 1.0f, sy);
                spanX = cellTexel;
            } else {
                hL = latticeBilinear(g, tex, sx - 1.0f, sy);
                hR = latticeBilinear(g, tex, sx + 1.0f, sy);
            }
            if (uvsY < invTex) {
                hD = h;
                hU = latticeBilinear(g, tex, sx, sy + 1.0f);
                spanZ = cellTexel;
            } else if (uvsY > 1.0f - invTex) {
                hU = h;
                hD = latticeBilinear(g, tex, sx, sy - 1.0f);
                spanZ = cellTexel;
            } else {
                hD = latticeBilinear(g, tex, sx, sy - 1.0f);
                hU = latticeBilinear(g, tex, sx, sy + 1.0f);
            }

            float nx = (hL - hR) / spanX;
            float nz = (hD - hU) / spanZ;
            float inv = 1.0f / sqrtf(nx * nx + 1.0f + nz * nz);

            o->pos[0]    = originX + localX;
            o->pos[1]    = h;
            o->pos[2]    = originZ + localZ;
            o->normal[0] = nx * inv;
            o->normal[1] = inv;
            o->normal[2] = nz * inv;
        }
    }
}
}  // namespace engine
