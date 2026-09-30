#include "renderer/diligent/ShaderCache.h"

#include "renderer/diligent/DiligentRenderer.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/PipelineState.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Shader.h"
#include "FileStream.h"
#include "logger/Logger.h"
#include "platform/Platform.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace engine::renderer::diligent {

namespace {

u64 fnv1a(const void* data, size_t size, u64 state) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < size; i++) {
        state ^= p[i];
        state *= 1099511628211ULL;
    }
    return state;
}

std::string readWhole(const char* path) {
    std::string out;
    FILE* f = fopen(path, "rb");
    if (!f)
        return out;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return out;
    }
    const long n = ftell(f);
    rewind(f);
    if (n > 0) {
        out.resize((size_t)n);
        if (fread(out.data(), 1, (size_t)n, f) != (size_t)n)
            out.clear();
    }
    fclose(f);
    return out;
}

bool writeWhole(const char* path, const void* data, size_t size) {
    char tmp[1152];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f)
        return false;
    const bool ok = fwrite(data, 1, size, f) == size && fclose(f) == 0;
    if (!ok) {
        remove(tmp);
        return false;
    }
    return rename(tmp, path) == 0;
}

std::string cachePath(Diligent::IRenderDevice* dev, u64 hash) {
    char file[32];
    snprintf(file, sizeof(file), "%016llx.spv", (unsigned long long)hash);
    std::string slug = dev ? dev->GetAdapterInfo().Description : "unknown";
    for (char& c : slug)
        if (!isalnum((unsigned char)c))
            c = '_';
    std::string dir = std::string(utils::platform.dataDirectory) + "shadercache" +
                      utils::platform.seperator + slug;
    utils::createDirectory((dir + utils::platform.seperator).c_str());
    return dir + utils::platform.seperator + file;
}

// The FX shaders are compiled through the DiligentFX source factory, whose
// sources are embedded at build time; a shader's own file is only the top of its
// include chain (PBR_PixelShader.fx pulls PBR_Shading.fxh etc.). Hashing the
// top-level file alone serves stale SPIR-V for every edit below it — measured:
// a props-receiver PCF kernel change left 91 % of the frame's pixels untouched
// with the cache on, and changed them with the cache off.
// Hash the EMBEDDED blobs (DiligentFX/shaders_inc/*.h, the File2String output
// that the FX source factory serves): the key then tracks exactly the bytes the
// compiler sees. Keying on the raw Shaders/ tree is actively wrong — an edit
// that has not been rebuilt moves the key while the compiled bytes are still
// the old ones, so the stale SPIR-V gets stored under the new key and survives
// the rebuild (measured: a props-receiver change stayed invisible for three
// runs with the cache on and appeared the moment the cache was bypassed).
u64 includeTreeFingerprint(void) {
    static const u64 fp = [] {
        const char* root = getenv("DILIGENT_FX_SHADER_INC_DIR");
        if (root == nullptr)
            root = "/home/enes/Projects/c/cpp-thirdparty/diligent/git/build-linux/DiligentFX/shaders_inc";
        std::error_code ec;
        std::vector<std::string> files;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
            if (!entry.is_regular_file())
                continue;
            const std::string p = entry.path().string();
            if (!p.empty() && p.back() == 'h')
                files.push_back(p);
        }
        std::sort(files.begin(), files.end());
        u64 h = 1469598103934665603ULL;
        for (const std::string& f : files) {
            h = fnv1a(f.c_str(), f.size(), h);
            const std::string body = readWhole(f.c_str());
            h = fnv1a(body.data(), body.size(), h);
        }
        return h;
    }();
    return fp;
}

}

