#include "Terrain.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "gltf/Gltf.h"
#include "json/Json.h"
#include "logger/Logger.h"
#include "renderer/Renderer.h"

#include <basisu_transcoder.h>
#include <filament/Engine.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>

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

// ── KTX2 (UASTC) → BC7 layers of a single 2D array texture ─────────────────
// Each file contributes one layer; all files must share dimensions and level
// count (tiles are 1024x1024, styles are 2048x2048 — uniform per array).
static Texture* loadKtx2Array(const std::vector<std::string>& files, Texture::InternalFormat format) {
    Engine& engine = *renderer::filamentEngine;

    if (!Texture::isTextureFormatSupported(engine, format)) {
        utils::warn("terrain: BC7 not supported by backend");
        return nullptr;
    }

    const Texture::CompressedType compressedType =
            format == Texture::InternalFormat::SRGB_ALPHA_BPTC_UNORM
            ? Texture::CompressedType::SRGB_ALPHA_BPTC_UNORM
            : Texture::CompressedType::RGBA_BPTC_UNORM;

    Texture* texture = nullptr;

    for (size_t layer = 0; layer < files.size(); layer++) {
        utils::String data = utils::dataManagerRead(files[layer].c_str());
        if (!data.data) {
            utils::warn("terrain: cannot read %s", files[layer].c_str());
            return nullptr;
        }

        basist::ktx2_transcoder ktx2;
        if (!ktx2.init((const void*)data.data, data.size) || !(ktx2.is_uastc() || ktx2.is_xuastc_ldr())) {
            utils::warn("terrain: %s is not a UASTC ktx2", files[layer].c_str());
            utils::stringDestroy(&data);
            engine.destroy(texture);
            return nullptr;
        }

        if (!texture) {
            texture = Texture::Builder()
                              .width(ktx2.get_width())
                              .height(ktx2.get_height())
                              .levels(ktx2.get_levels())
                              .depth((uint32_t)files.size())
                              .sampler(Texture::Sampler::SAMPLER_2D_ARRAY)
                              .format(format)
                              .build(engine);
        } else if (texture->getWidth() != ktx2.get_width() || texture->getHeight() != ktx2.get_height() ||
                   texture->getLevels() != ktx2.get_levels()) {
            utils::warn("terrain: %s dimensions differ from array", files[layer].c_str());
            utils::stringDestroy(&data);
            engine.destroy(texture);
            return nullptr;
        }

        if (!ktx2.start_transcoding()) {
            utils::warn("terrain: start_transcoding failed for %s", files[layer].c_str());
            utils::stringDestroy(&data);
            engine.destroy(texture);
            return nullptr;
        }

        for (uint32_t level = 0; level < ktx2.get_levels(); level++) {
            basist::ktx2_image_level_info info;
            if (!ktx2.get_image_level_info(info, level, 0, 0)) {
                utils::warn("terrain: level info failed for %s", files[layer].c_str());
                utils::stringDestroy(&data);
                engine.destroy(texture);
                return nullptr;
            }

            // BC7: 16 bytes per 4x4 block
            const size_t byteCount = 16u * info.m_total_blocks;
            uint8_t* blocks = (uint8_t*)malloc(byteCount);
            if (!ktx2.transcode_image_level(level, 0, 0, blocks, info.m_total_blocks,
                        basist::transcoder_texture_format::cTFBC7_RGBA, 0)) {
                utils::warn("terrain: transcode failed for %s level %u", files[layer].c_str(), level);
                free(blocks);
                utils::stringDestroy(&data);
                engine.destroy(texture);
                return nullptr;
            }

            Texture::PixelBufferDescriptor descriptor(blocks, byteCount, compressedType, byteCount,
                    bufferFree);
            texture->setImage(engine, level, 0, 0, (uint32_t)layer, info.m_orig_width,
                    info.m_orig_height, 1, std::move(descriptor));
        }

        utils::stringDestroy(&data);
    }

    return texture;
}

// ── manifest → material + texture arrays ────────────────────────────────────
bool terrainInit(const char* manifestPath) {
    Engine& engine = *renderer::filamentEngine;

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

    utils::info("terrain: ready — %zu splat layers, %zu styles", splatFiles.size(), styleCount);
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
