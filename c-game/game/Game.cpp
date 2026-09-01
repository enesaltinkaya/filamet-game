#include "Game.h"
#include "Utils.h"
#include "gltf/Gltf.h"
#include "renderer/Renderer.h"

#include <filament/Engine.h>
#include <filament/IndirectLight.h>
#include <filament/LightManager.h>
#include <filament/Scene.h>
#include <math/vec3.h>
#include <utils/EntityManager.h>

namespace game {
using namespace filament::math;

static utils::Entity sun;
static filament::IndirectLight* ambientLight = nullptr;

GameSystem::GameSystem() : System("Game") {}

void GameSystem::added() {
    // prove the pak system works: read a file shipped in pak_0
    utils::String version = utils::dataManagerRead("version.txt");
    utils::stringTrim(&version);
    utils::info("game: added — pak version: %s", version.data);
    utils::stringDestroy(&version);

    engine::gltf::gltfInit();
    engine::gltf::gltfLoad("models/DamagedHelmet.glb");
    engine::gltf::gltfFrameCamera();

    sun = utils::EntityManager::get().create();
    filament::LightManager::Builder(filament::LightManager::Type::SUN)
        .color({1.0f, 0.97f, 0.92f})
        .intensity(110000.0f)
        .direction(normalize(float3{-0.6f, -1.0f, -0.5f}))
        .build(*engine::renderer::filamentEngine, sun);
    engine::renderer::scene->addEntity(sun);

    // constant ambient (SH band 0) — no cubemap needed
    float3 ambient[9] = {};
    ambient[0] = {0.32f, 0.35f, 0.38f};
    ambientLight = filament::IndirectLight::Builder()
        .irradiance(3, ambient)
        .intensity(30000.0f)
        .build(*engine::renderer::filamentEngine);
    engine::renderer::scene->setIndirectLight(ambientLight);
}

void GameSystem::removed() {
    engine::gltf::gltfDestroy();

    engine::renderer::scene->remove(sun);
    engine::renderer::filamentEngine->destroy(sun);
    utils::EntityManager::get().destroy(sun);

    engine::renderer::scene->setIndirectLight(nullptr);
    engine::renderer::filamentEngine->destroy(ambientLight);
    ambientLight = nullptr;
    utils::info("game: removed");
}

void GameSystem::update() {
    engine::gltf::gltfUpdate(utils::nanos() / BILLION);
}

GameSystem gameSystem;
}  // namespace game