Diligent::IShader* shaderCacheCreate(Diligent::IRenderDevice* dev,
                                     Diligent::ShaderCreateInfo& ci,
                                     Diligent::IDataBlob** ppOutput) {
    if (ppOutput)
        *ppOutput = nullptr;
    if (!dev)
        return nullptr;

    std::string key;
    bool keyValid = true;
    if (ci.Source != nullptr) {
        const size_t len = ci.SourceLength != 0 ? ci.SourceLength : std::strlen(ci.Source);
        key.assign(ci.Source, len);
    } else if (ci.FilePath != nullptr) {
        if (ci.pShaderSourceStreamFactory != nullptr) {
            Diligent::RefCntAutoPtr<Diligent::IFileStream> stream;
            const Diligent::Bool found =
                ci.pShaderSourceStreamFactory->CreateInputStream(ci.FilePath, &stream);
            if (found && stream) {
                const size_t n = (size_t)stream->GetSize();
                key.resize(n);
                if (n > 0 && !stream->Read(key.data(), n))
                    key.clear();
            }
        } else {
            key = readWhole(ci.FilePath);
        }
        if (key.empty())
            keyValid = false;
    }

    const char* name  = ci.Desc.Name != nullptr ? ci.Desc.Name : "";
    const char* entry = ci.EntryPoint != nullptr ? ci.EntryPoint : "";
    u64 hash = 1469598103934665603ULL;
    hash = fnv1a(name, std::strlen(name), hash);
    hash = fnv1a(&ci.Desc.ShaderType, sizeof(ci.Desc.ShaderType), hash);
    hash = fnv1a(entry, std::strlen(entry), hash);
    hash = fnv1a(&ci.CompileFlags, sizeof(ci.CompileFlags), hash);
    if (ci.Macros.Elements != nullptr) {
        for (Diligent::Uint32 i = 0; i < ci.Macros.Count; i++) {
            const char* mn = ci.Macros.Elements[i].Name != nullptr ? ci.Macros.Elements[i].Name : "";
            const char* md = ci.Macros.Elements[i].Definition != nullptr ? ci.Macros.Elements[i].Definition : "";
            hash = fnv1a(mn, std::strlen(mn), hash);
            hash = fnv1a(md, std::strlen(md), hash);
        }
    }
    hash = fnv1a(key.data(), key.size(), hash);
    // Unconditional: the glTF PBR pass feeds the FX sources as ci.Source with
    // no stream factory, and a pak shader can gain includes later — keying the
    // fingerprint on the factory left the props pixel shaders serving stale
    // SPIR-V through two kernel changes (mode 3 with the cache on measured the
    // player's shadow at 2.014 gray, with the cache off 13.762).
    const u64 tree = includeTreeFingerprint();
    hash = fnv1a(&tree, sizeof(tree), hash);
    // The engine's GetPSMainSource hands the FX a generated PS output struct +
    // footer (a memory-include, not a file), so no hashed input changes when it
    // is edited. Bump this whenever those generated strings change.
    static const u64 generatedSourceRevision = 0x51A70001ULL;
    hash = fnv1a(&generatedSourceRevision, sizeof(generatedSourceRevision), hash);
    static const bool keyTrace = getenv("ENGINE_SHADER_CACHE_TRACE") != nullptr;
    if (keyTrace)
        utils::info("shaderCache: key %s tree=%llu keyBytes=%zu src=%d", name,
                    (unsigned long long)tree, key.size(), ci.Source != nullptr ? 1 : 0);

    const std::string path = cachePath(dev, hash);
    const bool cacheEnabled = keyValid && getenv("ENGINE_NO_SHADER_CACHE") == nullptr;

    if (cacheEnabled) {
        const std::string blob = readWhole(path.c_str());
        if (!blob.empty()) {
            Diligent::ShaderCreateInfo hitCI = ci;
            hitCI.FilePath                 = nullptr;
            hitCI.Source                   = nullptr;
            hitCI.pShaderSourceStreamFactory = nullptr;
            hitCI.SourceLanguage           = Diligent::SHADER_SOURCE_LANGUAGE_BYTECODE;
            hitCI.ByteCode                 = blob.data();
            hitCI.ByteCodeSize             = (Diligent::Uint32)blob.size();
            Diligent::RefCntAutoPtr<Diligent::IShader> shader;
            Diligent::RefCntAutoPtr<Diligent::IDataBlob> output;
            dev->CreateShader(hitCI, &shader, &output);
            if (shader) {
                utils::info("shaderCache: hit %s", name);
                shader->AddRef();
                return shader;
            }
            utils::warn("shaderCache: unreadable blob for %s, recompiling", name);
        }
    }

    Diligent::RefCntAutoPtr<Diligent::IShader> shader;
    Diligent::RefCntAutoPtr<Diligent::IDataBlob> output;
    dev->CreateShader(ci, &shader, &output);
    if (!shader) {
        if (ppOutput && output)
            *ppOutput = output.Detach();
        return nullptr;
    }
    if (cacheEnabled) {
        const void* bytes = nullptr;
        Diligent::Uint64 size = 0;
        shader->GetBytecode(&bytes, size);
        if (bytes && size > 0) {
            if (writeWhole(path.c_str(), bytes, (size_t)size))
                utils::info("shaderCache: stored %s (%llu bytes)", name, (unsigned long long)size);
            else
                utils::warn("shaderCache: store failed %s", name);
        }
    }
    shader->AddRef();
    return shader;
}

extern "C" Diligent::RefCntAutoPtr<Diligent::IShader> fxShaderCacheCreate(Diligent::IRenderDevice* dev,
                                                                          Diligent::ShaderCreateInfo& ci) {
    return Diligent::RefCntAutoPtr<Diligent::IShader>(shaderCacheCreate(dev, ci, nullptr));
}

extern "C" Diligent::RefCntAutoPtr<Diligent::IPipelineState> fxPipelineStateCreate(
        Diligent::IRenderDevice* dev, Diligent::GraphicsPipelineStateCreateInfo& ci) {
    ci.pPSOCache = psoCache();
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    dev->CreateGraphicsPipelineState(ci, &pso);
    return pso;
}

extern "C" void fxPipelineStateCreateEx(Diligent::IRenderDevice* dev,
                                        Diligent::GraphicsPipelineStateCreateInfo& ci,
                                        Diligent::IPipelineState** ppPSO) {
    ci.pPSOCache = psoCache();
    dev->CreateGraphicsPipelineState(ci, ppPSO);
}

}
