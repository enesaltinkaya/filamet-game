#include "renderer/filament/FilamentRenderer.h"

#include "Engine.h"
#include "Utils.h"
#include "ecs/system/heightmap/HeightmapTerrainRender.h"
#include "gui/Gui.h"
#include "gui/GuiManager.h"
#include "logger/Logger.h"
#include "renderer/Renderer.h"
#include "renderer/Window.h"

#include <backend/PixelBufferDescriptor.h>
#include <cstdlib>
#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndirectLight.h>
#include <filament/LightManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <math/vec3.h>
#include <utils/EntityManager.h>

namespace engine::renderer {

namespace filament_globals {
filament::Engine* engine = nullptr;
filament::SwapChain* swapChain = nullptr;
filament::Renderer* renderer = nullptr;
filament::Scene* scene = nullptr;
filament::View* view = nullptr;
filament::View* uiView = nullptr;
filament::Camera* camera = nullptr;
utils::Entity cameraEntity{};
}  // namespace filament_globals

using namespace filament_globals;

// bridge for the readPixels completion callback (PixelBufferDescriptor takes a
// plain function pointer — no captures)
class FilamentBackend;
static FilamentBackend* gFilamentBackend = nullptr;
static void filamentScreenshotBufferReady(void);

class FilamentBackend final : public RenderBackend {
public:
    ~FilamentBackend() override = default;

    bool init() override {
        engine = filament::Engine::create(filament::Engine::Backend::VULKAN);
        if (!engine) {
            utils::error("renderer: filament::Engine::create failed (Vulkan)");
            return false;
        }
        utils::info("renderer: filament engine created");

        swapChain = engine->createSwapChain(windowNativeHandle());
        renderer = engine->createRenderer();
        scene = engine->createScene();
        view = engine->createView();
        uiView = engine->createView();

        cameraEntity = utils::EntityManager::get().create();
        camera = engine->createCamera(cameraEntity);
        const f32 eye[3] = {0.0f, 0.0f, 5.0f};
        const f32 center[3] = {0.0f, 0.0f, 0.0f};
        const f32 up[3] = {0.0f, 1.0f, 0.0f};
        cameraLookAt(eye, center, up);

        view->setScene(scene);
        view->setCamera(camera);

        renderer->setClearOptions({
                .clearColor = {kClearColor[0], kClearColor[1], kClearColor[2], kClearColor[3]},
                .clear = true,
        });

        resize(window.width, window.height);
        return true;
    }

    void resize(u32 width, u32 height) override {
        view->setViewport({0, 0, width, height});
        uiView->setViewport({0, 0, width, height});
        camera->setProjection((double)kCameraFovYDeg, (double)width / (double)height, kCameraNear,
                kCameraFar, filament::Camera::Fov::VERTICAL);
    }

    void draw() override {
        // heightmap terrain: sync streaming tiles + budgeted GPU uploads
        // (main thread, before the frame renders)
        heightmapTerrainRenderUpdate();

        if (renderer->beginFrame(swapChain)) {
            renderer->render(view);  // 3D scene
            if (gui::guiIsActive()) {
                renderer->render(uiView);  // 2D GUI overlay (translucent, on top)
            }

            // capture after a few frames so shaders/textures are warm; the
            // callback fires a few frames later, once the readback completes
            if (rendererScreenshotShouldCapture()) {
                u8* buffer = (u8*)malloc((size_t)window.width * window.height * 4);
                renderer->readPixels(0, 0, window.width, window.height,
                        filament::backend::PixelBufferDescriptor(buffer,
                                (size_t)window.width * window.height * 4,
                                filament::backend::PixelDataFormat::RGBA,
                                filament::backend::PixelDataType::UBYTE,
                                [](void*, size_t, void*) { filamentScreenshotBufferReady(); }));
                pendingScreenshot = buffer;
            }

            renderer->endFrame();
        }
    }

