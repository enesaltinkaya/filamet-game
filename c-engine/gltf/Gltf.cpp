#include "Gltf.h"
#include "Utils.h"
#include "datamanager/DataManager.h"
#include "logger/Logger.h"
#include "renderer/Renderer.h"
#include "string/String.h"

#include <cmath>
#include <filament/Box.h>
#include <filament/Camera.h>
#include <filament/Scene.h>
#include <gltfio/Animator.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/FilamentInstance.h>
#include <gltfio/MaterialProvider.h>
#include <gltfio/ResourceLoader.h>
#include <gltfio/TextureProvider.h>
#include <math/vec3.h>
#include <utils/EntityManager.h>
#include <utils/NameComponentManager.h>

#include "materials/uberarchive.h"

namespace engine::gltf {
using namespace filament::gltfio;
using namespace filament::math;

FilamentAsset* asset = nullptr;

static AssetLoader* loader = nullptr;
static ResourceLoader* resourceLoader = nullptr;
static MaterialProvider* materialProvider = nullptr;
static TextureProvider* stbDecoder = nullptr;
static TextureProvider* ktx2Decoder = nullptr;
static utils::NameComponentManager* names = nullptr;
static FilamentInstance* instance = nullptr;
static Animator* animator = nullptr;

bool gltfInit(void) {
    if (!renderer::filamentEngine) {
        utils::warn("gltf: renderer not initialized");
        return false;
    }

    materialProvider =
            createUbershaderProvider(renderer::filamentEngine, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);
    if (!materialProvider) {
        utils::warn("gltf: ubershader provider failed");
        return false;
    }

    names = new utils::NameComponentManager(utils::EntityManager::get());
    loader = AssetLoader::create({
        .engine = renderer::filamentEngine,
        .materials = materialProvider,
        .names = names,
    });
    if (!loader) {
        utils::warn("gltf: AssetLoader::create failed");
        return false;
    }

    resourceLoader = new ResourceLoader({renderer::filamentEngine, nullptr, true});
    stbDecoder = createStbProvider(renderer::filamentEngine);
    ktx2Decoder = createKtx2Provider(renderer::filamentEngine);
    resourceLoader->addTextureProvider("image/png", stbDecoder);
    resourceLoader->addTextureProvider("image/jpeg", stbDecoder);
    resourceLoader->addTextureProvider("image/ktx2", ktx2Decoder);

    utils::info("gltf: initialized (draco %s, webp %s)", GLTFIO_DRACO_SUPPORTED ? "on" : "off",
            isWebpSupported() ? "on" : "off");
    return true;
}

bool gltfLoad(const char* pakPath) {
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

    renderer::scene->addEntities(loaded->getEntities(), loaded->getEntityCount());
    utils::stringDestroy(&glb);
    return true;
}

void gltfUpdate(double elapsedSeconds) {
    if (asset) {
        names->gc();
        loader->gc();
    }
    if (animator && animator->getAnimationCount() > 0) {
        animator->applyAnimation(0, (float)elapsedSeconds);
        animator->updateBoneMatrices();
    }
}

void gltfFrameCamera(void) {
    if (!asset) {
        return;
    }

    filament::Aabb box = asset->getBoundingBox();
    float3 center = (box.min + box.max) * 0.5f;
    float radius = length(box.max - box.min) * 0.5f;
    if (radius <= 0.0f) {
        radius = 1.0f;
    }

    // rendererInit uses a 60 degree vertical fov
    float distance = (radius / tanf(30.0f * static_cast<float>(M_PI) / 180.0f)) * 1.2f;
    float3 direction = normalize(float3{0.5f, 0.3f, 1.0f});
    renderer::camera->lookAt(center + direction * distance, center, {0.0f, 1.0f, 0.0f});
}

size_t gltfEntitiesNamed(const char* prefix, utils::Entity* out, size_t cap) {
    if (!asset || !names) {
        return 0;
    }

    size_t found = 0;
    const utils::Entity* entities = asset->getEntities();
    for (size_t i = 0; i < asset->getEntityCount(); i++) {
        utils::Entity e = entities[i];
        if (!names->hasComponent(e)) {
            continue;
        }
        const char* name = names->getName(names->getInstance(e));
        if (name && utils::strStartsWith(name, prefix)) {
            if (found < cap) {
                out[found] = e;
            }
            found++;
        }
    }
    return found;
}

void gltfDestroy(void) {
    if (asset) {
        renderer::scene->removeEntities(asset->getEntities(), asset->getEntityCount());
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
    delete names;

    resourceLoader = nullptr;
    stbDecoder = nullptr;
    ktx2Decoder = nullptr;
    names = nullptr;
    utils::info("gltf: destroyed");
}
}  // namespace engine::gltf
