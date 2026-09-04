#include "renderer/filament/PropsRenderFilament.h"

#include "Utils.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "image/Image.h"
#include "logger/Logger.h"
#include "renderer/Renderer.h"
#include "renderer/filament/FilamentRenderer.h"
#include "timer/Timer.h"

#include <backend/PixelBufferDescriptor.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/VertexBuffer.h>
#include <math/mat3.h>
#include <math/quat.h>
#include <math/vec4.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/*
 * Filament half of the props (vegetation) pass (see PropsRender.h and
 * plans/azgaar-terrain.md phase 7).
 *
 * Filament has no arbitrary per-instance attributes (InstanceBuffer carries
 * mat4 transforms for at most 64 instances), so per-instance data goes
 * through a per-tile instance-data texture: each instance occupies 3
 * RGBA32F texels, fetched in the material's vertex stage via
 * getInstanceIndex() (material `instanced : true` + Builder::instances(n),
 * max 32767 per draw — long ranges are chunked). One MaterialInstance per
 * (tile, range, chunk) carries the range's uniforms (instance offset, mesh
 * sub-range bounds + sway, look flags, base texture); the merged mesh
 * VBO/IBO is shared by every draw.
 *
 * Per-tile state is keyed on the active HeightmapTerrain's (tileX, tileZ,
 * readyStamp) snapshot — the same eviction source as the terrain pass —
 * so props stream in/out with the terrain window.
 */

