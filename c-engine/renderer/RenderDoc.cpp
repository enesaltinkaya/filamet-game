#include "RenderDoc.h"

#include "Utils.h"
#include "logger/Logger.h"

#include <cstdlib>
#include <string>

#if !defined(NDEBUG) && defined(__linux__)
#include "renderdoc_app.h"
#include <dlfcn.h>
#include <sys/stat.h>

namespace engine::renderer {
static RENDERDOC_API_1_1_2* rdocApi = nullptr;

static const char* kDefaultLib = "/home/enes/Apps/renderdoc/build/lib/librenderdoc.so";
// template prefix — captures land at /tmp/RenderDoc/c-game_<date>_<time>_frameN.rdc
static const char* kDefaultCaptureTemplate = "/tmp/RenderDoc/c-game";

void* renderDocInit(void) {
    if (rdocApi) {
        return rdocApi;
    }

    const char* libPath = getenv("ENGINE_RENDERDOC_LIB");
    if (!libPath || !*libPath) {
        libPath = kDefaultLib;
    }

    // Prefer an already-loaded copy (LD_PRELOAD / implicit layer) so we talk
    // to the instance whose Vulkan hooks are active.
    void* module = dlopen(libPath, RTLD_NOLOAD | RTLD_NOW);
    if (module) {
        utils::info("renderdoc: using preloaded %s", libPath);
    } else {
        // Not preloaded: load it anyway so the API is reachable. Captures
        // only work when the lib was preloaded, i.e. its Vulkan hooks were
        // installed at process start.
        module = dlopen(libPath, RTLD_NOW);
        if (module) {
            utils::warn("renderdoc: loaded %s (not preloaded — capture may not hook Vulkan)",
                    libPath);
        }
    }
    if (!module) {
        utils::info("renderdoc: not present");
        return nullptr;
    }

    pRENDERDOC_GetAPI getApi = (pRENDERDOC_GetAPI)dlsym(module, "RENDERDOC_GetAPI");
    if (!getApi || getApi(eRENDERDOC_API_Version_1_1_2, (void**)&rdocApi) != 1) {
        utils::warn("renderdoc: RENDERDOC_GetAPI failed");
        rdocApi = nullptr;
        return nullptr;
    }

    const char* dir = getenv("ENGINE_RENDERDOC_DIR");
    if (!dir || !*dir) {
        dir = kDefaultCaptureTemplate;
    }
    std::string captureDir(dir);
    size_t slash = captureDir.find_last_of('/');
    if (slash != std::string::npos) {
        mkdir(captureDir.substr(0, slash).c_str(), 0755);
    }
    rdocApi->SetLogFilePathTemplate(dir);
    utils::info("renderdoc: api ready, captures land at %s_<...>_frameN.rdc", dir);
    return rdocApi;
}

void renderDocCaptureNow(void) {
    RENDERDOC_API_1_1_2* api = (RENDERDOC_API_1_1_2*)renderDocInit();
    if (api) {
        utils::info("renderdoc: TriggerCapture");
        api->TriggerCapture();
    }
}
}  // namespace engine::renderer

#else
namespace engine::renderer {
void* renderDocInit(void) {
    return nullptr;
}
void renderDocCaptureNow(void) {}
}  // namespace engine::renderer
#endif
