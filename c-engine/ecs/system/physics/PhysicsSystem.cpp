#include "PhysicsSystem.h"
#include "Utils.h"

#include <zstd.h>

#include <cstring>
#include <string>
#include <vector>

namespace engine {
PhysicsSystem physicsSystem;

static char joltActive = 0;

char physicsSystemJoltActive(void) {
    return joltActive;
}

// ── Terrain sidecar (JBVH v2, see tools/jolt-shape-builder/README.md) ──────
// The terrain's physics representation: one static mesh body per chunk,
// pre-baked by scripts/blender-terrain.py's jolt-shape-builder (the blob is
// Jolt's own Shape::SaveBinaryState output, so restoring skips BVH
// construction). The chunk vertices are in absolute world coordinates and
// the chunk nodes carry no transforms, so every body goes in at the origin
// with identity rotation.
static std::string terrainSidecarPath;
static std::vector<void*> terrainBodies;

static void terrainBodiesDestroy(void) {
    for (void* body : terrainBodies) {
        joltBodyDestroy(static_cast<JoltBody*>(body));
    }
    terrainBodies.clear();
}

// pak entry -> raw bytes (pak assets may be zstd-compressed, sniffed by
// magic — same convention as the gltf model reader).
static bool sidecarReadRaw(const char* pakPath, std::vector<u8>& out, std::string& error) {
    utils::String data = utils::dataManagerRead(pakPath);
    if (!data.data) {
        error = std::string("cannot read ") + pakPath;
        return false;
    }
    static const u8 kZstdMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};
    if (data.size >= 4 && std::memcmp(data.data, kZstdMagic, 4) == 0) {
        const unsigned long long raw = ZSTD_getFrameContentSize(data.data, data.size);
        if (raw == ZSTD_CONTENTSIZE_ERROR || raw == ZSTD_CONTENTSIZE_UNKNOWN) {
            utils::stringDestroy(&data);
            error = std::string("bad zstd frame in ") + pakPath;
            return false;
        }
        out.resize((size_t)raw);
        const size_t written = ZSTD_decompress(out.data(), out.size(), data.data, data.size);
        utils::stringDestroy(&data);
        if (ZSTD_isError(written)) {
            error = std::string("zstd decompress failed: ") + ZSTD_getErrorName(written);
            return false;
        }
        out.resize(written);
        return true;
    }
    out.assign(reinterpret_cast<const u8*>(data.data),
            reinterpret_cast<const u8*>(data.data) + data.size);
    utils::stringDestroy(&data);
    return true;
}

static void terrainSidecarLoad(const char* pakPath) {
    terrainBodiesDestroy();

    std::vector<u8> raw;
    std::string error;
    if (!sidecarReadRaw(pakPath, raw, error)) {
        utils::warn("physics: %s", error.c_str());
        return;
    }
    if (raw.size() < 12 || std::memcmp(raw.data(), "JBVH", 4) != 0) {
        utils::warn("physics: invalid JBVH sidecar %s", pakPath);
        return;
    }
    u32 version = 0, entryCount = 0;
    memcpy(&version, raw.data() + 4, 4);
    memcpy(&entryCount, raw.data() + 8, 4);
    if (version != 2) {
        utils::warn("physics: unsupported JBVH sidecar version %u (%s)", version, pakPath);
        return;
    }
    size_t off = 12;
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float rot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (u32 e = 0; e < entryCount; e++) {
        if (off + 4 > raw.size()) {
            utils::warn("physics: truncated JBVH entry %u (%s)", e, pakPath);
            break;
        }
        u32 nameLen = 0;
        memcpy(&nameLen, raw.data() + off, 4);
        off += 4;
        if (off + nameLen > raw.size()) {
            utils::warn("physics: truncated JBVH entry name %u (%s)", e, pakPath);
            break;
        }
        const char* name = reinterpret_cast<const char*>(raw.data() + off);
        off += nameLen;
        if (off + 2 + 12 + 4 > raw.size()) {
            utils::warn("physics: truncated JBVH entry header %u (%s)", e, pakPath);
            break;
        }
        const u8 motionType = raw[off + 1];
        f32 mass = 0, friction = 0, restitution = 0;
        memcpy(&mass, raw.data() + off + 2, 4);
        memcpy(&friction, raw.data() + off + 6, 4);
        memcpy(&restitution, raw.data() + off + 10, 4);
        u32 blobSize = 0;
        memcpy(&blobSize, raw.data() + off + 14, 4);
        off += 18;
        if (off + blobSize > raw.size()) {
            utils::warn("physics: truncated JBVH blob %u (%s)", e, pakPath);
            break;
        }
        const void* blob = raw.data() + off;
        off += blobSize;

        void* body = joltCreateBodyFromShapeBlob(
                blob, blobSize,
                motionType == 1 ? JOLT_MOTION_DYNAMIC : JOLT_MOTION_STATIC,
                mass, friction, restitution, nullptr, pos, rot, JOLT_TERRAIN_USER_DATA);
        if (!body) {
            utils::warn("physics: JBVH restore failed for %.*s", (int)nameLen, name);
            continue;
        }
        terrainBodies.push_back(body);
    }
    utils::info("physics: %zu terrain bodies from %s (%zu JBVH entries)", terrainBodies.size(),
            pakPath, (size_t)entryCount);
}

void physicsTerrainSidecarSet(const char* pakPath) {
    if (!pakPath || !pakPath[0]) {
        return;
    }
    terrainSidecarPath = pakPath;
    if (joltActive) {
        terrainSidecarLoad(pakPath);
        terrainSidecarPath.clear();
    }
}

PhysicsSystem::PhysicsSystem() : System("physics") {}

void PhysicsSystem::added() {
    joltInit();
    joltActive = 1;
    if (!terrainSidecarPath.empty()) {
        terrainSidecarLoad(terrainSidecarPath.c_str());
        terrainSidecarPath.clear();
    }
    utils::info("physics: Jolt world up");
}

void PhysicsSystem::removed() {
    // ecsDestroy snapshots the system list, so removed() may run twice;
    // joltDestroy must not run twice (it does not null-check).
    if (!joltActive) return;
    joltActive = 0;
    // Terrain bodies out first — joltBodyDestroy drops them from the world
    // while it is still alive, then the world goes with the system.
    terrainBodiesDestroy();
    joltDestroy();
    utils::info("physics: Jolt world down");
}

void PhysicsSystem::update() {
    joltUpdate(0.02f);
}
}