namespace engine {
using namespace renderer::filament_globals;

namespace {

constexpr u32 kInstanceTexWidth = 1024;  // must match the material's uv()
constexpr u32 kMaxInstancesPerDraw = 32767;  // RenderableManager::Builder::instances clamp
constexpr u32 kTilesPerFrame = 3;  // same fill budget as the terrain pass
constexpr u32 kDeferredDestroyFrames = 3;

// Shared merged mesh (one per world).
bool             meshDirty = false;
std::vector<float> meshVerts;  // 13 floats / vertex (52 B), kept alive: setBufferAt does NOT copy
std::vector<u32>  meshIdx;
filament::VertexBuffer* meshVbo = nullptr;
filament::IndexBuffer*  meshIbo = nullptr;
u32  meshVertCount = 0;
u32  meshIdxCount  = 0;

// Per-(species, variant) merged-mesh table (copied from the game).
struct VariantDef {
    u32 species = 0, variant = 0;
    u32 indexOffset = 0, indexCount = 0;
    float boundsMin[3] = {}, boundsMax[3] = {};
    float swayFactor = 0.0f;
    u32 flags = 0;
    std::string texPath;  // empty = procedural (no base texture)
    filament::Texture* tex = nullptr;  // lazy-resolved on first draw
    bool texResolved = false;
};
std::vector<VariantDef> variants;

// Queued per-tile scatter (copied at SetTile; applied by the frame update).
struct PendingTile {
    i32 tileX = 0, tileZ = 0;
    u64 readyStamp = 0;
    std::vector<PropsRenderInstance> instances;
    std::vector<PropsRenderRange> ranges;
};
std::vector<PendingTile> pending;

// Resident GPU state for one scattered tile.
struct GpuRange {
    utils::Entity entity{};
    filament::MaterialInstance* mi = nullptr;
    u32 instances = 0;  // chunk size (stats; <= kMaxInstancesPerDraw)
};
struct GpuTile {
    i32 tileX = 0, tileZ = 0;
    u64 readyStamp = 0;
    filament::Texture* instanceTex = nullptr;
    // Kept alive until the texture is destroyed: the driver may still be
    // reading the upload (setImage, like setBufferAt, does not copy).
    std::vector<float> texPixels;
    std::vector<GpuRange> ranges;
};
std::vector<GpuTile> tiles;

// GPU destruction deferred a few frames (in-flight command buffers may
// still reference the resources).
struct DeferredDestroy {
    filament::Texture* instanceTex = nullptr;
    std::vector<float> texPixels;
    std::vector<GpuRange> ranges;
    u32 framesLeft = kDeferredDestroyFrames;
};
std::vector<DeferredDestroy> deferred;

// Global wind (dirX, dirZ, speed, strength) + accumulated phase (rad).
float  windDirX = 0.70710678f, windDirZ = 0.70710678f;
float  windSpeed = 0.10f, windStrength = 0.15f;
double windPhase = 0.0;
bool   enabled = false;

// Rolling per-frame cost of the pass update (phase-7 acceptance):
// updateImpl total + the tile-apply (upload) portion, averaged over the
// last kPerfFrames frames (~2 s at 60 fps).
constexpr u32 kPerfFrames = 120;
u32    perfFrame  = 0;
double perfTotalMs[kPerfFrames] = {};
double perfApplyMs[kPerfFrames] = {};
double perfApplyFrameMs = 0.0;  // apply cost accumulated by the current frame

filament::Material*         material = nullptr;
filament::Texture*          fallbackTex = nullptr;  // 1x1 white (procedural ranges)
bool                        initialized = false;
HeightmapTerrain*           cachedHt = nullptr;

// ── Texture helpers ────────────────────────────────────────────────────────

filament::TextureSampler const samplerNearestClamp = {
        filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::NEAREST,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE};
// Grass card sampler: LINEAR magnification (smooth close-up edges) +
// NEAREST minification. The min filter must NOT be LINEAR: the cards are
// 1-level (.levels(1)) sparse alpha-CUTOUT SRGB8_A8 textures, and RADV
// (Mesa, observed on NAVI31 26.2.1) returns a corrupted OPAQUE alpha for
// LINEAR-minified samples of sRGB images — every mid-distance card then
// renders as a solid striped tinted rectangle because the fragment's
// 0.5-alpha discard never fires (docs/lessons.md "RADV" entry, A/B matrix
// in .pi/ledger/notes.md round 2). NEAREST minification of a 1-level image
// is exactly "level-0 only", which is the intended look.
filament::TextureSampler const samplerCardClamp = {
        filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE};

// Heap copy of a CPU pixel buffer handed to a PixelBufferDescriptor.
// setImage is ZERO-COPY: the descriptor only references the buffer and the
// driver reads it later, on the engine loop thread. The callback form of the
// descriptor (PixelBufferDescriptor::make with a release functor) is the only
// safe way to release the storage — freeing (or stack-allocating) it right
// after setImage makes the GPU copy whatever the heap block holds by then:
// garbage texture content, different every launch.
void* uploadCopy(const void* pixels, size_t bytes) {
    void* copy = malloc(bytes);
    if (copy) memcpy(copy, pixels, bytes);
    return copy;
}

// Load a pak PNG (grass card) as sRGB RGBA8, level 0 only. These are sparse
// alpha-CUTOUT textures (a tuft over a mostly-transparent backdrop), so they
// must NOT be mipmipped: the generated mips average the alpha, raising the
// transparent border above the fragment's hard 0.5 cutout and rendering the
// whole card as a solid rectangle at distance. Level-0 sampling is the correct
// look (matches the old engine's grass). The cards are small on screen, so the
// absence of a minification chain is not noticeable.
filament::Texture* loadGrassTexture(const char* path) {
    utils::Image img = utils::imageLoad(path);
    if (!img.data || img.depth != 1 || img.channels != 4 || img.width <= 0 || img.height <= 0) {
        utils::warn("propsRender: grass texture load failed: %s", path);
        utils::imageDestory(&img);
        return nullptr;
    }
    const size_t bytes = (size_t)img.width * (size_t)img.height * 4u;
    void* pixels = uploadCopy(img.data, bytes);
    utils::imageDestory(&img);
    if (!pixels) return nullptr;
    filament::Texture::Builder builder = filament::Texture::Builder()
            .width(img.width)
            .height(img.height)
            .levels(1)
            .format(filament::Texture::InternalFormat::SRGB8_A8)
            .sampler(filament::Texture::Sampler::SAMPLER_2D)
            .usage(filament::Texture::Usage::DEFAULT);
    filament::Texture* tex = builder.build(*engine);
    if (!tex) {
        free(pixels);
        return nullptr;
    }
    tex->setImage(*engine, 0,
            filament::Texture::PixelBufferDescriptor::make(
                    pixels, bytes,
                    filament::Texture::Format::RGBA,
                    filament::Texture::Type::UBYTE,
                    [](void* b, size_t) { free(b); }));
    return tex;
}

// ── Pass lifecycle ─────────────────────────────────────────────────────────

void initPass(void) {
    if (initialized || !engine) return;

    utils::String blob = utils::dataManagerRead("materials/props.filamat");
    material = filament::Material::Builder().package(blob.data, blob.size).build(*engine);
    utils::stringDestroy(&blob);
    if (!material) {
        utils::warn("propsRender: material build failed (materials/props.filamat)");
        return;
    }

    const u8 white[4] = {255, 255, 255, 255};
    fallbackTex = filament::Texture::Builder()
                          .width(1)
                          .height(1)
                          .levels(1)
                          .format(filament::Texture::InternalFormat::SRGB8_A8)
                          .sampler(filament::Texture::Sampler::SAMPLER_2D)
                          .usage(filament::Texture::Usage::DEFAULT)
                          .build(*engine);
    if (fallbackTex) {
        void* whiteCopy = uploadCopy(white, 4);
        if (whiteCopy) {
            fallbackTex->setImage(*engine, 0,
                    filament::Texture::PixelBufferDescriptor::make(
                            whiteCopy, 4,
                            filament::Texture::Format::RGBA,
                            filament::Texture::Type::UBYTE,
                            [](void* b, size_t) { free(b); }));
        }
    }

    initialized = true;
    utils::info("propsRender: filament pass initialized");
}

// Merged mesh VBO/IBO (rebuilt when the game re-sets the mesh). The VBO
// source storage is the state's own vector (kept alive until the next
// rebuild, which destroys the old VBO first — setBufferAt does not copy).
// The normal float4 slot is repacked into a tangent-frame quaternion (the
// lit shading model needs TANGENTS; the object transform is identity, so
// the decoded world normal IS the mesh normal — same TBN construction as
// the terrain pass).
bool buildMeshBuffers(void) {
    if (!meshDirty || !engine) return !meshDirty;
    meshDirty = false;

    if (meshVbo) {
        engine->destroy(meshVbo);
        meshVbo = nullptr;
    }
    if (meshIbo) {
        engine->destroy(meshIbo);
        meshIbo = nullptr;
    }
    meshVertCount = 0;
    meshIdxCount = 0;

    if (meshVerts.empty() || meshIdx.empty()) {
        return false;
    }

    // Repack the per-vertex normal into a TBN-frame quaternion in place.
    for (size_t i = 0; i < meshVerts.size(); i += 13u) {
        filament::math::float3 N(meshVerts[i + 3], meshVerts[i + 4], meshVerts[i + 5]);
        filament::math::float3 T =
                filament::math::float3{1.0f, 0.0f, 0.0f} - N * N.x;
        const float tLen2 = T.x * T.x + T.y * T.y + T.z * T.z;
        filament::math::float3 B =
                filament::math::float3{0.0f, 0.0f, 1.0f} - N * N.z - T * T.z;
        const float bLen2 = B.x * B.x + B.y * B.y + B.z * B.z;
        if (tLen2 < 1e-12f || bLen2 < 1e-12f) {
            // Exact axis-aligned normals (the ±X̂ / ±Ẑ equator verts of the
            // sphere/blob canopy) collapse one fixed-axis projection to the
            // zero vector: normalize(0) gives a NaN frame and NaN-lit faces.
            // Rebuild the tangent from a reference axis that can never be
            // parallel to N: this branch only fires for N ≈ ±X̂ or ±Ẑ
            // (tLen2<eps needs |Nx|≈1; bLen2 = 1 − Nz² − Tz² < eps needs
            // |Nz|≈1), so |N.y| < 0.9 always holds and Ŷ is safe; the X̂
            // arm keeps cross(R,N) nonzero for any future N ≈ ±Ŷ too.
            const bool vert = std::fabs(N.y) > 0.9f;
            const filament::math::float3 R(vert ? 1.0f : 0.0f,
                    vert ? 0.0f : 1.0f, 0.0f);
            const filament::math::float3 t = cross(R, N);
            T = t * (1.0f / std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z));
            B = cross(N, T);
        } else {
            T = T * (1.0f / std::sqrt(tLen2));
            B = B * (1.0f / std::sqrt(bLen2));
        }
        filament::math::quatf q =
                filament::math::mat3f::packTangentFrame(filament::math::mat3f(T, B, N),
                        sizeof(float));
        meshVerts[i + 3] = q.x;
        meshVerts[i + 4] = q.y;
        meshVerts[i + 5] = q.z;
        meshVerts[i + 6] = q.w;
    }

