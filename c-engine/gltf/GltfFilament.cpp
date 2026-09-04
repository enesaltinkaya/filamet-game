#include "gltf/GltfInternal.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "logger/Logger.h"
#include "renderer/filament/FilamentRenderer.h"

#include <filament/Box.h>
#include <filament/Engine.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/TransformManager.h>
#include <gltfio/Animator.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/FilamentInstance.h>
#include <gltfio/MaterialProvider.h>
#include <gltfio/ResourceLoader.h>
#include <gltfio/TextureProvider.h>
#include <math/mat4.h>
#include <utils/EntityManager.h>
#include <zstd.h>

#include <algorithm>
#include <vector>

#include "materials/uberarchive.h"

namespace engine::gltf {
using namespace filament::gltfio;

using engine::renderer::filament_globals::engine;
using engine::renderer::filament_globals::scene;

static FilamentAsset* asset = nullptr;
static AssetLoader* loader = nullptr;
static ResourceLoader* resourceLoader = nullptr;
static MaterialProvider* materialProvider = nullptr;
static TextureProvider* stbDecoder = nullptr;
static TextureProvider* ktx2Decoder = nullptr;
static FilamentInstance* instance = nullptr;
static Animator* animator = nullptr;

// ── Animation source (old engine's models/animations.dat) ──────────────────
// A second asset carrying the skeleton + all clips (no textures). It is NOT
// added to the scene: its Animator is the clip source, and its joint node
// transforms are copied onto the visible model's joints every frame.
static FilamentAsset* animAsset = nullptr;
static FilamentInstance* animInstance = nullptr;
static Animator* animAnimator = nullptr;

// Node sync (anim asset -> main). Both exports run through
// scripts/gltf-standardize.py, so the main and animation assets live in
// the exact same space (identity armature, metre-space joints, identical
// node layout) and a joint's animated local transform can be copied
// straight onto the main asset's joint. The main asset's mesh node and
// armature node are never animated, so they stay at rest.
struct SyncNode {
    filament::TransformManager::Instance mainInst;
    filament::TransformManager::Instance animInst;
};
static std::vector<SyncNode> s_syncNodes;
static bool s_haveSync = false;
static bool s_syncBones = false;  // playback is driven by the anim asset

// Mixamo-style exports keep the armature at a 0.01 scale (the rig is authored
// in centimetres; the armature's 90-degree X + 0.01 converts to glTF metres).
// In such a file the mesh's vertex data is already in metres while the node
// hierarchy carries the 0.01, so under standard glTF semantics the rendered
// model comes out 100x too small ("eve was 2cm"). We detect the topmost
// node's scale at load time and compensate with a uniform wrapper scale in
// gltfPlaceAtFacing (the bone matrices are relative, so the factor cancels
// in skinning and animation sync is unaffected).
static f32 s_compScale = 1.0f;

// Per-model playback state (the old engine's AnimationInstance, single-clip
// edition: one active clip + one crossfade source).
struct AnimPlayback {
    bool playing = false;
    u32 clip = 0;
    f32 time = 0.0f;
    f32 speed = 1.0f;
    bool loop = true;
    bool fading = false;
    u32 fromClip = 0;
    f32 fromTime = 0.0f;
    f32 fadeElapsed = 0.0f;
    f32 fadeDuration = 0.0f;
};
static AnimPlayback s_play = {};

// Decode (zstd or plain) <pakPath> and create the asset. The caller owns the
// result (loader->destroyAsset). Null on failure.
static FilamentAsset* loadAsset(const char* pakPath) {
    utils::String glb = utils::dataManagerRead(pakPath);

    // Models are zstd-compressed glbs (scripts/export-models.sh produces
    // <name>.zstd); a plain glb passes through untouched. createAsset copies the bytes, so a
    // temporary decompression buffer is safe.
    void* glbBytes = glb.data;
    u32 glbBytesSize = glb.size;
    void* zstdBuf = nullptr;
    const u8* d = (const u8*)glb.data;
    if (glb.size >= 4 && d[0] == 0x28 && d[1] == 0xB5 && d[2] == 0x2F && d[3] == 0xFD) {
        u64 outSize = ZSTD_getFrameContentSize(glb.data, glb.size);
        if (outSize == ZSTD_CONTENTSIZE_UNKNOWN || outSize == ZSTD_CONTENTSIZE_ERROR) {
            utils::warn("gltf: %s is not a valid zstd frame", pakPath);
            utils::stringDestroy(&glb);
            return nullptr;
        }
        zstdBuf = malloc((size_t)outSize);
        u64 decomp = ZSTD_decompress(zstdBuf, outSize, glb.data, glb.size);
        if (ZSTD_isError(decomp) || decomp != outSize) {
            utils::warn("gltf: zstd decompress failed for %s (%s)", pakPath,
                    ZSTD_getErrorName(decomp));
            free(zstdBuf);
            utils::stringDestroy(&glb);
            return nullptr;
        }
        free(glb.data);
        glb.data = nullptr;
        glbBytes = zstdBuf;
        glbBytesSize = (u32)decomp;
    }

    FilamentAsset* loaded = loader->createAsset((const u8*)glbBytes, glbBytesSize);
    if (!loaded) {
        utils::warn("gltf: createAsset failed for %s", pakPath);
        free(zstdBuf);
        utils::stringDestroy(&glb);
        return nullptr;
    }
    free(zstdBuf);

    if (!resourceLoader->loadResources(loaded)) {
        utils::warn("gltf: loadResources failed for %s", pakPath);
    }
    utils::stringDestroy(&glb);
    return loaded;
}

// Pair the anim asset's skin joints with the main asset's by skin index
// (both exports carry the same rig, so skin joint i is the same bone in
// both). Only skin joints are synced: the main asset's mesh node and
// armature node stay at rest (they are not skin joints, and the two
// assets' raw entity lists are not index-aligned).
static void buildJointSync(void) {
    s_syncNodes.clear();
    s_haveSync = false;
    if (!instance || !animInstance || !engine) {
        return;
    }
    if (instance->getSkinCount() == 0 || animInstance->getSkinCount() == 0) {
        return;
    }
    const size_t mj = instance->getJointCountAt(0);
    const size_t aj = animInstance->getJointCountAt(0);
    const size_t n = mj < aj ? mj : aj;
    if (mj != aj) {
        utils::warn("gltf: joint count mismatch for animation sync (main %zu, anim %zu) — syncing %zu",
                mj, aj, n);
    }
    filament::TransformManager& tcm = engine->getTransformManager();
    const utils::Entity* mjt = instance->getJointsAt(0);
    const utils::Entity* ajt = animInstance->getJointsAt(0);
    s_syncNodes.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        SyncNode sn;
        sn.mainInst = tcm.getInstance(mjt[i]);
        sn.animInst = tcm.getInstance(ajt[i]);
        s_syncNodes.push_back(sn);
    }
    s_haveSync = true;
    utils::info("gltf: animation sync ready — %zu joints",
            s_syncNodes.size());
}

