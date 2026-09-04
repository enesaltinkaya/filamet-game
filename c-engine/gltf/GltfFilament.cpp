#include "gltf/GltfInternal.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "logger/Logger.h"
#include "renderer/filament/FilamentRenderer.h"

#include <filament/Box.h>
#include <filament/Engine.h>
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
            return false;
        }
        zstdBuf = malloc((size_t)outSize);
        u64 decomp = ZSTD_decompress(zstdBuf, outSize, glb.data, glb.size);
        if (ZSTD_isError(decomp) || decomp != outSize) {
            utils::warn("gltf: zstd decompress failed for %s (%s)", pakPath,
                    ZSTD_getErrorName(decomp));
            free(zstdBuf);
            utils::stringDestroy(&glb);
            return false;
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
        return false;
    }
    free(zstdBuf);

    if (!resourceLoader->loadResources(loaded)) {
        utils::warn("gltf: loadResources failed for %s", pakPath);
    }

    instance = loaded->getInstance();
    animator = instance ? instance->getAnimator() : nullptr;
    asset = loaded;

    filament::Aabb box = loaded->getBoundingBox();
    utils::info("gltf: %s — %zu entities, %zu animations, bounds [%.2f %.2f %.2f]-[%.2f %.2f %.2f]",
            pakPath, loaded->getEntityCount(), animator ? animator->getAnimationCount() : 0, box.min.x, box.min.y,
            box.min.z, box.max.x, box.max.y, box.max.z);

    scene->addEntities(loaded->getEntities(), loaded->getEntityCount());
    utils::stringDestroy(&glb);
    return true;
}

bool gltfPlaceAtFilament(f32 x, f32 y, f32 z) {
    if (!asset || !instance || !engine) {
        return false;
    }
    // The asset AABB is in instance-local space (node hierarchy only); the
    // gltfio root transform is applied on top, so translating the root puts
    // the local min corner (feet for character assets) at (x, y, z).
    filament::Aabb box = asset->getBoundingBox();
    filament::math::float3 offset{x - box.min.x, y - box.min.y, z - box.min.z};
    filament::TransformManager& tcm = engine->getTransformManager();
    tcm.setTransform(tcm.getInstance(instance->getRoot()),
            filament::math::mat4f::translation(offset));
    return true;
}

void gltfUpdateFilament(double elapsedSeconds) {
    if (asset) {
        loader->gc();
    }
    if (animator && animator->getAnimationCount() > 0) {
        animator->applyAnimation(0, (float)elapsedSeconds);
        animator->updateBoneMatrices();
    }
}

bool gltfBoundingBoxFilament(float min[3], float max[3]) {
    if (!asset) {
        return false;
    }
    filament::Aabb box = asset->getBoundingBox();
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
