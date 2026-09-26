#include "Engine.h"
#include "Utils.h"
#include "ecs/Ecs.h"
#include "ecs/system/lua/LuaSystem.h"
#include "renderer/Renderer.h"
#include "renderer/Window.h"

namespace engine {
volatile char engineRunning = 1;

static System* gameSystem;
static double engineStopAtNanos = 0;  // ENGINE_LOG_TIMEOUT: auto-quit for automated runs
static double engineWorldLoadedAtNanos = 0;

void engineMarkWorldLoaded(void) {
    engineWorldLoadedAtNanos = utils::nanos();
}

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

        ecsApplyDeferred();  // apply system add/remove queued last frame (e.g. GUI transitions)
        ecsPreUpdate();
        ecsUpdate();
        ecsPostUpdate();

        renderer::rendererDraw();

        if (engineWorldLoadedAtNanos) {
            utils::info("load timing: world loaded -> first drawn frame %.1f ms",
                    (utils::nanos() - engineWorldLoadedAtNanos) / MILLION);
            engineWorldLoadedAtNanos = 0;
        }

        utils::timerEnd();

        if (engineStopAtNanos && utils::nanos() > engineStopAtNanos) {
            utils::info("engine: ENGINE_LOG_TIMEOUT reached");
            engineRunning = 0;
        }
    }

    utils::info("engine: stopping");
    ecsDestroy();
    // The Lua state must outlive ecsDestroy: rmlDestroy (in the rmlui systems'
    // removed()) tears down Rml::Contexts whose Lua listeners use the state
    // during ~Context. Same order as the old engine (ecsDestroy → luaDestroy).
    luaDestroy();
    renderer::rendererDestroy();
}

void engineStop(void) {
    engineRunning = 0;
}
}