// The Animator that owns the clips: the dedicated animation source when it
// carries clips, otherwise the loaded model's own.
static Animator* activeAnimator(void) {
    if (animAnimator && animAnimator->getAnimationCount() > 0) {
        return animAnimator;
    }
    if (animator && animator->getAnimationCount() > 0) {
        return animator;
    }
    return nullptr;
}

static u32 findAnimationByName(const char* name) {
    Animator* src = activeAnimator();
    if (!src) {
        return (u32)-1;
    }
    const size_t n = src->getAnimationCount();
    for (size_t i = 0; i < n; ++i) {
        const char* an = src->getAnimationName(i);
        if (an && utils::strequals(an, name)) {
            return (u32)i;
        }
    }
    return (u32)-1;
}

bool gltfInitFilament(void) {
    if (loader) {
        return true;  // idempotent: re-init after gltfDestroy re-creates the loader
    }

    if (!engine) {
        utils::warn("gltf: renderer not initialized");
        return false;
    }

    materialProvider = createUbershaderProvider(engine, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);
    if (!materialProvider) {
        utils::warn("gltf: ubershader provider failed");
        return false;
    }

    loader = AssetLoader::create({
            .engine = engine,
            .materials = materialProvider,
    });
    if (!loader) {
        utils::warn("gltf: AssetLoader::create failed");
        return false;
    }

    resourceLoader = new ResourceLoader({engine, nullptr, true});
    stbDecoder = createStbProvider(engine);
    ktx2Decoder = createKtx2Provider(engine);
    resourceLoader->addTextureProvider("image/png", stbDecoder);
    resourceLoader->addTextureProvider("image/jpeg", stbDecoder);
    resourceLoader->addTextureProvider("image/ktx2", ktx2Decoder);

    utils::info("gltf: initialized (draco %s, webp %s)", GLTFIO_DRACO_SUPPORTED ? "on" : "off",
            isWebpSupported() ? "on" : "off");
    return true;
}