    // called from the readPixels completion callback (buffer filled)
    void screenshotDeliver() {
        if (pendingScreenshot) {
            u8* buffer = pendingScreenshot;
            pendingScreenshot = nullptr;
            rendererScreenshotDeliver(buffer);
        }
    }

    void destroy() override {
        // terrain GPU state first (its scene entities, buffers and textures
        // all live in the engine about to be torn down)
        heightmapTerrainRenderDestroy();

        filament::Engine* enginePtr = engine;
        if (enginePtr) {
            enginePtr->destroyCameraComponent(cameraEntity);
            utils::EntityManager::get().destroy(cameraEntity);
            enginePtr->destroy(view);
            enginePtr->destroy(uiView);
            enginePtr->destroy(scene);
            enginePtr->destroy(renderer);
            enginePtr->destroy(swapChain);
            filament::Engine::destroy(&enginePtr);
            engine = nullptr;
            swapChain = nullptr;
            renderer = nullptr;
            scene = nullptr;
            view = nullptr;
            uiView = nullptr;
            camera = nullptr;
        }
    }

    void cameraLookAt(const f32 eye[3], const f32 center[3], const f32 up[3]) override {
        camera->lookAt({eye[0], eye[1], eye[2]}, {center[0], center[1], center[2]},
                {up[0], up[1], up[2]});
    }

    void cameraGet(f32 pos[3], f32 forward[3]) override {
        auto p = camera->getPosition();
        pos[0] = p.x;
        pos[1] = p.y;
        pos[2] = p.z;
        auto f = camera->getForwardVector();
        forward[0] = f.x;
        forward[1] = f.y;
        forward[2] = f.z;
    }

    void setSun(const f32 direction[3], const f32 color[3], f32 intensity) override {
        sunColor[0] = color[0];
        sunColor[1] = color[1];
        sunColor[2] = color[2];
        sunIntensity = intensity;
        if (!sunEntity) {
            sunEntity = utils::EntityManager::get().create();
            filament::LightManager::Builder(filament::LightManager::Type::SUN)
                    .direction({direction[0], direction[1], direction[2]})
                    .build(*engine, sunEntity);
            scene->addEntity(sunEntity);
        }
        filament::LightManager::Instance inst = engine->getLightManager().getInstance(sunEntity);
        engine->getLightManager().setColor(inst, {color[0], color[1], color[2]});
        engine->getLightManager().setIntensity(inst, intensity);
        engine->getLightManager().setDirection(inst, {direction[0], direction[1], direction[2]});
    }

    void setAmbient(const f32 color[3], f32 intensity) override {
        if (ambientLight) {
            scene->setIndirectLight(nullptr);
            engine->destroy(ambientLight);
            ambientLight = nullptr;
        }
        // constant ambient (SH band 0) — no cubemap needed
        filament::math::float3 sh[9] = {};
        sh[0] = {color[0], color[1], color[2]};
        ambientLight = filament::IndirectLight::Builder()
                               .irradiance(3, sh)
                               .intensity(intensity)
                               .build(*engine);
        scene->setIndirectLight(ambientLight);
    }

    void releaseWorldLights() {
        if (ambientLight) {
            scene->setIndirectLight(nullptr);
            engine->destroy(ambientLight);
            ambientLight = nullptr;
        }
        if (sunEntity) {
            scene->remove(sunEntity);
            engine->destroy(sunEntity);
            utils::EntityManager::get().destroy(sunEntity);
            sunEntity = {};
        }
    }

private:
    u8* pendingScreenshot = nullptr;
    utils::Entity sunEntity{};
    filament::IndirectLight* ambientLight = nullptr;
    f32 sunColor[3] = {1.0f, 1.0f, 1.0f};
    f32 sunIntensity = 0.0f;
};

RenderBackend* filamentBackendCreate(void) {
    gFilamentBackend = new FilamentBackend();
    return gFilamentBackend;
}

static void filamentScreenshotBufferReady(void) {
    if (gFilamentBackend) {
        gFilamentBackend->screenshotDeliver();
    }
}

}  // namespace engine::renderer
