#include "Renderer.h"
#include "Utils.h"
#include "logger/Logger.h"
#include "Window.h"
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

// ENGINE_SCREENSHOT=path — capture one frame to a BMP (for automated runs)
static const char* screenshotPath = nullptr;
static u8* screenshotBuffer = nullptr;
static bool screenshotDone = false;
static u32 screenshotFrame = 0;

static void screenshotSave(void) {
    FILE* file = fopen(screenshotPath, "wb");
    if (!file) {
        utils::warn("renderer: cannot open %s for screenshot", screenshotPath);
        return;
    }

    u32 width = window.width;
    u32 height = window.height;
    u32 stride = (width * 3 + 3) & ~3u;
    u32 dataSize = stride * height;

    u8 header[54] = {};
    header[0] = 'B';
    header[1] = 'M';
    u32 fileSize = 54 + dataSize;
    memcpy(&header[2], &fileSize, 4);
    u32 dataOffset = 54;
    memcpy(&header[10], &dataOffset, 4);
    u32 dibSize = 40;
    memcpy(&header[14], &dibSize, 4);
    memcpy(&header[18], &width, 4);
    memcpy(&header[22], &height, 4);
    u16 planes = 1;
    memcpy(&header[26], &planes, 2);
    u16 bpp = 24;
    memcpy(&header[28], &bpp, 2);
    memcpy(&header[34], &dataSize, 4);

    fwrite(header, 1, sizeof(header), file);
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            u8* px = &screenshotBuffer[((y * width) + x) * 4];
            u8 bgr[3] = {px[2], px[1], px[0]};
            fwrite(bgr, 1, 3, file);
        }
        for (u32 pad = width * 3; pad < stride; pad++) {
            fputc(0, file);
        }
    }
    fclose(file);

    free(screenshotBuffer);
    screenshotBuffer = nullptr;
    utils::info("renderer: screenshot saved to %s", screenshotPath);
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
    camera->setProjection(60.0, (double)viewportWidth / (double)viewportHeight, 0.1, 100.0, filament::Camera::Fov::VERTICAL);

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
        camera->setProjection(60.0, (double)viewportWidth / (double)viewportHeight, 0.1, 100.0, filament::Camera::Fov::VERTICAL);
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