bool gltfLoadFilament(const char* pakPath) {
    if (!loader) {
        utils::warn("gltf: not initialized");
        return false;
    }

    // Replace any previously loaded asset (re-entering the world loads fresh)
    if (asset) {
        scene->removeEntities(asset->getEntities(), asset->getEntityCount());
        loader->destroyAsset(asset);
        asset = nullptr;
        instance = nullptr;
        animator = nullptr;
    }
    s_play = {};
    s_syncBones = false;
    s_compScale = 1.0f;

    FilamentAsset* loaded = loadAsset(pakPath);
    if (!loaded) {
        return false;
    }
    instance = loaded->getInstance();
    animator = instance ? instance->getAnimator() : nullptr;
    asset = loaded;

    // Detect a centimetre-authored armature (topmost node scaled by ~0.01)
    // and remember the compensating wrapper scale (see s_compScale).
    s_compScale = 1.0f;
    if (instance) {
        filament::TransformManager& tcm = engine->getTransformManager();
        const utils::Entity root = instance->getRoot();
        const utils::Entity* ents = instance->getEntities();
        for (size_t i = 0; i < instance->getEntityCount(); ++i) {
            const auto inst = tcm.getInstance(ents[i]);
            if (tcm.getParent(inst) == root) {
                const filament::math::mat4f m = tcm.getTransform(inst);
                const f32 sx = m[0][0], sy = m[1][0], sz = m[2][0];
                const f32 s = sqrtf(sx * sx + sy * sy + sz * sz);
                if (s > 0.001f && s < 0.5f) {
                    s_compScale = 1.0f / s;
                    utils::info("gltf: %s — cm-authored armature (scale %.4f), wrapper comp scale %.1f",
                            pakPath, s, s_compScale);
                }
                break;
            }
        }
    }

    filament::Aabb box = loaded->getBoundingBox();
    utils::info("gltf: %s — %zu entities, %zu animations, bounds [%.2f %.2f %.2f]-[%.2f %.2f %.2f]",
            pakPath, loaded->getEntityCount(), animator ? animator->getAnimationCount() : 0, box.min.x, box.min.y,
            box.min.z, box.max.x, box.max.y, box.max.z);

    scene->addEntities(loaded->getEntities(), loaded->getEntityCount());
    buildJointSync();  // in case the animation source was loaded first
    return true;
}

bool gltfLoadAnimationsFilament(const char* pakPath) {
    if (!loader) {
        utils::warn("gltf: not initialized");
        return false;
    }

    // Replace any previous animation source.
    if (animAsset) {
        loader->destroyAsset(animAsset);
        animAsset = nullptr;
        animInstance = nullptr;
        animAnimator = nullptr;
    }
    s_play = {};
    s_syncBones = false;

    FilamentAsset* loaded = loadAsset(pakPath);
    if (!loaded) {
        return false;
    }
    // Deliberately NOT added to the scene: invisible clip source.
    animAsset = loaded;
    animInstance = loaded->getInstance();
    animAnimator = animInstance ? animInstance->getAnimator() : nullptr;
    utils::info("gltf: animation source %s — %zu clips", pakPath,
            animAnimator ? animAnimator->getAnimationCount() : 0);
    buildJointSync();
    return true;
}

u32 gltfAnimationCountFilament(void) {
    Animator* src = activeAnimator();
    return src ? (u32)src->getAnimationCount() : 0;
}

const char* gltfAnimationNameFilament(u32 index) {
    Animator* src = activeAnimator();
    if (!src || index >= src->getAnimationCount()) {
        return "";
    }
    return src->getAnimationName(index);
}

f32 gltfAnimationDurationFilament(u32 index) {
    Animator* src = activeAnimator();
    if (!src || index >= src->getAnimationCount()) {
        return 0.0f;
    }
    return src->getAnimationDuration(index);
}

