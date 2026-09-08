#include "treegen/TreeGen.h"

#include <cfloat>
#include <math.h>

namespace treegen {
namespace {

constexpr float kTwoPi = 6.283185307179586f;

struct V3 {
    float x, y, z;
};

float vDot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

V3 vCross(const V3& a, const V3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

V3 vScale(const V3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

V3 vAdd(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

V3 vSub(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

V3 vLerp(const V3& a, const V3& b, float t) {
    return vAdd(a, vScale(vSub(b, a), t));
}

V3 vNorm(const V3& a) {
    float l = sqrtf(vDot(a, a));
    if (l < 1e-8f) return {0.0f, 1.0f, 0.0f};
    return vScale(a, 1.0f / l);
}

void vRotate(V3* v, const V3& axis, float ang) {
    float c = cosf(ang), s = sinf(ang);
    V3 d = vScale(axis, (1.0f - c) * vDot(*v, axis));
    V3 cr = vCross(axis, *v);
    *v = vAdd(vAdd(vScale(*v, c), d), vScale(cr, s));
}

V3 vOrthonormal(const V3& n) {
    V3 ref = fabsf(n.y) < 0.9f ? V3{1.0f, 0.0f, 0.0f} : V3{0.0f, 0.0f, 1.0f};
    return vNorm(vCross(n, ref));
}

struct Rng {
    u32 s;
    explicit Rng(u32 seed) : s(seed ? seed : 1u) {}
    float unit(void) {
        s += 0x6D2B79F5u;
        u32 t = s;
        t = (t ^ (t >> 15)) * (t | 1u);
        t = t + ((t ^ (t >> 7)) * (t | 61u));
        t ^= t >> 14;
        return static_cast<float>(t & 0x00FFFFFFu) / 16777216.0f;
    }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
};

u32 mixSeed(u32 a, u32 b) {
    u32 h = a * 0x8DA6B343u ^ (b + 1u) * 0x27D4EB2Fu;
    h     = (h ^ (h >> 15)) * 0x2C1B3C6Du;
    h     = (h ^ (h >> 12)) * 0x297A2D39u;
    return h ^ (h >> 15);
}

struct Gen {
    Mesh m;
    u32 triLimit;
    u32 tris;
    bool room(u32 n) const { return tris + n <= triLimit; }
    u32 vert(const V3& p, const V3& n, float u, float v, const float col[4]) {
        if (m.verts.size() >= 0x00FFFFFFu) return 0xFFFFFFFFu;
        u32 i = static_cast<u32>(m.verts.size());
        Vertex x = {};
        x.pos[0] = p.x;
        x.pos[1] = p.y;
        x.pos[2] = p.z;
        V3 nn = vNorm(n);
        x.nrm[0] = nn.x;
        x.nrm[1] = nn.y;
        x.nrm[2] = nn.z;
        x.uv[0]  = u;
        x.uv[1]  = v;
        x.col[0] = col[0];
        x.col[1] = col[1];
        x.col[2] = col[2];
        x.col[3] = col[3];
        m.verts.push_back(x);
        return i;
    }
    void tri(u32 a, u32 b, u32 c) {
        m.idx.push_back(a);
        m.idx.push_back(b);
        m.idx.push_back(c);
        tris++;
    }
};

void addBand(Gen& g,
             const V3& o,
             const V3& o2,
             const V3& axis,
             const V3& u,
             const V3& v,
             float r0,
             float r1,
             u32 sides,
             const float trunkCol[3],
             bool baseCap,
             bool topCap) {
    float len     = sqrtf(vDot(vSub(o2, o), vSub(o2, o)));
    float col[4]  = {trunkCol[0], trunkCol[1], trunkCol[2], 1.0f};
    u32 first     = static_cast<u32>(g.m.verts.size());
    for (u32 s = 0; s < sides; s++) {
        float a0 = kTwoPi * static_cast<float>(s) / static_cast<float>(sides);
        float a1 = kTwoPi * static_cast<float>(s + 1) / static_cast<float>(sides);
        float am = 0.5f * (a0 + a1);
        V3 w0 = vAdd(vScale(u, cosf(a0)), vScale(v, sinf(a0)));
        V3 w1 = vAdd(vScale(u, cosf(a1)), vScale(v, sinf(a1)));
        V3 w  = vAdd(vScale(u, cosf(am)), vScale(v, sinf(am)));
        V3 n  = vAdd(vScale(w, len), vScale(axis, r0 - r1));
        u32 b0 = g.vert(vAdd(o, vScale(w0, r0)), n, 0.0f, 0.0f, col);
        u32 b1 = g.vert(vAdd(o, vScale(w1, r0)), n, 0.0f, 0.0f, col);
        u32 t0 = g.vert(vAdd(o2, vScale(w0, r1)), n, 0.0f, 1.0f, col);
        u32 t1 = g.vert(vAdd(o2, vScale(w1, r1)), n, 0.0f, 1.0f, col);
        g.tri(b0, t1, b1);
        g.tri(b0, t0, t1);
    }
    if (topCap && r1 > 0.004f) {
        u32 c = g.vert(o2, axis, 0.5f, 1.0f, col);
        for (u32 s = 0; s < sides; s++) {
            u32 t0 = first + 4u * s + 2u;
            u32 t1 = first + 4u * s + 3u;
            g.tri(c, t1, t0);
        }
    }
    if (baseCap && r0 > 0.004f) {
        u32 c = g.vert(o, vScale(axis, -1.0f), 0.5f, 0.0f, col);
        for (u32 s = 0; s < sides; s++) {
            u32 b0 = first + 4u * s;
            u32 b1 = first + 4u * s + 1u;
            g.tri(c, b0, b1);
        }
    }
}

void addSphere(Gen& g, const V3& c, float r, float flat, u32 seg, u32 ring) {
    if (seg < 3) seg = 3;
    if (ring < 2) ring = 2;
    float ry    = r * flat;
    float col[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    u32 first   = static_cast<u32>(g.m.verts.size());
    for (u32 j = 0; j <= ring; j++) {
        float phi = static_cast<float>(j) / static_cast<float>(ring) * (float)M_PI;
        float y   = c.y + cosf(phi) * ry;
        float rad = r * sinf(phi);
        for (u32 i = 0; i <= seg; i++) {
            float theta = static_cast<float>(i) / static_cast<float>(seg) * kTwoPi;
            V3 p;
            p.x = c.x + cosf(theta) * rad;
            p.y = y;
            p.z = c.z + sinf(theta) * rad;
            V3 n;
            n.x = cosf(theta) * sinf(phi);
            n.y = cosf(phi) * flat;
            n.z = sinf(theta) * sinf(phi);
            g.vert(p, n,
                   static_cast<float>(i) / static_cast<float>(seg),
                   static_cast<float>(j) / static_cast<float>(ring), col);
        }
    }
    for (u32 j = 0; j < ring; j++) {
        for (u32 i = 0; i < seg; i++) {
            u32 a  = first + j * (seg + 1) + i;
            u32 b  = a + 1;
            u32 c2 = a + (seg + 1);
            u32 d  = c2 + 1;
            g.tri(a, b, d);
            g.tri(a, d, c2);
        }
    }
}

void addCone(Gen& g, const V3& base, float baseR, float topR, float height) {
    static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    V3 o  = base;
    V3 o2 = {base.x, base.y + height, base.z};
    V3 axis = {0.0f, 1.0f, 0.0f};
    V3 u    = {1.0f, 0.0f, 0.0f};
    V3 v    = {0.0f, 0.0f, 1.0f};
    addBand(g, o, o2, axis, u, v, baseR, topR, 4, white, false, true);
}

void emitBlobs(Gen& g, const Config& cfg, Rng& rng, const V3& tip, bool fan) {
    u32 n = cfg.blobsPerTip + (u32)(rng.unit() * 2.0f);
    if (n > 5) n = 5;
    for (u32 k = 0; k < n; k++) {
        float r    = rng.range(cfg.leafRMin, cfg.leafRMax);
        float flat = fan ? 0.4f : cfg.leafFlat;
        V3 p;
        p.x = tip.x + rng.range(-1.0f, 1.0f) * r * 0.5f;
        p.y = tip.y + rng.range(0.0f, 1.0f) * r * 0.4f;
        p.z = tip.z + rng.range(-1.0f, 1.0f) * r * 0.5f;
        if (fan) {
            float top = p.y + r * flat;
            if (top > cfg.fanClampY) p.y = cfg.fanClampY - r * flat;
        }
        u32 cost = cfg.leafSeg * cfg.leafRing * 2u;
        if (!g.room(cost)) return;
        addSphere(g, p, r, flat, cfg.leafSeg, cfg.leafRing);
    }
}

void emitCones(Gen& g, const Config& cfg, Rng& rng, const V3& tip) {
    u32 n = cfg.coneCountMin +
            (u32)(rng.unit() * (static_cast<float>(cfg.coneCountMax + 1u) -
                                static_cast<float>(cfg.coneCountMin)));
    float r0 = cfg.tipConeRadius * (0.8f + 0.4f * rng.unit());
    float h  = cfg.tipConeHeight * (0.8f + 0.4f * rng.unit());
    for (u32 i = 0; i < n; i++) {
        if (!g.room(12u)) return;
        float y  = tip.y + static_cast<float>(i) * h;
        float rr = r0 * (1.0f - 0.22f * static_cast<float>(i));
        V3 b;
        b.x = tip.x + rng.range(-0.02f, 0.02f);
        b.y = y;
        b.z = tip.z + rng.range(-0.02f, 0.02f);
        addCone(g, b, rr, 0.015f, h);
    }
}

struct Job {
    u32 level;
    u32 seed;
    V3 origin;
    V3 dir;
    float radius;
    float length;
};

void grow(const Config& cfg, Gen& g, std::vector<Job>& q, const Job& j) {
    Rng rng(j.seed);
    const LevelCfg& lv = cfg.lev[j.level];
    u32 nsec           = lv.sections < 1 ? 1 : lv.sections;
    if (nsec > 6) nsec = 6;
    u32 sides = lv.radialSegs < 3 ? 3 : lv.radialSegs;
    if (sides > 8) sides = 8;
    V3 dir = vNorm(j.dir);
    V3 u   = vOrthonormal(dir);
    V3 v   = vCross(u, dir);
    float len   = j.length * (1.0f + 0.15f * (2.0f * rng.unit() - 1.0f));
    V3 o         = j.origin;
    V3 origins[7];
    origins[0] = o;
    float twistSign = rng.unit() < 0.5f ? 1.0f : -1.0f;
    float rBase     = j.radius;
    for (u32 s = 0; s < nsec; s++) {
        float t0 = static_cast<float>(s) / static_cast<float>(nsec);
        float t1 = static_cast<float>(s + 1) / static_cast<float>(nsec);
        float rc = rBase * (1.0f - lv.taper * t1);
        float perturb = cfg.gnarliness * (0.4f + 0.6f * rng.unit()) /
                        fmaxf(0.15f, sqrtf(rc));
        if (perturb > 0.001f) {
            V3 axis = vAdd(vScale(u, cosf(kTwoPi * rng.unit())),
                           vScale(v, sinf(kTwoPi * rng.unit())));
            axis = vNorm(axis);
            vRotate(&dir, axis, perturb * (rng.unit() < 0.5f ? 1.0f : -1.0f));
        }
        float liftF = cfg.lift * 0.3f * (1.0f - dir.y);
        dir = vNorm(vAdd(dir, V3{0.0f, liftF, 0.0f}));
        V3 perp = vSub(u, vScale(dir, vDot(u, dir)));
        float pl = sqrtf(vDot(perp, perp));
        u = pl > 1e-5f ? vScale(perp, 1.0f / pl) : vOrthonormal(dir);
        v   = vCross(u, dir);
        float tw = twistSign * cfg.twist * (t1 - t0);
        vRotate(&u, dir, tw);
        vRotate(&v, dir, tw);
        float step = (len / static_cast<float>(nsec)) * (0.9f + 0.2f * rng.unit());
        V3 o2      = vAdd(o, vScale(dir, step));
        origins[s + 1] = o2;
        V3 bandAxis = vNorm(vSub(o2, o));
        float r0    = rBase * (1.0f - lv.taper * t0);
        u32 bandCost = 2u * sides + sides * (u32)((s + 1 == nsec) + (j.level == 0 && s == 0));
        if (!g.room(bandCost)) return;
        addBand(g, o, o2, bandAxis, u, v, r0, rc, sides, cfg.trunkColor,
                j.level == 0 && s == 0, s + 1 == nsec);
        o = o2;
    }

    if (j.level + 1 < cfg.levels) {
        const LevelCfg& clv = cfg.lev[j.level + 1];
        u32 want           = clv.children;
        if (want > 0) {
            u32 n = want + (u32)(rng.unit() * 2.0f);
            if (n > 7) n = 7;
            for (u32 c = 0; c < n; c++) {
                float spawnT = cfg.startFrac + (1.0f - cfg.startFrac) * rng.unit();
                float f      = spawnT * static_cast<float>(nsec);
                u32 s0       = static_cast<u32>(f);
                if (s0 >= nsec) s0 = nsec - 1;
                u32 s1       = s0 + 1 < nsec ? s0 + 1 : s0;
                float fr     = f - static_cast<float>(s0);
                V3 p = vLerp(origins[s0], origins[s1], fr);
                float az  = kTwoPi * (static_cast<float>(c) + 0.5f) /
                               static_cast<float>(n) +
                           (rng.unit() - 0.5f) * 1.2f;
                float ang  = clv.angleSpread * (0.7f + 0.6f * rng.unit());
                V3 w = vAdd(vScale(u, cosf(az)), vScale(v, sinf(az)));
                V3 cd = vAdd(vScale(dir, cosf(ang)), vScale(w, sinf(ang)));
                cd = vNorm(vAdd(cd, V3{0.0f, 0.35f * (1.0f - cd.y), 0.0f}));
                float rad  = clv.radius * (0.8f + 0.4f * rng.unit());
                float clen = clv.length * (0.8f + 0.4f * rng.unit());
                V3 cop = vAdd(p, vScale(w, rBase * (1.0f - lv.taper) * 0.5f));
                Job cj;
                cj.level  = j.level + 1;
                cj.seed   = mixSeed(j.seed, c * 31u + j.level);
                cj.origin = cop;
                cj.dir    = cd;
                cj.radius = rad;
                cj.length = clen;
                q.push_back(cj);
            }
        }
    } else {
        V3 tip = origins[nsec];
        if (cfg.leaf == LeafStrategy::BLOBS) emitBlobs(g, cfg, rng, tip, false);
        else if (cfg.leaf == LeafStrategy::FAN) emitBlobs(g, cfg, rng, tip, true);
        else if (cfg.leaf == LeafStrategy::CONES) emitCones(g, cfg, rng, tip);
    }
}

}

Config configConifer(void) {
    Config c;
    c.levels     = 2;
    c.lev[0].sections = 4;
    c.lev[0].radialSegs = 4;
    c.lev[0].taper  = 0.45f;
    c.lev[1].children    = 5;
    c.lev[1].sections    = 3;
    c.lev[1].radialSegs  = 3;
    c.lev[1].angleSpread = 0.9f;
    c.lev[1].length      = 0.2f;
    c.lev[1].radius      = 0.028f;
    c.lev[1].taper       = 0.7f;
    c.baseRadius = 0.055f;
    c.baseLength = 0.6f;
    c.startFrac  = 0.5f;
    c.twist      = 0.35f;
    c.gnarliness = 0.035f;
    c.lift       = 0.5f;
    c.leaf          = LeafStrategy::CONES;
    c.tipConeRadius = 0.26f;
    c.tipConeHeight = 0.1f;
    c.coneCountMin  = 3;
    c.coneCountMax  = 5;
    c.maxTris       = 400;
    return c;
}

Config configDeciduous(void) {
    Config c;
    c.levels     = 3;
    c.lev[0].sections = 4;
    c.lev[0].radialSegs = 4;
    c.lev[0].taper  = 0.4f;
    c.lev[1].children    = 4;
    c.lev[1].sections    = 4;
    c.lev[1].radialSegs  = 3;
    c.lev[1].angleSpread = 1.0f;
    c.lev[1].length      = 0.28f;
    c.lev[1].radius      = 0.026f;
    c.lev[1].taper       = 0.65f;
    c.lev[2].children    = 2;
    c.lev[2].sections    = 1;
    c.lev[2].radialSegs  = 3;
    c.lev[2].angleSpread = 0.9f;
    c.lev[2].length      = 0.16f;
    c.lev[2].radius      = 0.012f;
    c.lev[2].taper       = 0.9f;
    c.baseRadius = 0.05f;
    c.baseLength = 0.5f;
    c.startFrac  = 0.55f;
    c.twist      = 0.35f;
    c.gnarliness = 0.035f;
    c.lift       = 0.4f;
    c.leaf        = LeafStrategy::BLOBS;
    c.leafSeg     = 3;
    c.leafRing    = 2;
    c.leafRMin    = 0.2f;
    c.leafRMax    = 0.42f;
    c.leafFlat    = 0.85f;
    c.blobsPerTip = 3;
    c.maxTris     = 600;
    return c;
}

Config configAcacia(void) {
    Config c;
    c.levels     = 2;
    c.lev[0].sections = 4;
    c.lev[0].radialSegs = 4;
    c.lev[0].taper  = 0.5f;
    c.lev[1].children    = 4;
    c.lev[1].sections    = 3;
    c.lev[1].radialSegs  = 3;
    c.lev[1].angleSpread = 1.3f;
    c.lev[1].length      = 0.32f;
    c.lev[1].radius      = 0.02f;
    c.lev[1].taper       = 0.75f;
    c.baseRadius = 0.05f;
    c.baseLength = 0.55f;
    c.startFrac  = 0.6f;
    c.twist      = 0.3f;
    c.gnarliness = 0.035f;
    c.lift       = 0.3f;
    c.leaf       = LeafStrategy::FAN;
    c.leafSeg     = 3;
    c.leafRing    = 2;
    c.leafRMin    = 0.3f;
    c.leafRMax    = 0.5f;
    c.blobsPerTip = 2;
    c.fanClampY   = 0.9f;
    c.maxTris     = 400;
    return c;
}

Config configDeadTree(void) {
    Config c;
    c.levels     = 3;
    c.lev[0].sections = 4;
    c.lev[0].radialSegs = 4;
    c.lev[0].taper  = 0.5f;
    c.lev[1].children    = 5;
    c.lev[1].sections    = 3;
    c.lev[1].radialSegs  = 3;
    c.lev[1].angleSpread = 1.25f;
    c.lev[1].length      = 0.26f;
    c.lev[1].radius      = 0.024f;
    c.lev[1].taper       = 0.85f;
    c.lev[2].children    = 3;
    c.lev[2].sections    = 1;
    c.lev[2].radialSegs  = 3;
    c.lev[2].angleSpread = 1.1f;
    c.lev[2].length      = 0.18f;
    c.lev[2].radius      = 0.01f;
    c.lev[2].taper       = 0.95f;
    c.baseRadius = 0.06f;
    c.baseLength = 0.5f;
    c.startFrac  = 0.5f;
    c.twist      = 0.5f;
    c.gnarliness = 0.09f;
    c.lift       = 0.35f;
    c.leaf       = LeafStrategy::NONE;
    c.maxTris    = 300;
    return c;
}

Config configShrub(void) {
    Config c;
    c.levels     = 2;
    c.lev[0].sections = 1;
    c.lev[0].radialSegs = 4;
    c.lev[0].taper  = 0.4f;
    c.lev[1].children    = 5;
    c.lev[1].sections    = 1;
    c.lev[1].radialSegs  = 3;
    c.lev[1].angleSpread = 0.9f;
    c.lev[1].length      = 0.22f;
    c.lev[1].radius      = 0.018f;
    c.lev[1].taper       = 0.8f;
    c.baseRadius = 0.028f;
    c.baseLength = 0.1f;
    c.startFrac  = 0.6f;
    c.twist      = 0.35f;
    c.gnarliness = 0.04f;
    c.lift       = 0.25f;
    c.leaf        = LeafStrategy::BLOBS;
    c.leafSeg     = 3;
    c.leafRing    = 2;
    c.leafRMin    = 0.1f;
    c.leafRMax    = 0.2f;
    c.leafFlat    = 0.7f;
    c.blobsPerTip = 2;
    c.maxTris     = 200;
    return c;
}

Mesh generate(const Config& cfg, u32 seed, float maxRadius) {
    Gen g;
    g.triLimit = cfg.maxTris;
    g.tris     = 0;
    std::vector<Job> q;
    Job root;
    root.level   = 0;
    root.seed    = seed ? seed : 1u;
    root.origin  = {0.0f, 0.0f, 0.0f};
    root.dir     = {0.0f, 1.0f, 0.0f};
    root.radius  = cfg.baseRadius;
    root.length  = cfg.baseLength;
    q.push_back(root);
    while (!q.empty()) {
        Job j = q.back();
        q.pop_back();
        grow(cfg, g, q, j);
    }
    Mesh& m = g.m;
    if (m.verts.empty()) {
        m.aabbMin[0] = m.aabbMin[1] = m.aabbMin[2] = 0.0f;
        m.aabbMax[0] = m.aabbMax[1] = m.aabbMax[2] = 0.0f;
        return m;
    }
    float minY = FLT_MAX, maxY = -FLT_MAX;
    for (const Vertex& p : m.verts) {
        if (p.pos[1] < minY) minY = p.pos[1];
        if (p.pos[1] > maxY) maxY = p.pos[1];
    }
    float h = maxY - minY;
    if (h <= 1e-6f) h = 1e-6f;
    float s = 1.0f / h;
    for (Vertex& p : m.verts) {
        p.pos[0] *= s;
        p.pos[1] = (p.pos[1] - minY) * s;
        p.pos[2] *= s;
    }
    if (maxRadius > 0.001f) {
        float ext = 0.0f;
        for (const Vertex& p : m.verts)
            ext = fmaxf(ext, fmaxf(fabsf(p.pos[0]), fabsf(p.pos[2])));
        if (ext > maxRadius) {
            float k = maxRadius / ext;
            for (Vertex& p : m.verts) {
                p.pos[0] *= k;
                p.pos[2] *= k;
            }
        }
    }
    for (u32 c = 0; c < 3; c++) {
        m.aabbMin[c] = FLT_MAX;
        m.aabbMax[c] = -FLT_MAX;
    }
    for (const Vertex& p : m.verts) {
        for (u32 c = 0; c < 3; c++) {
            if (p.pos[c] < m.aabbMin[c]) m.aabbMin[c] = p.pos[c];
            if (p.pos[c] > m.aabbMax[c]) m.aabbMax[c] = p.pos[c];
        }
    }
    return m;
}

}
