#include "azgaar/AzgaarPropMesh.h"
#include "azgaar/AzgaarProps.h"
#include "Utils.h"

#include <cfloat>
#include <math.h>
#include <stdio.h>

/*
 * Procedural species mesh builders, ported verbatim from the old engine
 * (game-001-cpp, c-game/game/azgaar/AzgaarProps.cpp: the MeshBuilder
 * helpers + the 12 vegetation species builders + the crossed grass card).
 * The only adaptations: the vertex is the slimmer AzgaarPropMesh layout
 * (the old PropsVertex carried tangent/joints/weights/texId, unused here —
 * species/variant is the draw-call identity, not a per-vertex texture
 * index), and the grass card no longer records a texture-array id (one
 * card = one variant = one texture, selected by the range).
 *
 * All geometry is UNIT height (base y=0, top y=1) so an instance's uniform
 * scale is its target height in metres, exactly like the old pass.
 */
namespace game {

namespace {

struct MeshBuilder {
    std::vector<AzgaarPropVertex> verts;
    u32 vertCap, vertCount;
    std::vector<u32> idx;
    u32 idxCap, idxCount;
};

void mbInit(MeshBuilder* mb, u32 vertCap, u32 idxCap) {
    mb->verts.resize(vertCap);
    mb->vertCap   = vertCap;
    mb->vertCount = 0;
    mb->idx.resize(idxCap);
    mb->idxCap   = idxCap;
    mb->idxCount = 0;
}

// [DEBUG] Dump one built range to an OBJ for silhouette inspection
// (matches the old engine's propsDumpBuilder; see the ledger task).
void propsDumpBuilder(const char* path, const MeshBuilder* mb) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (u32 i = 0; i < mb->vertCount; i++) {
        const AzgaarPropVertex* p = &mb->verts[i];
        fprintf(f, "v %f %f %f\n", p->position[0], p->position[1], p->position[2]);
    }
    for (u32 i = 0; i < mb->idxCount; i += 3) {
        fprintf(f, "f %u %u %u\n", mb->idx[i] + 1, mb->idx[i + 1] + 1, mb->idx[i + 2] + 1);
    }
    fclose(f);
    utils::info("azgaarPropMesh: dumped %s (%u verts, %u idx)", path, mb->vertCount,
            mb->idxCount);
}

u32 mbAddVert(MeshBuilder* mb,
              float x,
              float y,
              float z,
              float nx,
              float ny,
              float nz,
              float u,
              float v) {
    u32 i = mb->vertCount;
    if (i >= mb->vertCap) return (u32)-1;
    AzgaarPropVertex* p = &mb->verts[i];
    p->position[0]      = x;
    p->position[1]      = y;
    p->position[2]      = z;
    p->normal[0]        = nx;
    p->normal[1]        = ny;
    p->normal[2]        = nz;
    p->normal[3]        = 0.0f;
    p->uv[0]            = u;
    p->uv[1]            = v;
    p->color[0]         = 1.0f;  // white = tintable (receives the per-instance tint)
    p->color[1]         = 1.0f;
    p->color[2]         = 1.0f;
    p->color[3]         = 1.0f;
    mb->vertCount++;
    return i;
}

// Override a vertex' part colour (e.g. mark trunk verts brown so they are
// NOT tinted by the per-instance biome colour).  `color` is a 3-float array.
void mbVertColor(MeshBuilder* mb, u32 idx, const float color[3]) {
    if (idx == (u32)-1 || idx >= mb->vertCount) return;
    AzgaarPropVertex* p = &mb->verts[idx];
    p->color[0]         = color[0];
    p->color[1]         = color[1];
    p->color[2]         = color[2];
}

void mbTri(MeshBuilder* mb, u32 a, u32 b, u32 c) {
    if (a == (u32)-1 || b == (u32)-1 || c == (u32)-1) return;
    if (mb->idxCount + 3 > mb->idxCap) return;
    mb->idx[mb->idxCount++] = a;
    mb->idx[mb->idxCount++] = b;
    mb->idx[mb->idxCount++] = c;
}

void mbQuad(MeshBuilder* mb, u32 a, u32 b, u32 c, u32 d) {
    mbTri(mb, a, b, c);
    mbTri(mb, a, c, d);
}

// An N-sided frustum/cone ring from baseY to topY (open, side quads only).
// Returns the first base-vertex index.
u32 mbCone(MeshBuilder* mb,
           float cx,
           float cz,
           float baseY,
           float topY,
           float baseR,
           float topR,
           u32 sides) {
    if (sides < 3) sides = 3;
    float span = topY - baseY;
    if (span <= 0.0f) span = 0.001f;
    // Outward side normal per segment: radial part weighted by the height
    // span, vertical part by the taper (baseR > topR tilts it upward, equal
    // radii -> purely radial).  The old code baked ONE vertical normal for
    // the whole cone, which degenerated to (0,0,0) for straight cylinders
    // (tower shafts, gate posts, lighthouse lantern, ...) and handed
    // normalize() an undefined input, so those parts shaded flat/dead.
    float taper = baseR - topR;
    float inv   = 1.0f / sqrtf(span * span + taper * taper);
    float nr    = span * inv;
    float ny    = taper * inv;
    u32 ring0   = mb->vertCount;
    for (u32 s = 0; s < sides; s++) {
        float a0 = static_cast<float>(s) / static_cast<float>(sides) * 2.0f * M_PI;
        float a1 = static_cast<float>(s + 1) / static_cast<float>(sides) * 2.0f * M_PI;
        float am = 0.5f * (a0 + a1);  // segment centre: shared flat normal
        float nx = cosf(am) * nr;
        float nz = sinf(am) * nr;
        u32 b0 = mbAddVert(mb,
                           cx + cosf(a0) * baseR,
                           baseY,
                           cz + sinf(a0) * baseR,
                           nx,
                           ny,
                           nz,
                           0,
                           0);
        u32 b1 = mbAddVert(mb,
                           cx + cosf(a1) * baseR,
                           baseY,
                           cz + sinf(a1) * baseR,
                           nx,
                           ny,
                           nz,
                           0,
                           0);
        u32 t0 =
            mbAddVert(mb, cx + cosf(a0) * topR, topY, cz + sinf(a0) * topR, nx, ny, nz, 0, 1);
        u32 t1 =
            mbAddVert(mb, cx + cosf(a1) * topR, topY, cz + sinf(a1) * topR, nx, ny, nz, 0, 1);
        // Band must wind CCW from OUTSIDE (front face).  The old winding was
        // clockwise, so these sides rasterized as back faces and the
        // fragment shader' gl_FrontFacing flip inverted their normals.
        //
        // The quad splits along ONE diagonal (b0->t1).  The second tri used
        // to be (b0, t0, b1) — the OTHER diagonal — so the two tris only met
        // at the base edge and left the top corner (t0, t1, centre) of every
        // segment uncovered: a row of sky wedges under the roofline, the
        // "jagged crown" visible on the citadel tower / lighthouse.
        mbTri(mb, b0, t1, b1);
        mbTri(mb, b0, t0, t1);
    }
    // Flat top cap (so cones read solid from below).  The band interleaves
    // four verts per segment (b0, b1, t0, t1), so the top ring is NOT a
    // contiguous block: segment s' top verts sit at ring0 + 4*s + 2/3.
    // Fan the centre to the CONSECUTIVE top verts of each segment (t0->t1,
    // one segment apart) so the triangles tile the whole disc.  Fanning to
    // the next segment' t1 (4*(s+1)+3) instead makes each chord skip a
    // vertex and leaves the circular segment uncovered — a jagged crown
    // instead of a solid cap (visible on the lighthouse).
    if (topR > 0.001f) {
        u32 c = mbAddVert(mb, cx, topY, cz, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);
        for (u32 s = 0; s < sides; s++) {
            u32 t0 = ring0 + 4u * s + 2u;
            u32 t1 = ring0 + 4u * s + 3u;
            mbTri(mb, c, t1, t0);  // CCW from above (front face, normal +Y)
        }
    }
    return ring0;
}

// A solid cylinder (side + base + top).  Convenience wrapper over mbCone.
void
mbCylinder(MeshBuilder* mb, float cx, float cz, float baseY, float topY, float r, u32 sides) {
    mbCone(mb, cx, cz, baseY, topY, r, r, sides);
    u32 c0 = mbAddVert(mb, cx, baseY, cz, 0.0f, -1.0f, 0.0f, 0.5f, 0.0f);
    for (u32 s = 0; s < sides; s++) {
        float a0 = static_cast<float>(s) / static_cast<float>(sides) * 2.0f * M_PI;
        float a1 = static_cast<float>(s + 1) / static_cast<float>(sides) * 2.0f * M_PI;
        u32 b0   = mbAddVert(mb, cx + cosf(a0) * r, baseY, cz + sinf(a0) * r, 0, -1, 0, 0, 0);
        u32 b1   = mbAddVert(mb, cx + cosf(a1) * r, baseY, cz + sinf(a1) * r, 0, -1, 0, 0, 0);
        mbTri(mb, c0, b0, b1);  // CCW from below (front face, normal -Y)
    }
}

// A small displaced octahedron blob (6 verts, 8 tris) — rocks / shrubs.
// `flat` squashes it in Y. `jit` displaces each vertex radially (deterministic
// via a salt hash) so rocks read as boulders, not crystals.
void mbBlob(MeshBuilder* mb, float cx, float cy, float cz, float r, float flat, u32 salt) {
    float ry = r * flat;
    u32 v[6];
    v[0] = mbAddVert(mb, cx, cy + ry, cz, 0, 1, 0, 0.5f, 1.0f);
    v[1] = mbAddVert(mb, cx, cy - ry, cz, 0, -1, 0, 0.5f, 0.0f);
    for (u32 k = 0; k < 4; k++) {
        float a  = static_cast<float>(k) / 4.0f * 2.0f * M_PI;
        float j  = 1.0f + 0.25f * (static_cast<float>((salt + k * 7) & 3) -
                                   1.5f);  // deterministic 0.75..1.25
        v[2 + k] = mbAddVert(mb,
                             cx + cosf(a) * r * j,
                             cy,
                             cz + sinf(a) * r * j,
                             cosf(a),
                             0.0f,
                             sinf(a),
                             static_cast<float>(k),
                             0.5f);
    }
    mbTri(mb, v[0], v[2], v[3]);
    mbTri(mb, v[0], v[3], v[4]);
    mbTri(mb, v[0], v[4], v[5]);
    mbTri(mb, v[0], v[5], v[2]);
    mbTri(mb, v[1], v[3], v[2]);
    mbTri(mb, v[1], v[4], v[3]);
    mbTri(mb, v[1], v[5], v[4]);
    mbTri(mb, v[1], v[2], v[5]);
}

// A UV sphere (seg x ring) centred at (cx, cy, cz), radius r, squashed by
// `flat` in Y.  Used for the deciduous canopy so it reads as a rounded crown
// rather than a flat diamond.  Verts = (seg+1)*(ring+1); tris = seg*ring*2.
void mbSphere(MeshBuilder* mb,
              float cx,
              float cy,
              float cz,
              float r,
              float flat,
              u32 seg,
              u32 ring) {
    if (seg < 3) seg = 3;
    if (ring < 2) ring = 2;
    float ry  = r * flat;
    u32 first = mb->vertCount;
    for (u32 j = 0; j <= ring; j++) {
        float v   = static_cast<float>(j) / static_cast<float>(ring);
        float phi = v * M_PI;
        float y   = cy + cosf(phi) * ry;
        float rad = r * sinf(phi);
        for (u32 i = 0; i <= seg; i++) {
            float u     = static_cast<float>(i) / static_cast<float>(seg);
            float theta = u * 2.0f * M_PI;
            float x     = cx + cosf(theta) * rad;
            float z     = cz + sinf(theta) * rad;
            float nx    = cosf(theta) * sinf(phi);
            float nz    = sinf(theta) * sinf(phi);
            float ny    = cosf(phi) * flat;
            mbAddVert(mb, x, y, z, nx, ny, nz, u, v);
        }
    }
    for (u32 j = 0; j < ring; j++) {
        for (u32 i = 0; i < seg; i++) {
            u32 a = first + j * (seg + 1) + i;
            u32 b = a + 1;
            u32 c = a + (seg + 1);
            u32 d = c + 1;
            mbQuad(mb, a, b, d, c);
        }
    }
}

// `count` crossed blades from the base (grass / reed).  Each blade is a short
// strip of quads that curves outward as it rises and tapers to a point, so a
// tuft reads as grass instead of flat cards.  Per-blade height and curl vary
// deterministically (golden-ratio hash on the blade index, stable across
// tiles / rebuilds) so the silhouette is uneven like real turf.
// Verts per blade = 2*SEGS + 1 (pointed tip); tris = 2*SEGS - 1.
void mbBlades(MeshBuilder* mb, u32 count, float height, float halfW, float spread) {
    const u32 SEGS = 3;
    for (u32 b = 0; b < count; b++) {
        float f    = static_cast<float>(b);
        float a    = f / static_cast<float>(count) * 2.0f * M_PI + 0.3f;
        float dirX = cosf(a), dirZ = sinf(a);
        float h1 = f * 0.6180339887f + 0.13f;
        h1 -= floorf(h1);
        float h2 = f * 0.379f + 0.71f;
        h2 -= floorf(h2);
        float H    = height * (0.80f + 0.35f * h1);  // uneven blade heights
        float bend = 0.30f + 0.40f * h2;             // outward curl (rad)
        float R    = H / bend;                       // arc radius
        float bx = dirX * spread, bz = dirZ * spread;
        u32 prev0 = (u32)-1, prev1 = (u32)-1;
        for (u32 i = 0; i <= SEGS; i++) {
            float t  = static_cast<float>(i) / static_cast<float>(SEGS);
            float th = bend * t;
            float ct = cosf(th), st = sinf(th);
            float r  = R * (1.0f - ct);
            float y  = R * st;
            float cx = bx + dirX * r, cz = bz + dirZ * r;
            // Top-face normal: convex side of the outward curl (straight up at
            // the base, tilted up-inward at the tip).
            float nx = -dirX * ct, ny = st, nz = -dirZ * ct;
            if (i == SEGS) {
                u32 tip = mbAddVert(mb, cx, y, cz, nx, ny, nz, 0.5f, 1.0f);
                mbTri(mb, prev0, prev1, tip);
                break;
            }
            float w = halfW * powf(1.0f - t, 1.5f);  // taper to a point
            // Pinch the very base (where the blade meets the ground) so the
            // tuft skirt reads thin; full width is reached by t = 0.25.
            w *= 0.3f + 0.7f * fminf(t / 0.25f, 1.0f);
            float px = -dirZ * w, pz = dirX * w;  // width axis
            u32 v0 = mbAddVert(mb, cx - px, y, cz - pz, nx, ny, nz, 0.0f, t);
            u32 v1 = mbAddVert(mb, cx + px, y, cz + pz, nx, ny, nz, 1.0f, t);
            if (prev0 != (u32)-1) mbQuad(mb, prev0, prev1, v1, v0);
            prev0 = v0;
            prev1 = v1;
        }
    }
}

// A single small quad (flowers): unit UV so the fragment shader's radial alpha
// test reads it as a flower dot.  A thin stem rises to the quad.
void mbFlower(MeshBuilder* mb) {
    // stem
    mbCylinder(mb, 0.0f, 0.0f, 0.0f, 0.9f, 0.02f, 4);
    // flower head: a diamond quad at y=1.0 with unit UV.
    float h = 0.9f;
    float s = 0.28f;
    u32 n   = mbAddVert(mb, 0.0f, h + s, 0.0f, 0, 1, 0, 0.5f, 1.0f);
    u32 e   = mbAddVert(mb, s, h, 0.0f, 0, 1, 0, 1.0f, 0.5f);
    u32 w   = mbAddVert(mb, -s, h, 0.0f, 0, 1, 0, 0.0f, 0.5f);
    u32 f   = mbAddVert(mb, 0.0f, h - s * 0.5f, 0.0f, 0, 1, 0, 0.5f, 0.0f);
    mbQuad(mb, n, e, f, w);
}

// Per-species geometry builders (unit height, base at y=0).
// Baked brown trunk colour: NOT tinted by the per-instance biome tint, so
// trunks stay brown like in Blender instead of going green in-game.
static const float kTrunkColor[3] = {0.36f, 0.25f, 0.16f};

// Colour the contiguous vertex block added by the most recent geometry call
// (a single mbCylinder / mbCone), starting at `start`.
void mbColorSince(MeshBuilder* mb, u32 start, const float color[3]) {
    for (u32 i = start; i < mb->vertCount; i++) mbVertColor(mb, i, color);
}

// A crossed grass card: two perpendicular quads (a "+" viewed from above),
// each carrying the grass texture.  The texture's alpha channel is tested in
// the fragment shader (alpha test), so only the tuft pixels survive and the
// card reads as grass instead of a flat quad.  `aspect` is the texture's
// width/height (the mesh is unit-height, so the card width == aspect).
// `vBottom` is the V of the card's bottom edge: the card's V spans
// [0, vBottom] instead of [0, 1] so the texture's empty bottom band is
// trimmed and the visible tuft ends exactly at the card's base (the ground)
// instead of floating above it (vBottom = 1.0 keeps the full texture).
// Verts are white (tintable) so the per-instance biome tint modulates the
// texture.  8 verts / 4 tris.
void buildGrassCard(MeshBuilder* mb, float aspect, float vBottom) {
    if (aspect <= 0.0f) aspect = 1.0f;
    if (vBottom < 0.0f) vBottom = 0.0f;
    else if (vBottom > 1.0f) vBottom = 1.0f;
    float hw  = aspect * 0.5f;  // half-width (unit height = 1)
    // Quad A (along X, in the XY plane).  V=0 is the image top (blades),
    // V=1 the image bottom (empty padding) — see the grass variant loader.
    u32 a0 = mbAddVert(mb, -hw, 0.0f, 0.0f, 0, 1, 0, 0.0f, vBottom);
    u32 a1 = mbAddVert(mb, hw, 0.0f, 0.0f, 0, 1, 0, 1.0f, vBottom);
    u32 a2 = mbAddVert(mb, hw, 1.0f, 0.0f, 0, 1, 0, 1.0f, 0.0f);
    u32 a3 = mbAddVert(mb, -hw, 1.0f, 0.0f, 0, 1, 0, 0.0f, 0.0f);
    mbQuad(mb, a0, a1, a2, a3);
    // Quad B (along Z, in the ZY plane).
    u32 b0 = mbAddVert(mb, 0.0f, 0.0f, -hw, 0, 1, 0, 0.0f, vBottom);
    u32 b1 = mbAddVert(mb, 0.0f, 0.0f, hw, 0, 1, 0, 1.0f, vBottom);
    u32 b2 = mbAddVert(mb, 0.0f, 1.0f, hw, 0, 1, 0, 1.0f, 0.0f);
    u32 b3 = mbAddVert(mb, 0.0f, 1.0f, -hw, 0, 1, 0, 0.0f, 0.0f);
    mbQuad(mb, b0, b1, b2, b3);
}

void buildConifer(MeshBuilder* mb) {
    u32 trunkStart = mb->vertCount;
    mbCylinder(mb, 0, 0, 0.0f, 0.25f, 0.06f, 6);  // trunk
    mbColorSince(mb, trunkStart, kTrunkColor);
    mbCone(mb, 0, 0, 0.15f, 0.55f, 0.38f, 0.0f, 7);  // lower cone
    mbCone(mb, 0, 0, 0.45f, 0.82f, 0.27f, 0.0f, 7);
    mbCone(mb, 0, 0, 0.72f, 1.0f, 0.16f, 0.0f, 7);
}

void buildConiferFar(MeshBuilder* mb) {
    u32 trunkStart = mb->vertCount;
    mbCylinder(mb, 0, 0, 0.0f, 0.3f, 0.06f, 5);
    mbColorSince(mb, trunkStart, kTrunkColor);
    mbCone(mb, 0, 0, 0.2f, 1.0f, 0.34f, 0.0f, 6);
}

void buildDeciduous(MeshBuilder* mb) {
    u32 trunkStart = mb->vertCount;
    mbCylinder(mb, 0, 0, 0.0f, 0.5f, 0.05f, 6);
    mbColorSince(mb, trunkStart, kTrunkColor);
    // Rounded crown (UV sphere) instead of the flat diamond blob.
    mbSphere(mb, 0.0f, 0.78f, 0.0f, 0.45f, 0.9f, 8, 6);
}

void buildDeciduousFar(MeshBuilder* mb) {
    u32 trunkStart = mb->vertCount;
    mbCylinder(mb, 0, 0, 0.0f, 0.45f, 0.05f, 5);
    mbColorSince(mb, trunkStart, kTrunkColor);
    mbSphere(mb, 0.0f, 0.72f, 0.0f, 0.45f, 0.85f, 6, 4);
}

void buildAcacia(MeshBuilder* mb) {
    // slightly bent trunk + a flat, wide canopy disc near the top.
    u32 trunkStart = mb->vertCount;
    mbCone(mb, 0.05f, 0.0f, 0.0f, 0.7f, 0.07f, 0.05f, 6);
    mbColorSince(mb, trunkStart, kTrunkColor);
    mbCone(mb, 0.1f, 0.0f, 0.68f, 0.82f, 0.55f, 0.5f, 10);  // disc canopy
}

void buildPalm(MeshBuilder* mb) {
    u32 trunkStart = mb->vertCount;
    mbCone(mb, 0.0f, 0.0f, 0.0f, 0.85f, 0.08f, 0.05f, 6);  // trunk
    mbColorSince(mb, trunkStart, kTrunkColor);
    // fronds: flat quads radiating from the crown.
    u32 crown = 8;
    for (u32 k = 0; k < crown; k++) {
        float a  = static_cast<float>(k) / static_cast<float>(crown) * 2.0f * M_PI;
        float fx = cosf(a) * 0.4f;
        float fz = sinf(a) * 0.4f;
        u32 c0   = mbAddVert(mb, 0, 0.85f, 0, 0, 1, 0, 0.5f, 0.5f);
        u32 c1   = mbAddVert(mb, fx * 0.5f, 0.82f, fz * 0.5f, 0, 0.5f, 0, 0.2f, 0.5f);
        u32 tip  = mbAddVert(mb, fx, 0.95f + 0.05f * sinf(a), fz, 0, 0.3f, 0, 1.0f, 0.5f);
        u32 c2   = mbAddVert(mb, fx * 0.5f, 0.78f, fz * 0.5f, 0, 0.5f, 0, 0.8f, 0.5f);
        mbQuad(mb, c0, c1, tip, c2);
    }
}

void buildCactus(MeshBuilder* mb) {
    mbCylinder(mb, 0, 0, 0.0f, 1.0f, 0.14f, 6);
    mbCone(mb, 0.14f, 0.0f, 0.35f, 0.6f, 0.09f, 0.06f, 5);   // arm 1
    mbCone(mb, -0.14f, 0.0f, 0.5f, 0.72f, 0.09f, 0.06f, 5);  // arm 2
}

void buildDeadTree(MeshBuilder* mb) {
    u32 start = mb->vertCount;
    mbCone(mb, 0, 0, 0.0f, 0.9f, 0.09f, 0.04f, 5);          // trunk
    mbCone(mb, 0.12f, 0.0f, 0.5f, 0.85f, 0.05f, 0.01f, 4);  // branch
    mbCone(mb, -0.1f, 0.1f, 0.6f, 0.95f, 0.04f, 0.01f, 4);
    mbColorSince(mb, start, kTrunkColor);  // whole dead tree is woody brown
}

void buildReed(MeshBuilder* mb) {
    mbBlades(mb, 3, 1.0f, 0.06f, 0.2f);
}

void buildShrub(MeshBuilder* mb) {
    mbBlob(mb, 0.0f, 0.45f, 0.0f, 0.6f, 0.6f, 51);
}

void buildRock(MeshBuilder* mb) {
    mbBlob(mb, 0.0f, 0.4f, 0.0f, 0.9f, 0.5f, 97);
    mbBlob(mb, 0.5f, 0.25f, 0.3f, 0.5f, 0.5f, 131);
}

void buildFlower(MeshBuilder* mb) {
    mbFlower(mb);
}

// ── Merge: one VBO + one IBO + a range per (species, variant) ─────────────

AzgaarPropMesh s_mesh;
bool s_meshBuilt = false;

const char* speciesKey(AzgaarPropSpecies s) {
    switch (s) {
        case AZGAAR_PROP_GRASS_TUFT:     return "grass";
        case AZGAAR_PROP_CONIFER:        return "conifer";
        case AZGAAR_PROP_CONIFER_FAR:    return "conifer_far";
        case AZGAAR_PROP_DECIDUOUS:      return "deciduous";
        case AZGAAR_PROP_DECIDUOUS_FAR:  return "deciduous_far";
        case AZGAAR_PROP_ACACIA:         return "acacia";
        case AZGAAR_PROP_PALM:           return "palm";
        case AZGAAR_PROP_CACTUS:         return "cactus";
        case AZGAAR_PROP_DEAD_TREE:      return "dead_tree";
        case AZGAAR_PROP_REED:           return "reed";
        case AZGAAR_PROP_SHRUB:          return "shrub";
        case AZGAAR_PROP_ROCK:           return "rock";
        case AZGAAR_PROP_FLOWER:         return "flower";
        default:                         return "?";
    }
}

void buildRangeInto(AzgaarPropMesh& mesh,
                    const MeshBuilder& b,
                    u32 species,
                    u32 variant,
                    u32& vOff,
                    u32& iOff) {
    AzgaarPropMeshRange r = {};
    r.species      = species;
    r.variant      = variant;
    r.vertexOffset = vOff;
    r.vertexCount  = b.vertCount;
    r.indexOffset  = iOff;
    r.indexCount   = b.idxCount;
    for (u32 c = 0; c < 3; c++) r.aabbMin[c] =  FLT_MAX;
    for (u32 c = 0; c < 3; c++) r.aabbMax[c] = -FLT_MAX;
    for (u32 i = 0; i < b.vertCount; i++) {
        const AzgaarPropVertex* p = &b.verts[i];
        for (u32 c = 0; c < 3; c++) {
            r.aabbMin[c] = fminf(r.aabbMin[c], p->position[c]);
            r.aabbMax[c] = fmaxf(r.aabbMax[c], p->position[c]);
        }
    }
    mesh.vertices.insert(mesh.vertices.end(), b.verts.data(), b.verts.data() + b.vertCount);
    for (u32 i = 0; i < b.idxCount; i++) mesh.indices.push_back(b.idx[i] + vOff);
    vOff += b.vertCount;
    iOff += b.idxCount;
    mesh.ranges.push_back(r);
}

void validateMesh(const AzgaarPropMesh& mesh, const char* where) {
    bool ok = true;
    for (const AzgaarPropMeshRange& r : mesh.ranges) {
        if (r.indexCount % 3 != 0 || r.indexCount == 0 || r.vertexCount == 0) {
            utils::warn("azgaarPropMesh %s: bad range %s/%u (vc=%u ic=%u)", where,
                    speciesKey((AzgaarPropSpecies)r.species), r.variant, r.vertexCount,
                    r.indexCount);
            ok = false;
            continue;
        }
        for (u32 i = 0; i < r.indexCount; i++) {
            u32 ix = mesh.indices[r.indexOffset + i];
            if (ix < r.vertexOffset || ix >= r.vertexOffset + r.vertexCount) {
                utils::warn("azgaarPropMesh %s: range %s/%u index %u out of vertex span "
                        "[%u, %u)", where,
                        speciesKey((AzgaarPropSpecies)r.species), r.variant, ix, r.vertexOffset,
                        r.vertexOffset + r.vertexCount);
                ok = false;
                break;
            }
        }
        // Unit height: base at y=0; the top is ~1 but a few builders
        // overshoot by design (deciduous sphere crown 1.185, flower head
        // 1.18, reed blade tips 1.04) — the scatter's `scale` is a target,
        // not a clamp. Nothing may go above 1.25 or below -0.5 (the rock
        // blob dips to -0.05 by design, nothing more).
        if (r.aabbMax[1] > 1.25f || r.aabbMin[1] < -0.5f) {
            utils::warn("azgaarPropMesh %s: range %s/%u breaks unit height (y [%.3f, %.3f])",
                    where, speciesKey((AzgaarPropSpecies)r.species), r.variant, r.aabbMin[1],
                    r.aabbMax[1]);
            ok = false;
        }
    }
    utils::info("azgaarPropMesh %s: %zu verts / %zu idx / %zu ranges, validation %s", where,
            mesh.vertices.size(), mesh.indices.size(), mesh.ranges.size(), ok ? "PASS" : "FAIL");
}

void buildMesh(void) {
    s_mesh = AzgaarPropMesh{};

    // The 12 non-grass vegetation builders (one variant row each). The
    // *_FAR rows are never scattered (no far-LOD double-instances) but stay
    // in the table for species-id parity with the scatter's 0..12 ids.
    using BuilderFn = void (*)(MeshBuilder*);
    static const BuilderFn builders[AZGAAR_PROP_COUNT] = {
            nullptr,          // grass (cards below)
            buildConifer,
            buildConiferFar,
            buildDeciduous,
            buildDeciduousFar,
            buildAcacia,
            buildPalm,
            buildCactus,
            buildDeadTree,
            buildReed,
            buildShrub,
            buildRock,
            buildFlower,
    };

    static const char* dumpEnv = nullptr;
    if (!dumpEnv) dumpEnv = getenv("ENGINE_AZGAAR_PROPS_MESH_DUMP");

    const u32 grassCount = azgaarPropsGrassVariantCount();
    u32 vOff = 0, iOff = 0;

    // Grass first (species 0): one crossed card per texture variant, in the
    // exact azgaarPropsGrassVariant order (== kGrassTexPaths order, which the
    // scatter's per-instance variant pick indexes into).
    {
        MeshBuilder b;
        for (u32 i = 0; i < grassCount; i++) {
            const AzgaarGrassVariantInfo* v = azgaarPropsGrassVariant(i);
            if (!v) continue;
            mbInit(&b, 256, 640);
            buildGrassCard(&b, v->aspect, v->bottomV);
            if (dumpEnv) {
                char path[96];
                snprintf(path, sizeof(path), "/tmp/azgaar_props_grass_%u.obj", i);
                propsDumpBuilder(path, &b);
            }
            buildRangeInto(s_mesh, b, AZGAAR_PROP_GRASS_TUFT, i, vOff, iOff);
        }
    }

    for (u32 s = 1; s < AZGAAR_PROP_COUNT; s++) {
        MeshBuilder b;
        mbInit(&b, 256, 640);
        builders[s](&b);
        if (dumpEnv) {
            char path[96];
            snprintf(path, sizeof(path), "/tmp/azgaar_props_%s.obj",
                    speciesKey((AzgaarPropSpecies)s));
            propsDumpBuilder(path, &b);
        }
        buildRangeInto(s_mesh, b, s, 0, vOff, iOff);
    }

    validateMesh(s_mesh, "build");
    s_meshBuilt = true;
}
}  // namespace

void azgaarPropMeshBuild(void) {
    buildMesh();
}

void azgaarPropMeshRelease(void) {
    s_mesh = AzgaarPropMesh{};
    s_meshBuilt = false;
}

const AzgaarPropMesh* azgaarPropMeshGet(void) {
    return s_meshBuilt ? &s_mesh : nullptr;
}

}  // namespace game