bool gltfPlayAnimationFilament(const char* name, f32 speed, bool loop) {
    return gltfPlayAnimationBlendedFilament(name, speed, loop, 0.0f);
}

bool gltfPlayAnimationBlendedFilament(const char* name, f32 speed, bool loop, f32 blendSeconds) {
    Animator* src = activeAnimator();
    if (!src) {
        utils::warn("gltf: play '%s' — no animation source", name);
        return false;
    }
    const u32 idx = findAnimationByName(name);
    if (idx == (u32)-1) {
        utils::warn("gltf: animation '%s' not found", name);
        return false;
    }

    // Same clip already playing: just retune speed/loop, keep the clock.
    if (s_play.playing && !s_play.fading && s_play.clip == idx) {
        s_play.speed = speed;
        s_play.loop = loop;
        return true;
    }

    const bool hadActive = s_play.playing;
    s_play.fromClip   = s_play.clip;
    s_play.fromTime   = s_play.time;
    s_play.fadeDuration = (hadActive && blendSeconds > 0.0f) ? blendSeconds : 0.0f;
    s_play.fading     = s_play.fadeDuration > 0.0f;
    s_play.fadeElapsed = 0.0f;
    s_play.clip      = idx;
    s_play.time      = 0.0f;
    s_play.speed     = speed;
    s_play.loop      = loop;
    s_play.playing   = true;
    s_syncBones      = (src == animAnimator);
    return true;
}

void gltfStopAnimationFilament(void) {
    s_play.playing = false;
    s_play.fading  = false;
}

bool gltfPlaceAtFilament(f32 x, f32 y, f32 z) {
    return gltfPlaceAtFacingFilament(x, y, z, 0.0f);
}

bool gltfPlaceAtFacingFilament(f32 x, f32 y, f32 z, f32 yaw) {
    if (!asset || !instance || !engine) {
        return false;
    }
    // The asset AABB is in instance-local space (node hierarchy only); the
    // gltfio root transform is applied on top. Pivot the yaw on the local AABB
    // min corner (feet for character assets) so the character rotates in place:
    // M = T(spawn) * R(-yaw, +Y) * S(comp) * T(-aabbMin). R uses -yaw because
    // mat4f::rotation maps +Z to (-sin r, 0, cos r), and the model's forward
    // must land on (sin yaw, 0, cos yaw) — the old engine's convention.
    // S(comp) restores metre scale for cm-authored assets (s_compScale, 1.0
    // otherwise) — it sits inside the yaw pivot so the character turns in
    // place at its true size.
    filament::Aabb box = asset->getBoundingBox();
    const filament::math::float3 off{x - box.min.x, y - box.min.y, z - box.min.z};  // T(spawn) * T(-min)
    const filament::math::float3 up{0.0f, 1.0f, 0.0f};
    const filament::math::float3 minc{box.min.x, box.min.y, box.min.z};
    filament::math::mat4f m = filament::math::mat4f::translation(off)
            * filament::math::mat4f::rotation(-yaw, up)
            * filament::math::mat4f::scaling(filament::math::float3{s_compScale, s_compScale, s_compScale})
            * filament::math::mat4f::translation(minc);
    filament::TransformManager& tcm = engine->getTransformManager();
    tcm.setTransform(tcm.getInstance(instance->getRoot()), m);
    return true;
}

