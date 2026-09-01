#include "Terrain.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "gltf/Gltf.h"
#include "json/Json.h"
#include "logger/Logger.h"
#include "renderer/Renderer.h"
#include "thread/Thread.h"

#include <basisu_transcoder.h>
#include <filament/Engine.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace engine::terrain {
using namespace filament;

static Material* material = nullptr;
static MaterialInstance* materialInstance = nullptr;
static Texture* splatTiles = nullptr;
static Texture* styleAlbedo = nullptr;
static Texture* styleNormal = nullptr;

static constexpr int kMaxGroups = 3;
static constexpr int kMaxTiles = 100;

static void bufferFree(void* buf, size_t, void*) {
    free(buf);
}

// ── KTX2 layers of a single 2D array texture ────────────────────────────────
// Each file contributes one layer; all files must share dimensions and level
// count (tiles are 1024x1024, styles are 2048x2048 — uniform per array).
// Two payload kinds, picked per file by vkFormat:
//   baked BC7 (VK_FORMAT_BC7_*_BLOCK, produced offline by scripts/ktx2bc7.c)
//     → level bytes are already BC7 blocks, copied and uploaded as-is
//   UASTC (build-terrain.py toktx output) → transcoded to BC7 here
// UASTC transcoding a layer takes tens of milliseconds of pure CPU (real BC7
// encoding per 4x4 block), so layers run in parallel on the default thread
// pool; filament uploads stay on the main thread (its API is not thread-safe
// by contract here).
struct LevelData {
    uint8_t* blocks;  // malloc'd BC7 blocks, handed to filament via bufferFree
    size_t byteCount;
    uint32_t width;
    uint32_t height;
};

struct LayerJob {
    const std::string* file;
    std::vector<LevelData> levels;
    uint32_t vkFormat = 0;
};

