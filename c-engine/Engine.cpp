#include "Engine.h"
#include "Utils.h"
#include "ecs/Ecs.h"
#include "ecs/system/lua/LuaSystem.h"
#include "renderer/Renderer.h"
#include "renderer/Window.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace engine {
volatile char engineRunning = 1;

static System* gameSystem;
static double engineStopAtNanos = 0;  // ENGINE_LOG_TIMEOUT: auto-quit for automated runs
static double engineWorldLoadedAtNanos = 0;

// ENGINE_RSS_TRACE=<frames>: log this process's RSS + peak RSS every N frames.
static void engineRssTrace(u32 frame)
{
#ifdef __linux__
    static const u32 every = [] {
        const char* e = getenv("ENGINE_RSS_TRACE");
        return e ? (u32)atoi(e) : 0u;
    }();
    if (!every || frame % every != 0) {
        return;
    }
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) {
        return;
    }
    long rss = -1, hwm = -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "VmRSS:", 6)) {
            rss = atol(line + 6);
        } else if (!strncmp(line, "VmHWM:", 6)) {
            hwm = atol(line + 6);
        }
    }
    fclose(f);
    utils::info("mem: frame %u RSS %.0f MB peak %.0f MB", frame, (double)rss / 1024.0,
                (double)hwm / 1024.0);
#endif
}

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

    u32 engineFrame = 0;
    while (engineRunning) {
        engineFrame++;
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

        engineRssTrace(engineFrame);

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
