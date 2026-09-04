#include "renderer/filament/HeightmapTerrainFilament.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "ecs/system/heightmap/HeightmapLattice.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "logger/Logger.h"
#include "renderer/Renderer.h"
#include "renderer/filament/FilamentRenderer.h"

#include <backend/PixelBufferDescriptor.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/RenderableManager.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <ktxreader/Ktx2Reader.h>
#include <math/mat3.h>
#include <utils/EntityManager.h>

#include <cstdlib>
#include <vector>

/*
 * Filament half of the heightmap terrain pass (see HeightmapTerrainRender.h
 * and plans/azgaar-terrain.md phase 5).
 *
 * Per READY tile: the CPU lattice (256^2 world-space corners, see
 * HeightmapLattice) is repacked into (position, tangent-frame quaternion)
 * and uploaded as a VertexBuffer; one shared 255-segment IndexBuffer backs
 * every tile. One material (terrain.mat, compiled to
 * pak_1/materials/heightmap_terrain.filamat) does all the look work in its
 * fragment stage — the vertex stage is empty.
 *
 * Uploads are budgeted (kUploadsPerFrame) and nearest-to-camera first, so
 * the visible ring fills before the window edge (same policy as the old
 * engine's Vulkan pass).
 */

namespace engine {
using namespace renderer::filament_globals;

namespace {

constexpr u32 kUploadsPerFrame     = 3;
constexpr u32 kDeferredDestroyFrames = 3;

// Default terrain textures (engine pak, KTX2/BC7).
struct DefaultTexture {
    const char* path;
    const char* param;
    bool srgb;
};

const DefaultTexture kDefaultTextures[] = {
    {"images/terrain/grass_default/albedo.ktx2", "grassAlbedo", true},
    {"images/terrain/grass_default/normal.ktx2", "grassNormal", false},
    {"images/terrain/cliff_side_default/albedo.ktx2", "cliffAlbedo", true},
    {"images/terrain/cliff_side_default/normal.ktx2", "cliffNormal", false},
    {"images/terrain/snow_default/albedo.ktx2", "snowAlbedo", true},
    {"images/terrain/sand_default/albedo.ktx2", "sandAlbedo", true},
};

// Per-tile GPU state (main thread only).
struct GpuTile {
    bool          inUse = false;
    i32           tileX = 0, tileZ = 0;
    u64           readyStamp = 0;
    utils::Entity entity{};
    filament::VertexBuffer* vbo = nullptr;
};

// GPU destruction is deferred a few frames: in-flight command buffers may
// still reference the buffers.
struct DeferredDestroy {
    filament::VertexBuffer* vbo = nullptr;
    utils::Entity           entity{};
    u32                     framesLeft = kDeferredDestroyFrames;
};

filament::Material*         material = nullptr;
filament::MaterialInstance* materialInstance = nullptr;

static inline float fractD64(double x) { return (float)(x - std::floor(x)); }

filament::IndexBuffer*      latticeIbo = nullptr;
u32                         latticeIdxCount = 0;
filament::Texture*          fallbackTex = nullptr;   // 1x1 white (empty slots)
filament::Texture*          biomeColorTex = nullptr;
filament::Texture*          climateTex = nullptr;

std::vector<GpuTile>         gpuTiles;
std::vector<DeferredDestroy> deferred;
HeightmapTerrain*            cachedHt = nullptr;

HeightmapTerrainLook look = {};
bool                 lookRegistered = false;
std::vector<u8>      lookBiomePixels;
std::vector<u8>      lookClimatePixels;
float                debugView = 0.0f;

// ── Texture helpers ────────────────────────────────────────────────────────

// World-tiling terrain textures (grass/cliff/snow/sand). These are minified
// by orders of magnitude with distance — the grass albedo repeats every 3.4 m
// and is seen from kilometres away — so they MUST be mip-mapped and
// anisotropic; the KTX2 assets ship 11 levels for exactly this. With a plain
// LINEAR minifier the albedo aliases into high-frequency straw speckle (and
// its normal map into shading noise). Mirrors the old engine's SAMPLER_LINEAR
// (linear min/mag + mipmap LINEAR + anisotropy 16 + repeat).
const filament::TextureSampler makeTilingSampler() {
    filament::TextureSampler sampler{
            filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
            filament::TextureSampler::MagFilter::LINEAR,
            filament::TextureSampler::WrapMode::REPEAT};
    sampler.setAnisotropy(16.0f);
    return sampler;
}

filament::TextureSampler const samplerLinearClamp = {
        filament::TextureSampler::MinFilter::LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE};

filament::TextureSampler const samplerNearestClamp = {
        filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::NEAREST,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE};

// Load a pak KTX2 (UASTC supercompressed) and transcode+upload it as BC7.
// Uses Filament's own ktxreader (which bundles the matching BasisU
// transcoder). KTX-Software is deliberately NOT linked into the game
// anymore: its vendored BasisU (newer ABI) used to win the basist:: symbol
// overlap and made every UASTC transcode fail here (see c-game/CMakeLists.txt
// and docs/lessons.md 2026-09-04).
filament::Texture* loadKtx2(const char* path, bool srgb) {
    utils::String data = utils::dataManagerRead(path);
    if (!data.data || data.size == 0) {
        utils::warn("heightmapTerrain: KTX2 load failed: %s", path);
        utils::stringDestroy(&data);
        return nullptr;
    }

    ktxreader::Ktx2Reader reader(*engine, /*quiet=*/true);
    if (srgb) {
        reader.requestFormat(filament::Texture::InternalFormat::SRGB_ALPHA_BPTC_UNORM);
    } else {
        reader.requestFormat(filament::Texture::InternalFormat::RGBA_BPTC_UNORM);
    }
    filament::Texture* tex = reader.load(data.data, data.size,
            srgb ? ktxreader::Ktx2Reader::TransferFunction::sRGB
                 : ktxreader::Ktx2Reader::TransferFunction::LINEAR);
    utils::stringDestroy(&data);

    if (!tex) {
        utils::warn("heightmapTerrain: KTX2 transcode failed: %s", path);
    }
    return tex;
}

// Upload packed RGBA8 pixels (one mip level).
filament::Texture* createRgba8(const u8* pixels, u32 w, u32 h, bool srgb) {
    filament::Texture::Builder builder = filament::Texture::Builder()
            .width((int)w)
            .height((int)h)
            .levels(1)
            .sampler(filament::Texture::Sampler::SAMPLER_2D)
            .usage(filament::Texture::Usage::DEFAULT);
    if (srgb) {
        builder.format(filament::Texture::InternalFormat::SRGB8_A8);
    } else {
        builder.format(filament::Texture::InternalFormat::RGBA8);
    }
    filament::Texture* tex = builder.build(*engine);
    if (tex) {
        tex->setImage(*engine, 0,
                filament::backend::PixelBufferDescriptor(pixels, (size_t)w * (size_t)h * 4u,
                        filament::backend::PixelDataFormat::RGBA,
                        filament::backend::PixelDataType::UBYTE, nullptr));
    }
    return tex;
}

// ── Pass lifecycle ─────────────────────────────────────────────────────────

// Shared 255-segment lattice index buffer (one per engine, all tiles).
bool latticeIboEnsure(void) {
    if (latticeIbo) return true;

    latticeIdxCount = heightmapLatticeIndexCount();

    latticeIbo = filament::IndexBuffer::Builder()
                          .indexCount(latticeIdxCount)
                          .bufferType(filament::IndexBuffer::IndexType::UINT)
                          .build(*engine);
    if (!latticeIbo) {
        utils::warn("heightmapTerrain: lattice IBO creation failed");
        latticeIdxCount = 0;
        return false;
    }
    // The upload runs later on the engine's loop thread, so the source
    // storage must outlive this call: heap-allocate and free from the
    // onComplete callback.
    u32* idx = new u32[latticeIdxCount];
    heightmapLatticeBuildIndices(idx);
    latticeIbo->setBuffer(*engine, filament::IndexBuffer::BufferDescriptor(idx,
            latticeIdxCount * sizeof(u32),
            [](void* data, size_t, void*) { delete[] (u32*)data; }));
    return true;
}

void applyLookToInstance(void);

void initPass(void) {
    if (material || !engine) return;

    // Material (compiled with matc at build time, shipped in pak_1).
    utils::String blob = utils::dataManagerRead("materials/heightmap_terrain.filamat");
    material = filament::Material::Builder().package(blob.data, blob.size).build(*engine);
    utils::stringDestroy(&blob);
    if (!material) {
        utils::warn("heightmapTerrain: material build failed (materials/heightmap_terrain.filamat)");
        return;
    }
    materialInstance = material->createInstance("heightmapTerrain");

    // Default terrain textures (UASTC KTX2, transcoded to BC7 on load).
    for (const DefaultTexture& dt : kDefaultTextures) {
        filament::Texture* tex = loadKtx2(dt.path, dt.srgb);
        if (tex) {
            materialInstance->setParameter(dt.param, tex, makeTilingSampler());
        } else {
            utils::warn("heightmapTerrain: no default texture for %s (fallback white)", dt.param);
        }
    }

    // Fallback texture for the per-world slots before a world is loaded.
    const u8 white[4] = {255, 255, 255, 255};
    fallbackTex = createRgba8(white, 1, 1, false);

    if (!latticeIboEnsure()) {
        utils::warn("heightmapTerrain: pass init incomplete");
        return;
    }

    applyLookToInstance();

    // The game may have set a debug view before the first frame (the
    // material instance only exists from here on) — apply the cached mode.
    if (debugView != 0.0f) {
        materialInstance->setParameter("debugView", debugView);
    }

    utils::info("heightmapTerrain: filament pass initialized (material + default textures + shared IBO %u idx)",
            latticeIdxCount);
}

// ── Look ───────────────────────────────────────────────────────────────────

void applyLookToInstance(void) {
    if (!materialInstance) return;

    if (lookRegistered && lookBiomePixels.size() ==
            (size_t)look.biomeColorW * (size_t)look.biomeColorH * 4u) {
        if (!biomeColorTex) {
            biomeColorTex = createRgba8(lookBiomePixels.data(), look.biomeColorW, look.biomeColorH, true);
        }
        materialInstance->setParameter("biomeColor", biomeColorTex, samplerLinearClamp);
    } else {
        materialInstance->setParameter("biomeColor", fallbackTex, samplerLinearClamp);
    }

    if (lookRegistered && lookClimatePixels.size() ==
            (size_t)look.climateW * (size_t)look.climateH * 4u) {
        if (!climateTex) {
            climateTex = createRgba8(lookClimatePixels.data(), look.climateW, look.climateH, false);
        }
        // R/G/B (temperature/precip/coast) linear; A (biome id) nearest.
        materialInstance->setParameter("climate", climateTex, samplerLinearClamp);
        materialInstance->setParameter("climateNearest", climateTex, samplerNearestClamp);
    } else {
        materialInstance->setParameter("climate", fallbackTex, samplerLinearClamp);
        materialInstance->setParameter("climateNearest", fallbackTex, samplerNearestClamp);
    }

    materialInstance->setParameter("mapBounds",
            filament::math::float4(look.mapMinX, look.mapMinZ, look.mapMaxX, look.mapMaxZ));
    materialInstance->setParameter("climateParams",
            filament::math::float4(look.snowLoC, look.snowHiC, look.beachHeightM,
                    look.climateEnabled ? 1.0f : 0.0f));
    materialInstance->setParameter("maxLandHeight", look.maxLandHeightM);
}

void registerLookImpl(const HeightmapTerrainLook* lookPtr) {
    lookBiomePixels.clear();
    lookClimatePixels.clear();

    if (lookPtr && lookPtr->biomeColorPixels && lookPtr->climatePixels &&
        lookPtr->biomeColorW && lookPtr->biomeColorH && lookPtr->climateW && lookPtr->climateH) {
        look = *lookPtr;
        lookBiomePixels.assign(lookPtr->biomeColorPixels,
                lookPtr->biomeColorPixels + (size_t)lookPtr->biomeColorW * (size_t)lookPtr->biomeColorH * 4u);
        lookClimatePixels.assign(lookPtr->climatePixels,
                lookPtr->climatePixels + (size_t)lookPtr->climateW * (size_t)lookPtr->climateH * 4u);
        lookRegistered = true;
    } else {
        look = HeightmapTerrainLook{};
        lookRegistered = false;
    }

    if (materialInstance) {
        applyLookToInstance();
        utils::info("heightmapTerrain: look %s (climate %s, snow [%.1f, %.1f] C, beach %.1f m, "
                "maxLand %.0f m, map %.0fx%.0f m)",
                lookRegistered ? "registered" : "cleared",
                look.climateEnabled ? "on" : "off", look.snowLoC, look.snowHiC, look.beachHeightM,
                look.maxLandHeightM,
                look.mapMaxX - look.mapMinX, look.mapMaxZ - look.mapMinZ);
    }
}

void releaseLookImpl(void) {
    if (biomeColorTex) {
        engine->destroy(biomeColorTex);
        biomeColorTex = nullptr;
    }
    if (climateTex) {
        engine->destroy(climateTex);
        climateTex = nullptr;
    }
    lookBiomePixels.clear();
    lookClimatePixels.clear();
    lookRegistered = false;
    if (materialInstance) {
        applyLookToInstance();
    }
}

void setDebugViewImpl(u32 mode) {
    float v = (float)mode;
    if (v == debugView) return;
    debugView = v;
    if (materialInstance) {
        materialInstance->setParameter("debugView", v);
    }
}

// ── Tile cache ─────────────────────────────────────────────────────────────

void destroyTile(GpuTile* e) {
    if (!e->inUse) return;
    scene->remove(e->entity);
    deferred.push_back({.vbo = e->vbo, .entity = e->entity, .framesLeft = kDeferredDestroyFrames});
    *e = GpuTile{};
}

void destroyAllTiles(void) {
    for (GpuTile& t : gpuTiles) {
        if (!t.inUse) continue;
        scene->remove(t.entity);
        if (t.vbo) engine->destroy(t.vbo);
        if (t.entity) {
            engine->getRenderableManager().destroy(t.entity);
            utils::EntityManager::get().destroy(t.entity);
        }
    }
    gpuTiles.clear();
    for (DeferredDestroy& d : deferred) {
        if (d.vbo) engine->destroy(d.vbo);
        if (d.entity) {
            engine->getRenderableManager().destroy(d.entity);
            utils::EntityManager::get().destroy(d.entity);
        }
    }
    deferred.clear();
}

bool gpuTileHasView(const GpuTile* e, const HeightmapTileView* v) {
    return e->inUse && e->tileX == v->tileX && e->tileZ == v->tileZ && e->readyStamp == v->readyStamp;
}

bool gpuTileMatchesAnyView(const GpuTile* e, const HeightmapTileView* views, u32 count) {
    for (u32 j = 0; j < count; j++) {
        if (gpuTileHasView(e, &views[j])) return true;
    }
    return false;
}

GpuTile* gpuTileAcquireFree(void) {
    for (GpuTile& t : gpuTiles) {
        if (!t.inUse) return &t;
    }
    return nullptr;
}

// Upload one tile: CPU lattice -> (position, tangent-frame) VBO + renderable
// (shared IBO + shared material instance). The corners are repacked from the
// canonical (pos, normal) lattice into Filament's tangent-quaternion layout:
// the frame is the standard terrain TBN built from the normal (the fragment
// shader rebuilds the same frame for its normal maps).
bool uploadTile(GpuTile* e, const HeightmapTileView* v) {
    const u32 cornerCount = heightmapLatticeCornerCount();

    static std::vector<HeightmapLatticeCorner> cornerScratch;
    cornerScratch.resize(cornerCount);
    // Tile-LOCAL corners (0,0 origin): the renderable transform carries the
    // tile origin (relative to the world anchor, re-set every frame by the
    // re-anchor pass). Absolute f32 corners at 39 km would sit on the 3.9 mm
    // f32 grid; local corners + a small transform keep sub-mm precision.
    heightmapLatticeBuildCorners(v->heights, 0.0f, 0.0f, v->sizeMeters,
            cornerScratch.data());

    // Interleaved VBO layout: float3 pos @ 0, float4 tangent-frame @ 12 (stride 28).
    //
    // The storage is per upload (heap, freed by the BufferDescriptor callback):
    // setBufferAt does NOT copy, it hands the pointer to the driver, which
    // reads it when the command executes. A shared scratch buffer would be
    // overwritten by the next tile of this frame (kUploadsPerFrame > 1) before
    // that, giving every tile in the batch the last tile's corners. Same rule
    // as the shared IBO below.
    const size_t vboFloatCount = (size_t)cornerCount * 7u;
    float* vboData = new float[vboFloatCount];
    for (u32 i = 0; i < cornerCount; i++) {
        const HeightmapLatticeCorner& c = cornerScratch[i];
        // Same TBN construction as the fragment's buildTerrainTBN (the
        // lattice normal is never degenerate: ny > 0).
        filament::math::float3 N{c.normal[0], c.normal[1], c.normal[2]};
        filament::math::float3 T =
                filament::math::float3{1.0f, 0.0f, 0.0f} - N * N.x;
        T = T * (1.0f / std::sqrt(T.x * T.x + T.y * T.y + T.z * T.z));
        // dot(T, (0,0,1)) == T.z
        filament::math::float3 B =
                filament::math::float3{0.0f, 0.0f, 1.0f} - N * N.z - T * T.z;
        B = B * (1.0f / std::sqrt(B.x * B.x + B.y * B.y + B.z * B.z));

        filament::math::mat3f tbn(T, B, N);
        filament::math::quatf q =
                filament::math::mat3f::packTangentFrame(tbn, sizeof(float));

        float* dst = &vboData[(size_t)i * 7u];
        dst[0] = c.pos[0];
        dst[1] = c.pos[1];
        dst[2] = c.pos[2];
        dst[3] = q.x;
        dst[4] = q.y;
        dst[5] = q.z;
        dst[6] = q.w;
    }

    filament::VertexBuffer* vbo = filament::VertexBuffer::Builder()
                                     .bufferCount(1)
                                     .vertexCount(cornerCount)
                                     .attribute(filament::VertexAttribute::POSITION, 0,
                                             filament::VertexBuffer::AttributeType::FLOAT3, 0, 28)
                                     .attribute(filament::VertexAttribute::TANGENTS, 0,
                                             filament::VertexBuffer::AttributeType::FLOAT4, 12, 28)
                                     .build(*engine);
    if (!vbo) {
        utils::warn("heightmapTerrain: VBO creation failed tile(%d,%d)", v->tileX, v->tileZ);
        delete[] vboData;  // no BufferDescriptor took ownership
        return false;
    }
    vbo->setBufferAt(*engine, 0, filament::VertexBuffer::BufferDescriptor(vboData,
            vboFloatCount * sizeof(float),
            [](void* data, size_t, void*) { delete[] (float*)data; }));

    utils::Entity entity = utils::EntityManager::get().create();
    // Local-space culling box (object space == tile space: corners are
    // tile-local, the renderable transform carries the tile origin — see the
    // re-anchor pass). Conservative Y range (seabed .. above the tallest peak).
    const float minY = -128.0f;
    const float maxY = filament::math::max(look.maxLandHeightM + 128.0f, 256.0f);
    filament::Box box;
    box.set({0.0f, minY, 0.0f},
            {v->sizeMeters, maxY, v->sizeMeters});

    filament::RenderableManager::Builder(1)
            .boundingBox(box)
            .material(0, materialInstance)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, vbo, latticeIbo, 0,
                    latticeIdxCount)
            .castShadows(false)
            .receiveShadows(false)
            .build(*engine, entity);
    scene->addEntity(entity);