static constexpr uint32_t kVkFormatBC7Unorm = 145;
static constexpr uint32_t kVkFormatBC7Srgb = 146;

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd64(const uint8_t* p) {
    return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static void loadLayer(void* userData) {
    LayerJob* job = (LayerJob*)userData;

    utils::String data = utils::dataManagerRead(job->file->c_str());
    if (!data.data) {
        utils::warn("terrain: cannot read %s", job->file->c_str());
        return;
    }
    const uint8_t* bytes = (const uint8_t*)data.data;

    // minimal KTX2 container parse (little-endian, single 2D image)
    static const uint8_t ktx2Magic[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    if (data.size < 80 || memcmp(bytes, ktx2Magic, 12)) {
        utils::warn("terrain: %s is not a ktx2", job->file->c_str());
        utils::stringDestroy(&data);
        return;
    }
    const uint32_t vkFormat = rd32(bytes + 12);
    const uint32_t width = rd32(bytes + 20);
    const uint32_t height = rd32(bytes + 24);
    const uint32_t depth = rd32(bytes + 28);
    const uint32_t layerCount = rd32(bytes + 32);
    const uint32_t faceCount = rd32(bytes + 36);
    const uint32_t levelCount = rd32(bytes + 40);
    const uint32_t scheme = rd32(bytes + 44);
    if (!width || !height || depth || layerCount || faceCount != 1 || !levelCount ||
            (size_t)80 + 24u * levelCount > data.size) {
        utils::warn("terrain: %s is not a single 2D ktx2", job->file->c_str());
        utils::stringDestroy(&data);
        return;
    }
    job->vkFormat = vkFormat;

    if (vkFormat == kVkFormatBC7Unorm || vkFormat == kVkFormatBC7Srgb) {
        // offline-baked BC7: level payloads are raw blocks — copy and upload
        if (scheme != 0) {
            utils::warn("terrain: %s is baked BC7 but supercompressed", job->file->c_str());
            job->vkFormat = 0;
            utils::stringDestroy(&data);
            return;
        }
        job->levels.resize(levelCount);
        for (uint32_t level = 0; level < levelCount; level++) {
            const uint8_t* entry = bytes + 80 + 24u * level;
            const uint64_t offset = rd64(entry);
            const uint64_t byteCount = rd64(entry + 8);
            if (!byteCount || offset > data.size || byteCount > data.size - offset) {
                utils::warn("terrain: %s level %u out of bounds", job->file->c_str(), level);
                break;
            }
            uint8_t* blocks = (uint8_t*)malloc(byteCount);
            memcpy(blocks, bytes + offset, byteCount);
            job->levels[level] = {blocks, byteCount, std::max(1u, width >> level),
                    std::max(1u, height >> level)};
        }
        for (const LevelData& level : job->levels) {
            if (!level.blocks) {
                for (const LevelData& l : job->levels) {
                    free(l.blocks);
                }
                job->levels.clear();
                job->vkFormat = 0;
                break;
            }
        }
        utils::stringDestroy(&data);
        return;
    }

    basist::ktx2_transcoder ktx2;
    if (!ktx2.init((const void*)data.data, data.size) || !(ktx2.is_uastc() || ktx2.is_xuastc_ldr())) {
        utils::warn("terrain: %s is not a UASTC ktx2", job->file->c_str());
        utils::stringDestroy(&data);
        return;
    }
    if (!ktx2.start_transcoding()) {
        utils::warn("terrain: start_transcoding failed for %s", job->file->c_str());
        utils::stringDestroy(&data);
        return;
    }

    job->levels.resize(ktx2.get_levels());
    for (uint32_t level = 0; level < ktx2.get_levels(); level++) {
        basist::ktx2_image_level_info info;
        if (!ktx2.get_image_level_info(info, level, 0, 0)) {
            utils::warn("terrain: level info failed for %s", job->file->c_str());
            break;
        }

        // BC7: 16 bytes per 4x4 block
        const size_t byteCount = 16u * info.m_total_blocks;
        uint8_t* blocks = (uint8_t*)malloc(byteCount);
        if (!ktx2.transcode_image_level(level, 0, 0, blocks, info.m_total_blocks,
                    basist::transcoder_texture_format::cTFBC7_RGBA, 0)) {
            utils::warn("terrain: transcode failed for %s level %u", job->file->c_str(), level);
            free(blocks);
            break;
        }
        job->levels[level] = {blocks, byteCount, info.m_orig_width, info.m_orig_height};
    }

    // partial failure: release what transcoded, report the job as failed
    for (const LevelData& level : job->levels) {
        if (!level.blocks) {
            for (const LevelData& l : job->levels) {
                free(l.blocks);
            }
            job->levels.clear();
            break;
        }
    }
    utils::stringDestroy(&data);
}

static Texture* loadKtx2Array(const std::vector<std::string>& files, Texture::InternalFormat format) {
    Engine& engine = *renderer::filamentEngine;

    if (files.empty()) {
        return nullptr;
    }
    if (!Texture::isTextureFormatSupported(engine, format)) {
        utils::warn("terrain: BC7 not supported by backend");
        return nullptr;
    }

    const Texture::CompressedType compressedType =
            format == Texture::InternalFormat::SRGB_ALPHA_BPTC_UNORM
            ? Texture::CompressedType::SRGB_ALPHA_BPTC_UNORM
            : Texture::CompressedType::RGBA_BPTC_UNORM;

    // basisu lookup tables must exist before any transcode; gltfInit() happens
    // to do this via its Ktx2Provider, but the init is idempotent anyway
    basist::basisu_transcoder_init();

    std::vector<LayerJob> jobs(files.size());
    for (size_t layer = 0; layer < files.size(); layer++) {
        jobs[layer].file = &files[layer];
        utils::threadPoolAddWork(nullptr, loadLayer, &jobs[layer]);
    }
    utils::threadPoolWait(nullptr);

    // failure or dimension mismatch anywhere → reject the whole array
    for (size_t layer = 0; layer < jobs.size(); layer++) {
        if (jobs[layer].levels.empty() ||
                jobs[layer].vkFormat != jobs[0].vkFormat ||
                jobs[layer].levels.size() != jobs[0].levels.size() ||
                jobs[layer].levels[0].width != jobs[0].levels[0].width ||
                jobs[layer].levels[0].height != jobs[0].levels[0].height) {
            utils::warn("terrain: %s failed or differs from array", files[layer].c_str());
            for (LayerJob& job : jobs) {
                for (const LevelData& level : job.levels) {
                    free(level.blocks);
                }
            }
            return nullptr;
        }
    }

    // baked BC7 files must match the requested filament format (sRGB albedo
    // vs linear splat/normal) — catches a baked-with-wrong-flag pipeline bug
    // (UASTC payloads carry no BC7 vkFormat, so they are exempt)
    const bool wantSrgb = format == Texture::InternalFormat::SRGB_ALPHA_BPTC_UNORM;
    const bool baked = jobs[0].vkFormat == kVkFormatBC7Unorm || jobs[0].vkFormat == kVkFormatBC7Srgb;
    if (baked && (jobs[0].vkFormat == kVkFormatBC7Srgb) != wantSrgb) {
        utils::warn("terrain: baked vkFormat %u does not match %s request", jobs[0].vkFormat,
                wantSrgb ? "sRGB" : "linear");
        for (LayerJob& job : jobs) {
            for (const LevelData& level : job.levels) {
                free(level.blocks);
            }
        }
        return nullptr;
    }

    Texture* texture = Texture::Builder()
                               .width(jobs[0].levels[0].width)
                               .height(jobs[0].levels[0].height)
                               .levels((uint32_t)jobs[0].levels.size())
                               .depth((uint32_t)jobs.size())
                               .sampler(Texture::Sampler::SAMPLER_2D_ARRAY)
                               .format(format)
                               .build(engine);

    for (size_t layer = 0; layer < jobs.size(); layer++) {
        for (size_t level = 0; level < jobs[layer].levels.size(); level++) {
            const LevelData& l = jobs[layer].levels[level];
            Texture::PixelBufferDescriptor descriptor(l.blocks, l.byteCount, compressedType,
                    l.byteCount, bufferFree);
            texture->setImage(engine, (uint32_t)level, 0, 0, (uint32_t)layer, l.width, l.height, 1,
                    std::move(descriptor));
        }
    }

    return texture;
}

// ── manifest → material + texture arrays ────────────────────────────────────
bool terrainInit(const char* manifestPath) {
    Engine& engine = *renderer::filamentEngine;
    const double startNanos = utils::nanos();

    utils::String manifestData = utils::dataManagerRead(manifestPath);
    if (!manifestData.data) {
        utils::warn("terrain: cannot read manifest %s", manifestPath);
        return false;
    }

    json_t* root = jsonParse(manifestData.data);
    utils::stringDestroy(&manifestData);
    if (!root) {
        utils::warn("terrain: manifest parse failed");
        return false;
    }

    // material
    const char* materialPath = jsonGetString(root, "material");
    utils::String materialData = utils::dataManagerRead(materialPath);
    if (!materialData.data) {
        utils::warn("terrain: cannot read material %s", materialPath);
        json_decref(root);
        return false;
    }
    material = Material::Builder()
                       .package((const void*)materialData.data, materialData.size)
                       .build(engine);
    utils::stringDestroy(&materialData);
    if (!material) {
        utils::warn("terrain: material build failed");
        json_decref(root);
        return false;
    }

    // splat tile tables: group -> tile index -> layer (-1 = empty)
    int tileLayer[kMaxGroups][kMaxTiles];
    for (int g = 0; g < kMaxGroups; g++) {
        for (int t = 0; t < kMaxTiles; t++) {
            tileLayer[g][t] = -1;
        }
    }
    int styleRemap[12];
    for (int i = 0; i < 12; i++) {
        styleRemap[i] = -1;
    }

    const int udimGrid = jsonGetInt(root, "udimGrid");

    json_t* groups = jsonGetArray(root, "groups");
    std::vector<std::string> splatFiles;
    size_t groupCount = json_array_size(groups);
    if (groupCount > kMaxGroups) {
        groupCount = kMaxGroups;
    }

    for (size_t g = 0; g < groupCount; g++) {
        json_t* group = json_array_get(groups, g);

        json_t* tiles = jsonGetArray(group, "tiles");
        size_t tileCount = json_array_size(tiles);
        for (size_t i = 0; i < tileCount; i++) {
            json_t* tile = json_array_get(tiles, i);
            int udim = jsonGetInt(tile, "udim");
            int layer = jsonGetInt(tile, "layer");
            int u = (udim - 1001) % udimGrid;
            int v = (udim - 1001) / udimGrid;
            if (u < 0 || v < 0 || u >= udimGrid || v >= udimGrid) {
                continue;
            }
            tileLayer[g][u + udimGrid * v] = layer;

            const char* file = jsonGetString(tile, "file");
            if ((size_t)layer >= splatFiles.size()) {
                splatFiles.resize((size_t)layer + 1);
            }
            splatFiles[(size_t)layer] = file;
        }

        json_t* channels = jsonGetArray(group, "channels");
        for (int c = 0; c < 4 && (size_t)c < json_array_size(channels); c++) {
            styleRemap[g * 4 + c] = (int)json_integer_value(json_array_get(channels, c));
        }
    }

    // styles: albedo + normal layers in manifest order
    std::vector<std::string> albedoFiles;
    std::vector<std::string> normalFiles;
    json_t* styles = jsonGetArray(root, "styles");
    size_t styleCount = json_array_size(styles);
    for (size_t s = 0; s < styleCount; s++) {
        json_t* style = json_array_get(styles, s);
        albedoFiles.push_back(jsonGetString(style, "albedo"));
        normalFiles.push_back(jsonGetString(style, "normal"));
    }

    // build the three arrays (splat weights linear, albedo sRGB, normals linear)
    splatTiles = loadKtx2Array(splatFiles, Texture::InternalFormat::RGBA_BPTC_UNORM);
    styleAlbedo = loadKtx2Array(albedoFiles, Texture::InternalFormat::SRGB_ALPHA_BPTC_UNORM);
    styleNormal = loadKtx2Array(normalFiles, Texture::InternalFormat::RGBA_BPTC_UNORM);
    if (!splatTiles || !styleAlbedo || !styleNormal) {
        terrainDestroy();
        json_decref(root);
        return false;
    }

    materialInstance = material->createInstance();
    // splat tiles: clamp at layer edges; styles: repeat for world-space tiling
    TextureSampler splatSampler(TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
            TextureSampler::MagFilter::LINEAR, TextureSampler::WrapMode::CLAMP_TO_EDGE);
    TextureSampler styleSampler(TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
            TextureSampler::MagFilter::LINEAR, TextureSampler::WrapMode::REPEAT);
    materialInstance->setParameter("splatTiles", splatTiles, splatSampler);
    materialInstance->setParameter("styleAlbedo", styleAlbedo, styleSampler);
    materialInstance->setParameter("styleNormal", styleNormal, styleSampler);
    materialInstance->setParameter("tileLayer0", tileLayer[0], (size_t)kMaxTiles);
    materialInstance->setParameter("tileLayer1", tileLayer[1], (size_t)kMaxTiles);
    materialInstance->setParameter("tileLayer2", tileLayer[2], (size_t)kMaxTiles);
    materialInstance->setParameter("styleRemap", styleRemap, (size_t)12);
    materialInstance->setParameter("defaultStyle", (int)jsonGetInt(root, "defaultStyle"));
    materialInstance->setParameter("styleTiling", (float)jsonGetDouble(root, "styleTiling"));

    json_decref(root);

    utils::info("terrain: ready — %zu splat layers, %zu styles (%.0f ms)", splatFiles.size(),
            styleCount, (utils::nanos() - startNanos) / 1000000.0);
    return true;
}

void terrainApplyToAsset(void) {
    if (!materialInstance || !gltf::asset) {
        return;
    }

    Engine& engine = *renderer::filamentEngine;
    RenderableManager& rm = engine.getRenderableManager();

    utils::Entity chunks[kMaxTiles];
    size_t found = gltf::gltfEntitiesNamed("terrain_chunk_", chunks, kMaxTiles);

    size_t applied = 0;
    for (size_t i = 0; i < found && i < kMaxTiles; i++) {
        auto instance = rm.getInstance(chunks[i]);
        if (!instance) {
            continue;
        }
        size_t primitives = rm.getPrimitiveCount(instance);
        for (size_t p = 0; p < primitives; p++) {
            rm.setMaterialInstanceAt(instance, p, materialInstance);
        }
        applied++;
    }

    utils::info("terrain: material applied to %zu/%zu chunks", applied, found);
}

void terrainDestroy(void) {
    Engine* engine = renderer::filamentEngine;

    if (materialInstance) {
        engine->destroy(materialInstance);
        materialInstance = nullptr;
    }
    if (material) {
        engine->destroy(material);
        material = nullptr;
    }
    if (splatTiles) {
        engine->destroy(splatTiles);
        splatTiles = nullptr;
    }
    if (styleAlbedo) {
        engine->destroy(styleAlbedo);
        styleAlbedo = nullptr;
    }
    if (styleNormal) {
        engine->destroy(styleNormal);
        styleNormal = nullptr;
    }
}
}  // namespace engine::terrain