void gltfUpdateFilament(double elapsedSeconds) {
    if (asset) {
        loader->gc();
    }

    Animator* src = activeAnimator();
    if (!s_play.playing || !src) {
        return;
    }

    const f32 dt = (f32)elapsedSeconds;
    s_play.time += dt * s_play.speed;
    const float dur = src->getAnimationDuration(s_play.clip);
    if (dur > 0.0f) {
        if (s_play.loop) {
            while (s_play.time >= dur) s_play.time -= dur;
        } else if (s_play.time >= dur) {
            s_play.time = dur;  // hold the last frame (old engine: finished)
        }
    }

    // gltfio playback order: applyAnimation (current, weight 1) →
    // applyCrossFade (previous, weight 1-alpha) → updateBoneMatrices.
    src->applyAnimation(s_play.clip, s_play.time);
    if (s_play.fading) {
        s_play.fromTime += dt * s_play.speed;
        const float fromDur = src->getAnimationDuration(s_play.fromClip);
        if (s_play.loop && fromDur > 0.0f) {
            while (s_play.fromTime >= fromDur) s_play.fromTime -= fromDur;
        }
        s_play.fadeElapsed += dt;
        float alpha = s_play.fadeDuration > 0.0f ? s_play.fadeElapsed / s_play.fadeDuration : 1.0f;
        if (alpha > 1.0f) alpha = 1.0f;
        src->applyCrossFade(s_play.fromClip, s_play.fromTime, alpha);
        if (s_play.fadeElapsed >= s_play.fadeDuration) {
            s_play.fading = false;
        }
    }
    src->updateBoneMatrices();

    // Drive the visible model: both assets live in the same space (see the
    // SyncNode comment), so the anim asset's joint locals transfer directly.
    if (s_syncBones && engine && s_haveSync) {
        filament::TransformManager& tcm = engine->getTransformManager();
        if (const char* mode = getenv("GLTF_SYNC_MODE")) {
            if (strcmp(mode, "nocopy") == 0) {
                return;
            }
            if (strcmp(mode, "copyonly") == 0) {
                for (const SyncNode& sn : s_syncNodes) {
                    tcm.setTransform(sn.mainInst, tcm.getTransform(sn.animInst));
                }
                return;
            }
        }
        for (const SyncNode& sn : s_syncNodes) {
            tcm.setTransform(sn.mainInst, tcm.getTransform(sn.animInst));
        }
        if (animator) {
            animator->updateBoneMatrices();
        }
        if (const char* d = getenv("GLTF_DEBUG_SYNC")) {
            static int s_dbg = 0;
            if (s_dbg < 3) {
                s_dbg++;
                if (s_dbg == 2 && instance && engine) {
                    // Replicate gltfio's updateBoneMatrices formula for joint 0.
                    filament::TransformManager& tcm = engine->getTransformManager();
                    const utils::Entity* ents = instance->getEntities();
                    size_t ec = instance->getEntityCount();
                    auto* rcm = &engine->getRenderableManager();
                    utils::Entity target;
                    for (size_t i = 0; i < ec; ++i) {
                        if (rcm->getInstance(ents[i])) {
                            target = ents[i];
                            break;
                        }
                    }
                    const utils::Entity* jts = instance->getJointsAt(0);
                    const filament::math::mat4f& ibm0 = instance->getInverseBindMatricesAt(0)[0];
                    utils::info("gltfdbg target=%d", (int)!target.isNull());
                    if (!target.isNull()) {
                        auto wx = tcm.getInstance(target);
                        const auto wm = tcm.getWorldTransformAccurate(wx);
                        utils::info("gltfdbg W_mesh=(%.2f %.2f %.2f)", wm[3][0], wm[3][1], wm[3][2]);
                    }
                    {
                        const auto wj = tcm.getWorldTransformAccurate(tcm.getInstance(jts[0]));
                        utils::info("gltfdbg W_j0=(%.3f %.3f %.3f) s00=%.3f", wj[3][0], wj[3][1], wj[3][2], wj[0][0]);
                        utils::info("gltfdbg IBM0=(%.2f %.2f %.2f | %.1f %.1f %.1f)",
                                ibm0[0][0], ibm0[1][1], ibm0[2][2], ibm0[3][0], ibm0[3][1], ibm0[3][2]);
                    }
                    if (!target.isNull()) {
                        const auto wm = tcm.getWorldTransformAccurate(tcm.getInstance(target));
                        const auto wmi = inverse(wm);
                        const auto* ibms = instance->getInverseBindMatricesAt(0);
                        for (size_t j : (size_t[]){0u, 5u, 15u, 30u, 45u, 60u, 64u}) {
                            if (j >= instance->getJointCountAt(0)) continue;
                            const auto wj = tcm.getWorldTransformAccurate(tcm.getInstance(jts[j]));
                            const auto m = wmi * wj * ibms[j];
                            utils::info("gltfdbg M[%zu]=(s00=%.4f s11=%.4f s22=%.4f | t=%.2f %.2f %.2f) Wj_s=%.4f IBM_s=%.2f",
                                    j, m[0][0], m[1][1], m[2][2], m[3][0], m[3][1], m[3][2],
                                    std::sqrt(wj[0][0]*wj[0][0]+wj[1][0]*wj[1][0]+wj[2][0]*wj[2][0]),
                                    std::sqrt(ibms[j][0][0]*ibms[j][0][0]+ibms[j][1][0]*ibms[j][1][0]+ibms[j][2][0]*ibms[j][2][0]));
                        }
                    }
                }
                for (size_t i : (size_t[]){0u, 1u, 2u, 4u, 5u, 6u, 10u, 15u, 20u, 40u, 45u, 60u, 64u}) {
                    if (i >= s_syncNodes.size()) continue;
                    const auto a = tcm.getTransform(s_syncNodes[i].animInst);
                    const auto m = tcm.getTransform(s_syncNodes[i].mainInst);
                    utils::info("gltfdbg[%d] n%zu anim=(t %.3f %.3f %.3f | s %.4f %.4f %.4f) main=(t %.3f %.3f %.3f | s %.4f %.4f %.4f)",
                            s_dbg, i, a[3][0], a[3][1], a[3][2],
                            std::sqrt(a[0][0]*a[0][0]+a[1][0]*a[1][0]+a[2][0]*a[2][0]),
                            std::sqrt(a[0][1]*a[0][1]+a[1][1]*a[1][1]+a[2][1]*a[2][1]),
                            std::sqrt(a[0][2]*a[0][2]+a[1][2]*a[1][2]+a[2][2]*a[2][2]),
                            m[3][0], m[3][1], m[3][2],
                            std::sqrt(m[0][0]*m[0][0]+m[1][0]*m[1][0]+m[2][0]*m[2][0]),
                            std::sqrt(m[0][1]*m[0][1]+m[1][1]*m[1][1]+m[2][1]*m[2][1]),
                            std::sqrt(m[0][2]*m[0][2]+m[1][2]*m[1][2]+m[2][2]*m[2][2]));
                }
            }
        }
    }
}