    meshVertCount = (u32)(meshVerts.size() / 13u);
    meshIdxCount  = (u32)meshIdx.size();

    meshVbo = filament::VertexBuffer::Builder()
                       .bufferCount(1)
                       .vertexCount(meshVertCount)
                       .attribute(filament::VertexAttribute::POSITION, 0,
                               filament::VertexBuffer::AttributeType::FLOAT3, 0, 52)
                       .attribute(filament::VertexAttribute::TANGENTS, 0,
                               filament::VertexBuffer::AttributeType::FLOAT4, 12, 52)
                       .attribute(filament::VertexAttribute::UV0, 0,
                               filament::VertexBuffer::AttributeType::FLOAT2, 28, 52)
                       .attribute(filament::VertexAttribute::CUSTOM0, 0,
                               filament::VertexBuffer::AttributeType::FLOAT4, 36, 52)
                       .build(*engine);
    if (!meshVbo) {
        utils::warn("propsRender: mesh VBO creation failed");
        return false;
    }
    meshVbo->setBufferAt(*engine, 0, filament::VertexBuffer::BufferDescriptor(
            meshVerts.data(), meshVerts.size() * sizeof(float),
            [](void*, size_t, void*) {}));  // storage lives in the pass state

    meshIbo = filament::IndexBuffer::Builder()
                       .indexCount(meshIdxCount)
                       .bufferType(filament::IndexBuffer::IndexType::UINT)
                       .build(*engine);
    if (!meshIbo) {
        utils::warn("propsRender: mesh IBO creation failed");
        engine->destroy(meshVbo);
        meshVbo = nullptr;
        return false;
    }
    meshIbo->setBuffer(*engine, filament::IndexBuffer::BufferDescriptor(
            meshIdx.data(), meshIdx.size() * sizeof(u32),
            [](void*, size_t, void*) {}));  // storage lives in the pass state

