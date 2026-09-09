#pragma once

#include "Utils.h"

#include <vector>

namespace treegen {

struct Vertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    float col[4];
};

struct Mesh {
    std::vector<Vertex> verts;
    std::vector<u32> idx;
    float aabbMin[3];
    float aabbMax[3];
    u32 triCount(void) const { return static_cast<u32>(idx.size() / 3); }
};

struct UvRect {
    float u0, v0, u1, v1;
};

constexpr UvRect kFullUvRect  = {0.0f, 0.0f, 1.0f, 1.0f};
constexpr UvRect kBarkUvRect  = {0.20f, 0.10f, 0.80f, 0.22f};
constexpr UvRect kLeafUvRect  = {0.02f, 0.34f, 0.98f, 0.99f};

enum class LeafStrategy { NONE = 0, BLOBS, CONES, FAN, CARDS };

struct LevelCfg {
    u32 children = 0;
    u32 sections = 3;
    u32 radialSegs = 4;
    float angleSpread = 0.7f;
    float length = 0.4f;
    float radius = 0.05f;
    float relRadius = -1.0f;
    float taper = 0.6f;
    float startFrac = -1.0f;
    float twist = -1.0f;
    float gnarliness = -1.0f;
    bool continuation = false;
};

struct Config {
    u32 levels = 2;
    LevelCfg lev[4];
    float startFrac = 0.55f;
    float twist = 0.5f;
    float gnarliness = 0.12f;
    float lift = 0.4f;
    float upBias = 0.35f;
    float angleJitter = 0.3f;
    float baseRadius = 0.06f;
    float baseLength = 0.5f;
    float trunkColor[3] = {0.36f, 0.25f, 0.16f};
    LeafStrategy leaf = LeafStrategy::NONE;
    u32 leafSeg = 3;
    u32 leafRing = 2;
    float leafRMin = 0.15f;
    float leafRMax = 0.3f;
    float leafFlat = 0.8f;
    u32 blobsPerTip = 2;
    u32 cardCountMin = 5;
    u32 cardCountMax = 6;
    float cardSize = 0.068f;
    float cardVariance = 0.6f;
    float cardTilt = 0.9599f;
    float cardStartFrac = 0.0f;
    float tipConeRadius = 0.2f;
    float tipConeHeight = 0.16f;
    u32 coneCountMin = 3;
    u32 coneCountMax = 4;
    float fanClampY = 0.9f;
    u32 maxTris = 400;
};

Config configConifer(void);
Config configDeciduous(void);
Config configAcacia(void);
Config configDeadTree(void);
Config configShrub(void);

Mesh generate(const Config& cfg, u32 seed, float maxRadius);

}
