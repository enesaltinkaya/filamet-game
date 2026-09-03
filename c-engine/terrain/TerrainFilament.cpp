#include "terrain/TerrainInternal.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "gltf/Gltf.h"
#include "logger/Logger.h"
#include "renderer/filament/FilamentRenderer.h"

#include <filament/Engine.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <utils/Entity.h>

namespace engine::terrain {
using namespace filament;

using engine::renderer::filament_globals::engine;

static Material* material = nullptr;
static MaterialInstance* materialInstance = nullptr;
static Texture* splatTiles = nullptr;
static Texture* styleAlbedo = nullptr;
static Texture* styleNormal = nullptr;
static Texture* defaultAlbedo = nullptr;
static Texture* defaultNormal = nullptr;

static void bufferFree(void* buf, size_t, void*) {
    free(buf);
}

bool terrainStartFilament(const char* materialPath) {
    if (!engine) {
        utils::warn("terrain: renderer not initialized");
        return false;
    }

    utils::String materialData = utils::dataManagerRead(materialPath);
    if (!materialData.data) {
        utils::warn("terrain: cannot read material %s", materialPath);
        return false;
    }
    material = Material::Builder()
                       .package((const void*)materialData.data, materialData.size)
                       .build(*engine);
    utils::stringDestroy(&materialData);
    if (!material) {
        utils::warn("terrain: material build failed");
        return false;
    }
    return true;
}

static Texture::InternalFormat internalFormat(const TerrainDecodedArray& array) {
    if (array.rgba8) {
        return array.srgb ? Texture::InternalFormat::SRGB8_A8 : Texture::InternalFormat::RGBA8;
    }
    return array.srgb ? Texture::InternalFormat::SRGB_ALPHA_BPTC_UNORM
                      : Texture::InternalFormat::RGBA_BPTC_UNORM;
}

bool terrainArrayFilament(TerrainArrayKind kind, TerrainDecodedArray& array) {
    if (array.layers.empty()) {
        return false;
    }
    if (!Texture::isTextureFormatSupported(*engine, internalFormat(array))) {
        utils::warn("terrain: %s not supported by backend",
                array.rgba8 ? "RGBA8" : "BC7");
        return false;
    }

    Texture* texture = Texture::Builder()
                               .width(array.layers[0][0].width)
                               .height(array.layers[0][0].height)
                               .levels((uint32_t)array.layers[0].size())
                               .depth((uint32_t)array.layers.size())
                               .sampler(Texture::Sampler::SAMPLER_2D_ARRAY)
                               .format(internalFormat(array))
                               .build(*engine);
    if (!texture) {
        return false;
    }

    // ownership of the blocks transfers to the upload descriptors (freed by
    // bufferFree once the GPU consumed them). Compressed levels carry the
    // whole-level size as "stride"; uncompressed RGBA8 needs a per-row pixel
    // stride and an uncompressed descriptor
    for (size_t layer = 0; layer < array.layers.size(); layer++) {
        for (size_t level = 0; level < array.layers[layer].size(); level++) {
            TerrainLevelBlocks& l = array.layers[layer][level];
            Texture::PixelBufferDescriptor descriptor;
            if (array.rgba8) {
                descriptor = Texture::PixelBufferDescriptor(l.blocks, l.byteCount,
                        Texture::Format::RGBA, Texture::Type::UBYTE, 4u, 0u, 0u,
                        l.width, bufferFree);
            } else {
                const Texture::CompressedType compressedType = array.srgb
                        ? Texture::CompressedType::SRGB_ALPHA_BPTC_UNORM
                        : Texture::CompressedType::RGBA_BPTC_UNORM;
                descriptor = Texture::PixelBufferDescriptor(l.blocks, l.byteCount,
                        compressedType, l.byteCount, bufferFree);
            }
            texture->setImage(*engine, (uint32_t)level, 0, 0, (uint32_t)layer, l.width, l.height, 1,
                    std::move(descriptor));
            l.blocks = nullptr;
        }
    }
    array.layers.clear();

    switch (kind) {
        case TerrainArrayKind::SplatTiles: splatTiles = texture; break;
        case TerrainArrayKind::StyleAlbedo: styleAlbedo = texture; break;
        case TerrainArrayKind::StyleNormal: styleNormal = texture; break;
        case TerrainArrayKind::DefaultAlbedo: defaultAlbedo = texture; break;
        case TerrainArrayKind::DefaultNormal: defaultNormal = texture; break;
    }
    return true;
}

bool terrainFinishFilament(const TerrainParams& params) {
    if (!splatTiles || !styleAlbedo || !styleNormal || !defaultAlbedo || !defaultNormal) {
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
    materialInstance->setParameter("defaultAlbedo", defaultAlbedo, styleSampler);
    materialInstance->setParameter("defaultNormal", defaultNormal, styleSampler);
    materialInstance->setParameter("tileLayer0", (const int*)params.tileLayer[0], (size_t)TerrainParams::kMaxTiles);
    materialInstance->setParameter("tileLayer1", (const int*)params.tileLayer[1], (size_t)TerrainParams::kMaxTiles);
    materialInstance->setParameter("tileLayer2", (const int*)params.tileLayer[2], (size_t)TerrainParams::kMaxTiles);
    materialInstance->setParameter("styleRemap", (const int*)params.styleRemap, (size_t)12);
    materialInstance->setParameter("sandHeight", params.sandHeight);
    materialInstance->setParameter("sandFade", params.sandFade);
    materialInstance->setParameter("snowHeight", params.snowHeight);
    materialInstance->setParameter("snowFade", params.snowFade);
    materialInstance->setParameter("cliffSlope", params.cliffSlope);
    materialInstance->setParameter("cliffFade", params.cliffFade);
    materialInstance->setParameter("styleTiling", params.styleTiling);
    return true;
}

void terrainApplyFilament(void) {
    if (!materialInstance) {
        return;
    }

    RenderableManager& rm = engine->getRenderableManager();

    u64 chunks[TerrainParams::kMaxTiles];
    size_t found = gltf::gltfEntitiesNamed("terrain_chunk_", chunks, TerrainParams::kMaxTiles);

    size_t applied = 0;
    for (size_t i = 0; i < found && i < TerrainParams::kMaxTiles; i++) {
        utils::Entity entity = utils::Entity::import((int32_t)chunks[i]);
        auto instance = rm.getInstance(entity);
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

void terrainDestroyFilament(void) {
    Engine* enginePtr = engine;

    if (materialInstance) {
        enginePtr->destroy(materialInstance);
        materialInstance = nullptr;
    }
    if (material) {
        enginePtr->destroy(material);
        material = nullptr;
    }
    if (splatTiles) {
        enginePtr->destroy(splatTiles);
        splatTiles = nullptr;
    }
    if (styleAlbedo) {
        enginePtr->destroy(styleAlbedo);
        styleAlbedo = nullptr;
    }
    if (styleNormal) {
        enginePtr->destroy(styleNormal);
        styleNormal = nullptr;
    }
    if (defaultAlbedo) {
        enginePtr->destroy(defaultAlbedo);
        defaultAlbedo = nullptr;
    }
    if (defaultNormal) {
        enginePtr->destroy(defaultNormal);
        defaultNormal = nullptr;
    }
}
}  // namespace engine::terrain
