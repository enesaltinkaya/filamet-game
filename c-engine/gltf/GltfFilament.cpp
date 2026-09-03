#include "gltf/GltfInternal.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "logger/Logger.h"
#include "renderer/filament/FilamentRenderer.h"

#include <filament/Box.h>
#include <filament/Engine.h>
#include <filament/Scene.h>
#include <gltfio/Animator.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/FilamentInstance.h>
#include <gltfio/MaterialProvider.h>
#include <gltfio/ResourceLoader.h>
#include <gltfio/TextureProvider.h>
#include <utils/EntityManager.h>

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

    utils::String glb = utils::dataManagerRead(pakPath);
    FilamentAsset* loaded = loader->createAsset((const u8*)glb.data, glb.size);
    if (!loaded) {
        utils::warn("gltf: createAsset failed for %s", pakPath);
        utils::stringDestroy(&glb);
        return false;
    }

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
