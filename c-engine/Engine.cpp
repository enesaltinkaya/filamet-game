#include "Engine.h"
#include "Utils.h"
#include "ecs/Ecs.h"
#include "renderer/Renderer.h"
#include "renderer/Window.h"

namespace engine {
volatile char engineRunning = 1;

static System* gameSystem;
static double engineStopAtNanos = 0;  // ENGINE_LOG_TIMEOUT: auto-quit for automated runs

void engineSetGameSystem(System* system) {
    gameSystem = system;
}

void engineStart(void) {
    if (!gameSystem) {
        utils::terminate("call engineSetGameSystem(...) before engineStart()");
    }

    char* logTimeoutEnv = getenv("ENGINE_LOG_TIMEOUT");
    if (logTimeoutEnv) {
        engineStopAtNanos = utils::nanos() + atof(logTimeoutEnv) * MILLION;
    }

    utils::info("engine: starting");
    renderer::rendererInit("filament-game", 0, 0);
    ecsInit(gameSystem);

    while (engineRunning) {
        utils::timerBegin();

        windowPollEvents();

        ecsPreUpdate();
        ecsUpdate();
        ecsPostUpdate();

        renderer::rendererDraw();

        utils::timerEnd();

        if (engineStopAtNanos && utils::nanos() > engineStopAtNanos) {
            utils::info("engine: ENGINE_LOG_TIMEOUT reached");
            engineRunning = 0;
        }
    }

    utils::info("engine: stopping");
    ecsDestroy();
    renderer::rendererDestroy();
}

void engineStop(void) {
    engineRunning = 0;
}
}  // namespace engine