    e->inUse      = true;
    e->tileX      = v->tileX;
    e->tileZ      = v->tileZ;
    e->readyStamp = v->readyStamp;
    e->entity     = entity;
    e->vbo        = vbo;
    return true;
}

// ── Frame entry: cache maintenance + budgeted uploads ─────────────────────

// Per-frame cost tracking for the phase-5 acceptance log (reported through
// heightmapTerrainFilamentStats + HeightmapTerrainSystem::update). Skip a
// 120-frame warmup (initial tile ramp + builder settle), then hold the
// average over the next 1000 frames.
constexpr u32 kStatWarmupFrames = 120;
constexpr u32 kStatFrames       = 1000;
u64   statFrame   = 0;
double statSum    = 0.0;
u32   statCount   = 0;
double statAvgMs  = 0.0;

static void updateImplWork(void);

void updateImpl(void) {
    double t0 = utils::elapsedBegin();
    updateImplWork();
    double ms = utils::elapsedEnd(t0);

    ++statFrame;
    if (statFrame > kStatWarmupFrames && statCount < kStatFrames) {
        statSum += ms;
        statCount++;
        statAvgMs = statSum / (double)statCount;
    }
}

static void updateImplWork(void) {
    initPass();
    if (!materialInstance || !latticeIbo) return;

    // Tick deferred GPU destruction (must outlive in-flight command buffers).
    for (i32 i = (i32)deferred.size() - 1; i >= 0; i--) {
        if (deferred[i].framesLeft > 1) {
            deferred[i].framesLeft--;
            continue;
        }
        if (deferred[i].vbo) engine->destroy(deferred[i].vbo);
        if (deferred[i].entity) {
            engine->getRenderableManager().destroy(deferred[i].entity);
            utils::EntityManager::get().destroy(deferred[i].entity);
        }
        deferred[(u32)i] = deferred.back();
        deferred.pop_back();
    }

    HeightmapTerrain* ht = heightmapTerrainGetActive();
    if (cachedHt && ht != cachedHt) {
        destroyAllTiles();
        cachedHt = nullptr;
    }
    if (!ht || !ht->initialized) return;
    cachedHt = ht;

    const u32 cap = ht->windowSize * ht->windowSize;
    if (gpuTiles.size() < cap) gpuTiles.resize(cap);

    std::vector<HeightmapTileView> views(cap);
    const u32 viewCount = heightmapTerrainSnapshotTiles(ht, views.data(), cap);
    if (viewCount == 0) return;

    // 1) Drop cache entries whose tile left the window or was regenerated.
    for (u32 i = 0; i < gpuTiles.size(); i++) {
        if (gpuTiles[i].inUse && !gpuTileMatchesAnyView(&gpuTiles[i], views.data(), viewCount)) {
            destroyTile(&gpuTiles[i]);
        }
    }

    // 2) Upload tiles the cache is missing, nearest to the camera first so
    // the visible ring fills before the distant window edge.
    float camPos[3] = {0.0f, 0.0f, 0.0f};
    float camFwd[3] = {0.0f, 0.0f, 0.0f};
    renderer::rendererCameraGet(camPos, camFwd);
    const i32 anchorTileX = heightmapWorldToTileCoord(ht, camPos[0]);
    const i32 anchorTileZ = heightmapWorldToTileCoord(ht, camPos[2]);

    // Stable insertion sort by (Manhattan ring, view order); n <= 25.
    for (u32 i = 1; i < viewCount; i++) {
        HeightmapTileView key = views[i];
        i32 kdx = key.tileX - anchorTileX;
        if (kdx < 0) kdx = -kdx;
        i32 kdz = key.tileZ - anchorTileZ;
        if (kdz < 0) kdz = -kdz;
        i32 kx = kdx + kdz;
        i32 j  = (i32)i - 1;
        for (; j >= 0; j--) {
            i32 dx = views[j].tileX - anchorTileX;
            if (dx < 0) dx = -dx;
            i32 dz = views[j].tileZ - anchorTileZ;
            if (dz < 0) dz = -dz;
            if (dx + dz <= kx) break;
            views[j + 1] = views[j];
        }
        views[j + 1] = key;
    }

    u32 budget = kUploadsPerFrame;
    for (u32 j = 0; j < viewCount && budget > 0; j++) {
        bool have = false;
        for (u32 i = 0; i < gpuTiles.size(); i++) {
            if (gpuTileHasView(&gpuTiles[i], &views[j])) {
                have = true;
                break;
            }
        }
        if (have) continue;

        GpuTile* e = gpuTileAcquireFree();
        if (!e) break; // pool exhausted (shouldn't happen: cap = window^2)
        if (uploadTile(e, &views[j])) {
            budget--;
            utils::info("heightmapTerrain: uploaded tile(%d,%d) stamp=%llu", views[j].tileX,
                    views[j].tileZ, (unsigned long long)views[j].readyStamp);
        }
        // failed upload: retry next frame, do not consume budget
    }

    // 3) Re-anchor every resident tile to this frame's world anchor (the
    // camera eye's xz). The VBO corners are tile-local; the transform carries
    // the tile origin relative to the anchor — a small f32 value with sub-mm
    // precision, unlike absolute f32 at 39 km (the 3.9 mm grid that made the
    // ground shimmer). The material gets the anchor too so terrain.mat's
    // world-anchored fields (tiling/noise phases, map UV) stay world-locked.
    const double ax  = renderer::rendererWorldAnchorX();
    const double az  = renderer::rendererWorldAnchorZ();
    const double tile = (double)ht->tileSizeMeters;
    filament::TransformManager& tcm = engine->getTransformManager();
    for (u32 i = 0; i < gpuTiles.size(); i++) {
        if (!gpuTiles[i].inUse) continue;
        const double ox = (double)gpuTiles[i].tileX * tile;
        const double oz = (double)gpuTiles[i].tileZ * tile;
        tcm.setTransform(tcm.getInstance(gpuTiles[i].entity),
                filament::math::mat4f::translation(filament::math::float3{(float)(ox - ax), 0.0f,
                        (float)(oz - az)}));
    }
    if (materialInstance) {
        // World-anchored pattern phases (see terrain.mat): the grass tiling is
        // periodic, so an exact fract(anchor*freq) phase keeps it world-locked
        // with zero shimmer. Aperiodic noise fields use anchorOffset directly
        // (world xz reconstruction).
        const float grassTile = 2048.0f / 7000.0f;
        materialInstance->setParameter("anchorPhaseGrass",
                filament::math::float4{fractD64(ax * grassTile), fractD64(az * grassTile), 0.0f, 0.0f});
        materialInstance->setParameter("anchorOffset",
                filament::math::float4{(float)ax, (float)az, 0.0f, 0.0f});
    }
}

void destroyImpl(void) {
    if (!engine) return;

    destroyAllTiles();

    if (latticeIbo) {
        engine->destroy(latticeIbo);
        latticeIbo = nullptr;
        latticeIdxCount = 0;
    }
    if (biomeColorTex) {
        engine->destroy(biomeColorTex);
        biomeColorTex = nullptr;
    }
    if (climateTex) {
        engine->destroy(climateTex);
        climateTex = nullptr;
    }
    if (fallbackTex) {
        engine->destroy(fallbackTex);
        fallbackTex = nullptr;
    }
    if (materialInstance) {
        engine->destroy(materialInstance);
        materialInstance = nullptr;
    }
    if (material) {
        engine->destroy(material);
        material = nullptr;
    }

    lookBiomePixels.clear();
    lookClimatePixels.clear();
    lookRegistered = false;
    cachedHt = nullptr;
}
}  // namespace

// Public entry points (see HeightmapTerrainFilament.h).

void heightmapTerrainFilamentInit(void) {
    initPass();
}

void heightmapTerrainFilamentUpdate(void) {
    updateImpl();
}

void heightmapTerrainFilamentRegisterLook(const HeightmapTerrainLook* lookPtr) {
    registerLookImpl(lookPtr);
}

void heightmapTerrainFilamentReleaseLook(void) {
    releaseLookImpl();
}

void heightmapTerrainFilamentSetDebugView(u32 mode) {
    setDebugViewImpl(mode);
}

void heightmapTerrainFilamentStats(HeightmapTerrainRenderStats* out) {
    if (!out) return;
    out->renderAvgMs = (statCount > 0) ? statAvgMs : 0.0;
    u32    tiles = 0;
    size_t bytes = 0;
    for (const GpuTile& t : gpuTiles) {
        if (!t.inUse) continue;
        tiles++;
        bytes += (size_t)heightmapLatticeCornerCount() * 7u * sizeof(float);
    }
    bytes += (size_t)latticeIdxCount * sizeof(u32);
    out->gpuTiles = tiles;
    out->gpuBytes = bytes;
}

void heightmapTerrainFilamentDestroy(void) {
    destroyImpl();
}
}  // namespace engine