    utils::info("propsRender: merged mesh uploaded (%u verts, %u idx, %zu variants)",
            meshVertCount, meshIdxCount, variants.size());
    return true;
}

// ── Tile GPU state ─────────────────────────────────────────────────────────

VariantDef* findVariant(u32 species, u32 variant) {
    for (VariantDef& v : variants) {
        if (v.species == species && v.variant == variant) return &v;
    }
    return nullptr;
}

void resolveVariantTexture(VariantDef* v) {
    if (v->texResolved) return;
    v->texResolved = true;
    v->tex = nullptr;
    if (!v->texPath.empty()) {
        v->tex = loadGrassTexture(v->texPath.c_str());
        if (!v->tex) utils::warn("propsRender: no texture for %u/%u (%s); range falls back to white",
                v->species, v->variant, v->texPath.c_str());
    }
}

// Pack one instance into 3 RGBA32F texels (10 floats, 2 spare slots).
// Position is TILE-LOCAL: the renderable transform carries the tile origin
// (relative to the world anchor), so absolute f32 positions — which sit on
// the 3.9 mm f32 grid at 39 km — are never stored in the texture.
void packInstance(float* dst, const PropsRenderInstance& inst, double originX, double originZ) {
    dst[0]  = (float)(inst.pos[0] - originX);
    dst[1]  = inst.pos[1];
    dst[2]  = (float)(inst.pos[2] - originZ);
    dst[3]  = inst.yaw;
    dst[4]  = inst.scale;
    dst[5]  = inst.color[0];
    dst[6]  = inst.color[1];
    dst[7]  = inst.color[2];
    dst[8]  = inst.phase;
    dst[9]  = 0.0f;
    dst[10] = 0.0f;
    dst[11] = 0.0f;
}

// Build the full GPU state of one scattered tile: instance texture + one
// (MaterialInstance, renderable) per range chunk. Returns false on failure
// (the caller drops the tile and retries next frame).
bool buildTile(GpuTile& t, const PendingTile& p) {
    const u32 n = (u32)p.instances.size();
    if (n == 0 || !material || !meshVbo) return false;

    // Instance-data texture: 3 texels per instance, row-major over
    // width kInstanceTexWidth (the material's uv() must match). The pixel
    // buffer must cover the WHOLE width x height region (Filament's
    // setImage precondition), not just the used texels — the last row's
    // padding stays zero.
    const u32 texelCount = n * 3u;
    const u32 texH = (texelCount + kInstanceTexWidth - 1u) / kInstanceTexWidth;
    // Tile origin in exact (double) world metres.
    const double originX = (double)p.tileX * (double)cachedHt->tileSizeMeters;
    const double originZ = (double)p.tileZ * (double)cachedHt->tileSizeMeters;
    t.texPixels.assign((size_t)kInstanceTexWidth * texH * 4u, 0.0f);
    for (u32 i = 0; i < n; i++) {
        packInstance(&t.texPixels[(size_t)i * 12u], p.instances[i], originX, originZ);
    }

    t.instanceTex = filament::Texture::Builder()
                          .width(kInstanceTexWidth)
                          .height((int)texH)
                          .levels(1)
                          .format(filament::Texture::InternalFormat::RGBA32F)
                          .sampler(filament::Texture::Sampler::SAMPLER_2D)
                          .usage(filament::Texture::Usage::DEFAULT)
                          .build(*engine);
    if (!t.instanceTex) {
        utils::warn("propsRender: instance texture creation failed tile(%d,%d)", t.tileX, t.tileZ);
        return false;
    }
    t.instanceTex->setImage(*engine, 0,
            filament::backend::PixelBufferDescriptor(t.texPixels.data(),
                    t.texPixels.size() * sizeof(float),
                    filament::backend::PixelDataFormat::RGBA,
                    filament::backend::PixelDataType::FLOAT, nullptr));

    // One draw per (range, chunk): geometry = the range's merged-mesh
    // sub-range, instanced over its instance run.
    for (const PropsRenderRange& r : p.ranges) {
        if (r.count == 0 || r.start + r.count > n) continue;
        VariantDef* v = findVariant(r.species, r.variant);
        if (!v) {
            utils::warn("propsRender: no mesh variant for species %u variant %u tile(%d,%d)",
                    r.species, r.variant, t.tileX, t.tileZ);
            continue;
        }
        resolveVariantTexture(v);

        const u32 chunks = (r.count + kMaxInstancesPerDraw - 1u) / kMaxInstancesPerDraw;
        for (u32 c = 0; c < chunks; c++) {
            const u32 chunkCount = std::min(kMaxInstancesPerDraw, r.count - c * kMaxInstancesPerDraw);

            filament::MaterialInstance* mi = material->createInstance("props");
            if (!mi) {
                utils::warn("propsRender: material instance creation failed");
                return false;
            }
            mi->setParameter("instanceData", t.instanceTex, samplerNearestClamp);
            mi->setParameter("cardTex", v->tex ? v->tex : fallbackTex, samplerCardClamp);
            mi->setParameter("meshA", filament::math::float4(
                    v->boundsMin[0], v->boundsMin[1], v->boundsMin[2], v->swayFactor));
            mi->setParameter("meshB", filament::math::float4(
                    v->boundsMax[0], v->boundsMax[1], v->boundsMax[2], (float)v->flags));
            mi->setParameter("instOff", filament::math::float4(
                    (float)(r.start + c * kMaxInstancesPerDraw), 0.0f, 0.0f, 0.0f));
            mi->setParameter("instUV", filament::math::float4(
                    1.0f / (float)kInstanceTexWidth, 1.0f / (float)texH, 0.0f, 0.0f));

            // Thin double-sided vegetation (grass cards, reed blades, palm
            // fronds, flower heads): the built-in lit model flips the normal on
            // back faces (doubleSided), which would leave the back of each blade
            // with a down-facing normal (NdotL == 0) and render half of every
            // tuft near-black. Light both faces with the unflipped normal
            // instead — matches the old engine's per-species Nlight. Disable the
            // flip and keep culling NONE (both faces still render).
            if (v->flags & props_render_flags::DOUBLE_SIDED) {
                mi->setDoubleSided(false);
                mi->setCullingMode(filament::MaterialInstance::CullingMode::NONE);
            }

            utils::Entity entity = utils::EntityManager::get().create();
            // The range's AABB (inflated 1 m), TILE-LOCAL: the renderable
            // transform carries the tile origin, so Filament transforms the
            // local culling box (an absolute box here would be transformed a
            // second time and cull everything). Filament culls every
            // instance of the draw with this box, so the scatter-time
            // cull + re-scatter is the only per-instance work left.
            const float ox = (float)originX;
            const float oz = (float)originZ;
            filament::Box box;
            box.set({r.aabbMin[0] - ox - 1.0f, r.aabbMin[1] - 1.0f, r.aabbMin[2] - oz - 1.0f},
                    {r.aabbMax[0] - ox + 1.0f, r.aabbMax[1] + 1.0f, r.aabbMax[2] - oz + 1.0f});

            filament::RenderableManager::Builder(1)
                    .boundingBox(box)
                    .material(0, mi)
                    .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, meshVbo,
                            meshIbo, v->indexOffset, v->indexCount)
                    .instances(chunkCount)
                    .castShadows(false)
                    .receiveShadows(false)
                    .build(*engine, entity);
            scene->addEntity(entity);

            t.ranges.push_back({.entity = entity, .mi = mi, .instances = chunkCount});
        }
    }