bool gltfBoundingBoxFilament(float min[3], float max[3]) {
    if (!asset) {
        return false;
    }
    filament::Aabb box = asset->getBoundingBox();
    // Compensate the wrapper scale for cm-authored assets (see s_compScale);
    // the local AABB is then in true world metres.
    if (s_compScale != 1.0f) {
        box.min.x *= s_compScale; box.min.y *= s_compScale; box.min.z *= s_compScale;
        box.max.x *= s_compScale; box.max.y *= s_compScale; box.max.z *= s_compScale;
    }
    // World-space: add the instance root's translation (identity otherwise).
    if (instance && engine) {
        filament::TransformManager& tcm = engine->getTransformManager();
        const filament::math::mat4f& m = tcm.getTransform(tcm.getInstance(instance->getRoot()));
        f32 dx = m[3][0], dy = m[3][1], dz = m[3][2];  // translation column
        box.min.x += dx;
        box.min.y += dy;
        box.min.z += dz;
        box.max.x += dx;
        box.max.y += dy;
        box.max.z += dz;
    }
    min[0] = box.min.x;
    min[1] = box.min.y;
    min[2] = box.min.z;
    max[0] = box.max.x;
    max[1] = box.max.y;
    max[2] = box.max.z;
    return true;
}

void gltfDestroyFilament(void) {
    if (animAsset) {
        loader->destroyAsset(animAsset);
        animAsset = nullptr;
        animInstance = nullptr;
        animAnimator = nullptr;
    }
    s_play = {};
    s_syncBones = false;
    s_syncNodes.clear();
    s_haveSync = false;

    if (asset) {
        scene->removeEntities(asset->getEntities(), asset->getEntityCount());
        loader->destroyAsset(asset);
        asset = nullptr;
        instance = nullptr;
        animator = nullptr;
    }

    if (materialProvider) {
        materialProvider->destroyMaterials();
        delete materialProvider;
        materialProvider = nullptr;
    }

    delete resourceLoader;
    delete stbDecoder;
    delete ktx2Decoder;
    AssetLoader::destroy(&loader);
    resourceLoader = nullptr;
    stbDecoder = nullptr;
    ktx2Decoder = nullptr;
    utils::info("gltf: destroyed");
}
}  // namespace engine::gltf
