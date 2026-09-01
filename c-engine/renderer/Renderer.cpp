#include "Renderer.h"
#include "Utils.h"
#include "logger/Logger.h"
#include "Window.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/git/stb_image_write.h"
#include <backend/PixelBufferDescriptor.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <utils/EntityManager.h>

namespace engine::renderer {
filament::Engine* filamentEngine;
filament::SwapChain* swapChain;
filament::Renderer* renderer;
filament::Scene* scene;
filament::View* view;
filament::Camera* camera;
utils::Entity cameraEntity;

static u32 viewportWidth = 0;
static u32 viewportHeight = 0;

// ENGINE_SCREENSHOT=path — capture one frame to a JPEG (for automated runs)
static const char* screenshotPath = nullptr;
static u8* screenshotBuffer = nullptr;
static bool screenshotDone = false;
static u32 screenshotFrame = 0;

static void screenshotSave(void) {
    if (!stbi_write_jpg(screenshotPath, (int)window.width, (int)window.height,
                        4, screenshotBuffer, 90)) {
        utils::warn("renderer: cannot save screenshot to %s", screenshotPath);
    } else {
        utils::info("renderer: screenshot saved to %s", screenshotPath);
    }

    free(screenshotBuffer);
    screenshotBuffer = nullptr;
}

bool rendererInit(const char* title, u32 width, u32 height) {
    if (!windowCreate(title, width, height)) {
        return false;
    }

    filamentEngine = filament::Engine::create();
    if (!filamentEngine) {
        utils::error("renderer: filament::Engine::create failed");
        windowDestroy();
        return false;
    }
    utils::info("renderer: filament engine created");

    swapChain = filamentEngine->createSwapChain(windowNativeHandle());
    renderer = filamentEngine->createRenderer();
    scene = filamentEngine->createScene();
    view = filamentEngine->createView();

    cameraEntity = utils::EntityManager::get().create();
    camera = filamentEngine->createCamera(cameraEntity);
    camera->lookAt({0.0, 0.0, 5.0}, {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0});

    view->setScene(scene);
    view->setCamera(camera);

    renderer->setClearOptions({
        .clearColor = {0.02, 0.04, 0.09, 1.0},
        .clear = true,
    });

    viewportWidth = window.width;
    viewportHeight = window.height;
    view->setViewport({0, 0, (uint32_t)viewportWidth, (uint32_t)viewportHeight});
    camera->setProjection(60.0, (double)viewportWidth / (double)viewportHeight, 0.1, 20000.0, filament::Camera::Fov::VERTICAL);

    utils::info("renderer: initialized");

    const char* screenshotEnv = getenv("ENGINE_SCREENSHOT");
    if (screenshotEnv && screenshotEnv[0] != '\0') {
        screenshotPath = screenshotEnv;
    }
    return true;
}

void rendererDraw(void) {
    // window resized → update viewport + projection
    if (window.width != viewportWidth || window.height != viewportHeight) {
        viewportWidth = window.width;
        viewportHeight = window.height;
        view->setViewport({0, 0, (uint32_t)viewportWidth, (uint32_t)viewportHeight});
        camera->setProjection(60.0, (double)viewportWidth / (double)viewportHeight, 0.1, 20000.0, filament::Camera::Fov::VERTICAL);
    }

    if (renderer->beginFrame(swapChain)) {
        renderer->render(view);

        // capture after a few frames so shaders/textures are warm; the callback
        // fires a few frames later, once the readback completes
        if (screenshotPath && !screenshotDone && screenshotFrame++ >= 3) {
            screenshotBuffer = (u8*)malloc((size_t)window.width * window.height * 4);
            renderer->readPixels(0, 0, window.width, window.height,
                    filament::backend::PixelBufferDescriptor(screenshotBuffer, (size_t)window.width * window.height * 4,
                            filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::UBYTE,
                            [](void*, size_t, void*) { screenshotSave(); }));
            screenshotDone = true;
        }

        renderer->endFrame();
    }
}

void rendererDestroy(void) {
    utils::info("renderer: destroying");

    filament::Engine* enginePtr = filamentEngine;
    if (enginePtr) {
        enginePtr->destroyCameraComponent(cameraEntity);
        utils::EntityManager::get().destroy(cameraEntity);
        enginePtr->destroy(view);
        enginePtr->destroy(scene);
        enginePtr->destroy(renderer);
        enginePtr->destroy(swapChain);
        filament::Engine::destroy(&enginePtr);
        filamentEngine = nullptr;
    }

    windowDestroy();
}
}  // namespace engine::renderer