    utils::info("propsRender: tile(%d,%d) stamp=%llu %u instances -> %zu draws", t.tileX, t.tileZ,
            (unsigned long long)t.readyStamp, n, t.ranges.size());
    return true;
}

void destroyTile(GpuTile& t) {
    for (GpuRange& r : t.ranges) {
        if (r.entity) scene->remove(r.entity);
    }
    deferred.push_back({.instanceTex = t.instanceTex,
            .texPixels = std::move(t.texPixels),
            .ranges = std::move(t.ranges),
            .framesLeft = kDeferredDestroyFrames});
    t = GpuTile{};
}

void destroyAllTiles(void) {
    for (GpuTile& t : tiles) {
        if (!t.instanceTex && t.ranges.empty()) continue;
        for (GpuRange& r : t.ranges) {
            if (r.entity) {
                scene->remove(r.entity);
                engine->getRenderableManager().destroy(r.entity);
                utils::EntityManager::get().destroy(r.entity);
            }
            if (r.mi) engine->destroy(r.mi);
        }
        if (t.instanceTex) engine->destroy(t.instanceTex);
    }
    tiles.clear();
    for (DeferredDestroy& d : deferred) {
        for (GpuRange& r : d.ranges) {
            if (r.entity) {
                engine->getRenderableManager().destroy(r.entity);
                utils::EntityManager::get().destroy(r.entity);
            }
            if (r.mi) engine->destroy(r.mi);
        }
        if (d.instanceTex) engine->destroy(d.instanceTex);
    }
    deferred.clear();
    pending.clear();
}

