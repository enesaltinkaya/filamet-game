#include "renderer/texture/TextureManager.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "logger/Logger.h"
#include "renderer/diligent/DiligentRenderer.h"
#include "thread/Thread.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

struct TextureEntry {
    utils::Image image;
    Diligent::ITexture* texture = nullptr;
};

static std::unordered_map<std::string, TextureEntry> textureMap;
static utils::Thread textureLock = {.mutex = PTHREAD_MUTEX_INITIALIZER};

// Decode one file (the heavy part) and cache the image. The ITexture is
// created on first textureManagerGetTexture, on the main thread, with the
// consumer's srgb flag: the flag picks the Diligent format for uncompressed
// 8-bit payloads (texFormatFromVk), so it must come from the usage, not from
// a prewarm guess. The map slot is reserved before the heavy work so
// concurrent requests for the same path dedupe.
static void decodeOneTexture(const char* path) {
    utils::threadLock(&textureLock);
    const bool fresh = textureMap.find(path) == textureMap.end();
    if (fresh) {
        textureMap.emplace(path, TextureEntry{});
    }
    utils::threadUnlock(&textureLock);
    if (!fresh) {
        return;
    }

    utils::Image image = utils::imageLoad(path);
    if (!image.isKtx || !image.data || image.width <= 0 || image.height <= 0 ||
        image.mips <= 0) {
        utils::warn("textureManager: decode failed: %s", path);
        if (image.data) {
            utils::imageDestory(&image);
            image = {};
        }
    }

    utils::threadLock(&textureLock);
    textureMap[path].image = std::move(image);
    utils::threadUnlock(&textureLock);
}

static void decodeTextureWork(void* arg) {
    const char* path = static_cast<const char*>(arg);
    decodeOneTexture(path);
}

void textureManagerInit(void) {
    double elapsed = utils::elapsedBegin();

    std::vector<utils::String> ktxFiles = utils::dataManagerListFiles(".ktx2");
    utils::info("textureManager: found %d ktx2 files in paks", (int)ktxFiles.size());

    for (i32 i = 0, s = (i32)ktxFiles.size(); i < s; i++) {
        utils::threadPoolAddWork(nullptr, decodeTextureWork, ktxFiles[i].data);
    }

    utils::threadPoolWait(nullptr);

    for (i32 i = 0, s = (i32)ktxFiles.size(); i < s; i++) {
        utils::stringDestroy(&ktxFiles[i]);
    }

    elapsed = utils::elapsedEnd(elapsed);
    utils::info("textureManager: preloaded %d textures in %.02f ms", (int)textureMap.size(), elapsed);
}

// Main thread only: the first getTexture for a path uploads the cached
// decode (Diligent state transitions are not thread-safe).
static Diligent::ITexture* createCachedTexture(const char* path, bool srgbForUncompressed) {
    Diligent::ITexture* texture = nullptr;
    utils::threadLock(&textureLock);
    TextureEntry& entry = textureMap[path];
    if (entry.image.isKtx && entry.image.data) {
        texture = renderer::diligent::diligentCreateImageTexture(entry.image, path,
                srgbForUncompressed);
    }
    entry.texture = texture;
    utils::threadUnlock(&textureLock);
    if (texture) {
        texture->AddRef();
    }
    return texture;
}

Diligent::ITexture* textureManagerGetTexture(const char* path, bool srgbForUncompressed) {
    utils::threadLock(&textureLock);
    auto it = textureMap.find(path);
    if (it != textureMap.end() && it->second.texture) {
        it->second.texture->AddRef();
        utils::threadUnlock(&textureLock);
        return it->second.texture;
    }
    if (it != textureMap.end()) {
        utils::threadUnlock(&textureLock);
        return createCachedTexture(path, srgbForUncompressed);
    }
    utils::threadUnlock(&textureLock);

    decodeOneTexture(path);
    return createCachedTexture(path, srgbForUncompressed);
}

const utils::Image* textureManagerGetImage(const char* path) {
    utils::threadLock(&textureLock);
    auto it = textureMap.find(path);
    const utils::Image* image =
        (it != textureMap.end() && it->second.image.data) ? &it->second.image : nullptr;
    utils::threadUnlock(&textureLock);
    return image;
}

void textureManagerDestroy(void) {
    utils::threadLock(&textureLock);
    for (auto& [path, entry] : textureMap) {
        if (entry.texture) {
            entry.texture->Release();
            entry.texture = nullptr;
        }
        if (entry.image.data) {
            utils::imageDestory(&entry.image);
            entry.image = {};
        }
    }
    textureMap.clear();
    utils::threadUnlock(&textureLock);
}

}  // namespace engine