bool tileMatchesAnyView(const GpuTile* e, const HeightmapTileView* views, u32 count) {
    for (u32 j = 0; j < count; j++) {
        if (e->tileX == views[j].tileX && e->tileZ == views[j].tileZ &&
            e->readyStamp == views[j].readyStamp)
            return true;
    }
    return false;
}

// ── Frame entry ────────────────────────────────────────────────────────────

static void updateImplWork(void);

void updateImpl(void) {
    double t0 = utils::elapsedBegin();
    perfApplyFrameMs = 0.0;
    updateImplWork();
    const double ms = utils::elapsedEnd(t0);
    perfTotalMs[perfFrame % kPerfFrames] = ms;
    perfApplyMs[perfFrame % kPerfFrames] = perfApplyFrameMs;
    perfFrame++;
}

static void updateImplWork(void) {
    initPass();
    if (!initialized) return;

    // Tick deferred GPU destruction (must outlive in-flight command buffers).
    for (i32 i = (i32)deferred.size() - 1; i >= 0; i--) {
        if (deferred[(u32)i].framesLeft > 1) {
            deferred[(u32)i].framesLeft--;
            continue;
        }
        // Renderable before the material instance (the RI still references
        // it), matching the terrain pass' destruction order.
        for (GpuRange& r : deferred[(u32)i].ranges) {
            if (r.entity) {
                engine->getRenderableManager().destroy(r.entity);
                utils::EntityManager::get().destroy(r.entity);
            }
            if (r.mi) engine->destroy(r.mi);
        }
        if (deferred[(u32)i].instanceTex) engine->destroy(deferred[(u32)i].instanceTex);
        deferred[(u32)i] = deferred.back();
        deferred.pop_back();
    }

    // Wind phase (the material's wind.w is the raw strength; z = phase).
    // The 1.x bridge has no material-level parameters, so the wind uniform
    // is pushed to every live material instance (cheap: one float4 each).
    windPhase += utils::timer.dt * (double)windSpeed;

    if (!enabled) {
        if (!tiles.empty() || !pending.empty()) destroyAllTiles();
        return;
    }

    HeightmapTerrain* ht = heightmapTerrainGetActive();
    if (cachedHt && ht != cachedHt) {
        destroyAllTiles();
        cachedHt = nullptr;
    }
    if (!ht || !ht->initialized) return;
    cachedHt = ht;

    if (!buildMeshBuffers()) return;

    const u32 cap = ht->windowSize * ht->windowSize;
    if (tiles.size() < cap) tiles.resize(cap);

    std::vector<HeightmapTileView> views(cap);
    const u32 viewCount = heightmapTerrainSnapshotTiles(ht, views.data(), cap);
    if (viewCount == 0) return;

    // 1) Evict GPU tiles that left the window or were regenerated.
    for (GpuTile& t : tiles) {
        if (!t.instanceTex && t.ranges.empty()) continue;
        if (!tileMatchesAnyView(&t, views.data(), viewCount)) destroyTile(t);
    }

    // 2) Apply queued scatters, nearest to the camera first, budgeted.
    if (!pending.empty()) {
        float camPos[3] = {0.0f, 0.0f, 0.0f};
        float camFwd[3] = {0.0f, 0.0f, 0.0f};
        renderer::rendererCameraGet(camPos, camFwd);

        auto tileDist = [camPos, ht](const PendingTile& p) {
            float ox = (float)ht->tileSizeMeters * (float)p.tileX;
            float oz = (float)ht->tileSizeMeters * (float)p.tileZ;
            float size = ht->tileSizeMeters;
            float dx = (camPos[0] < ox)          ? ox - camPos[0]
                     : (camPos[0] > ox + size)   ? camPos[0] - (ox + size)
                                                 : 0.0f;
            float dz = (camPos[2] < oz)          ? oz - camPos[2]
                     : (camPos[2] > oz + size)   ? camPos[2] - (oz + size)
                                                 : 0.0f;
            return dx * dx + dz * dz;
        };
        std::stable_sort(pending.begin(), pending.end(),
                [&tileDist](const PendingTile& a, const PendingTile& b) {
                    return tileDist(a) < tileDist(b);
                });

        u32 budget = kTilesPerFrame;
        for (i32 i = (i32)pending.size() - 1; i >= 0 && budget > 0; i--) {
            const PendingTile& p = pending[(u32)i];
            // Stale: the terrain tile changed or left the window since the
            // game queued it — drop without building.
            bool resident = false;
            for (u32 j = 0; j < viewCount; j++) {
                if (views[j].tileX == p.tileX && views[j].tileZ == p.tileZ &&
                    views[j].readyStamp == p.readyStamp) {
                    resident = true;
                    break;
                }
            }
            if (!resident) {
                pending.erase(pending.begin() + (u32)i);
                continue;
            }
            // An equal (tile, stamp) already resident or queued: the queue
            // is fed by the bridge's buildSeq, so this is a duplicate.
            bool have = false;
            for (GpuTile& t : tiles) {
                if (t.instanceTex && t.tileX == p.tileX && t.tileZ == p.tileZ &&
                    t.readyStamp == p.readyStamp) {
                    have = true;
                    break;
                }
            }
            if (have) {
                pending.erase(pending.begin() + (u32)i);
                continue;
            }

            GpuTile* slot = nullptr;
            for (GpuTile& t : tiles) {
                if (!t.instanceTex && t.ranges.empty()) {
                    slot = &t;
                    break;
                }
            }
            if (!slot) break;  // pool exhausted (shouldn't happen: cap = window^2)

            slot->tileX = p.tileX;
            slot->tileZ = p.tileZ;
            slot->readyStamp = p.readyStamp;
            const double apply0 = utils::elapsedBegin();
            const bool    built = buildTile(*slot, p);
            perfApplyFrameMs += utils::elapsedEnd(apply0);
            if (!built) {
                *slot = GpuTile{};
                break;  // out of GPU resources; retry next frame
            }
            pending.erase(pending.begin() + (u32)i);
            budget--;
        }
    }

    // Push the wind phase to every live instance (also the ones just
    // built this frame, which haven't seen it yet).
    filament::math::float4 wind(windDirX, windDirZ, (float)windPhase, windStrength);
    for (const GpuTile& t : tiles) {
        for (const GpuRange& r : t.ranges) {
            if (r.mi) r.mi->setParameter("wind", wind);
        }
    }

    // 3) Re-anchor every resident tile to this frame's world anchor (same
    // transform the terrain tiles get — the instance data is tile-local,
    // the transform carries the tile origin relative to the anchor; the
    // vertex stage needs the same value as the tileRel parameter).
    if (cachedHt) {
        const double ax   = renderer::rendererWorldAnchorX();
        const double az   = renderer::rendererWorldAnchorZ();
        const double tile = (double)cachedHt->tileSizeMeters;
        filament::TransformManager& tcm = engine->getTransformManager();
        for (const GpuTile& t : tiles) {
            if (!t.instanceTex) continue;
            const float relX = (float)((double)t.tileX * tile - ax);
            const float relZ = (float)((double)t.tileZ * tile - az);
            for (const GpuRange& r : t.ranges) {
                tcm.setTransform(tcm.getInstance(r.entity),
                        filament::math::mat4f::translation(filament::math::float3{relX, 0.0f, relZ}));
                if (r.mi) r.mi->setParameter("tileRel",
                        filament::math::float4{relX, 0.0f, relZ, 0.0f});
            }
        }
    }
}

void destroyImpl(void) {
    if (!engine) return;

    destroyAllTiles();

    for (VariantDef& v : variants) {
        if (v.tex) {
            engine->destroy(v.tex);
            v.tex = nullptr;
        }
        v.texResolved = false;
    }
    variants.clear();

    meshDirty = false;
    meshVerts.clear();
    meshIdx.clear();
    if (meshVbo) {
        engine->destroy(meshVbo);
        meshVbo = nullptr;
    }
    if (meshIbo) {
        engine->destroy(meshIbo);
        meshIbo = nullptr;
    }
    meshVertCount = 0;
    meshIdxCount = 0;

    if (fallbackTex) {
        engine->destroy(fallbackTex);
        fallbackTex = nullptr;
    }
    if (material) {
        engine->destroy(material);
        material = nullptr;
    }
    initialized = false;
    cachedHt = nullptr;
    enabled = false;
}
}  // namespace

// Public entry points (see PropsRenderFilament.h).

void propsRenderFilamentSetMesh(const float* verts, u32 vertCount, const u32* idx, u32 idxCount) {
    meshVerts.assign(verts, verts + (size_t)vertCount * 13u);
    meshIdx.assign(idx, idx + (size_t)idxCount);
    meshDirty = true;
}

void propsRenderFilamentSetVariants(const PropsRenderMeshVariant* v, u32 count) {
    for (VariantDef& od : variants) {
        if (od.tex && engine) engine->destroy(od.tex);
    }
    variants.assign(count, VariantDef{});
    for (u32 i = 0; i < count; i++) {
        VariantDef& d = variants[i];
        d.species     = v[i].species;
        d.variant     = v[i].variant;
        d.indexOffset = v[i].indexOffset;
        d.indexCount  = v[i].indexCount;
        memcpy(d.boundsMin, v[i].boundsMin, sizeof(d.boundsMin));
        memcpy(d.boundsMax, v[i].boundsMax, sizeof(d.boundsMax));
        d.swayFactor  = v[i].swayFactor;
        d.flags       = v[i].flags;
        d.texPath     = v[i].texturePath ? v[i].texturePath : "";
    }
}

void propsRenderFilamentSetTile(i32 tileX, i32 tileZ, u64 readyStamp,
                                const PropsRenderInstance* instances, u32 instanceCount,
                                const PropsRenderRange* ranges, u32 rangeCount) {
    PendingTile p;
    p.tileX      = tileX;
    p.tileZ      = tileZ;
    p.readyStamp = readyStamp;
    if (instances && instanceCount > 0) {
        p.instances.assign(instances, instances + (size_t)instanceCount);
    }
    if (ranges && rangeCount > 0) {
        p.ranges.assign(ranges, ranges + (size_t)rangeCount);
    }
    // Replace a pending entry for the same (tile, stamp) (re-scatter).
    for (auto& q : pending) {
        if (q.tileX == tileX && q.tileZ == tileZ && q.readyStamp == readyStamp) {
            q = std::move(p);
            return;
        }
    }
    pending.push_back(std::move(p));
}

void propsRenderFilamentClearAll(void) {
    destroyAllTiles();
}

void propsRenderFilamentSetWind(float dirX, float dirZ, float speed, float strength) {
    windDirX     = dirX;
    windDirZ     = dirZ;
    windSpeed    = speed;
    windStrength = strength;
}

void propsRenderFilamentSetEnabled(bool e) {
    enabled = e;
}

void propsRenderFilamentUpdate(void) {
    updateImpl();
}

void propsRenderFilamentStats(PropsRenderStats* out) {
    if (!out) return;
    *out = PropsRenderStats{};

    const u32 n = perfFrame < kPerfFrames ? perfFrame : kPerfFrames;
    double total = 0.0, apply = 0.0;
    for (u32 i = 0; i < n; i++) {
        total += perfTotalMs[i];
        apply += perfApplyMs[i];
    }
    out->frame       = perfFrame;
    out->renderAvgMs = n ? total / n : 0.0;
    out->applyAvgMs  = n ? apply / n : 0.0;
    out->pending     = (u32)pending.size();

    // GPU footprint, analytic (Filament exposes no allocator query): the
    // RGBA32F instance textures are width*height*16 B exactly, the grass
    // cards are 1-LEVEL SRGB8_A8 (width*height*4 B), the mesh is the
    // 52 B-vertex VBO + u32 IBO.
    auto accountInstanceTex = [&](const filament::Texture* tex, const std::vector<float>& staging) {
        if (!tex) return;
        out->gpuBytes += (size_t)tex->getWidth() * (size_t)tex->getHeight() * 16u;
        out->cpuStagingBytes += staging.size() * sizeof(float);
    };
    for (const GpuTile& t : tiles) {
        if (!t.instanceTex && t.ranges.empty()) continue;
        out->gpuTiles++;
        accountInstanceTex(t.instanceTex, t.texPixels);
        for (const GpuRange& r : t.ranges) {
            out->gpuDraws++;
            out->gpuInstances += r.instances;
        }
    }
    // Deferred destruction is still allocated GPU memory until released.
    for (const DeferredDestroy& d : deferred) {
        accountInstanceTex(d.instanceTex, d.texPixels);
        out->gpuDraws += (u32)d.ranges.size();
    }
    if (meshVbo) out->gpuBytes += (size_t)meshVertCount * 52u;
    if (meshIbo) out->gpuBytes += (size_t)meshIdxCount * sizeof(u32);
    for (const VariantDef& v : variants) {
        if (v.tex)
            out->gpuBytes += (size_t)v.tex->getWidth() * v.tex->getHeight() * 4u;
    }
}

void propsRenderFilamentDestroy(void) {
    destroyImpl();
}
}  // namespace engine
