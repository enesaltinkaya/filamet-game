#include "gltf/GltfInternal.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "logger/Logger.h"
#include "renderer/diligent/DiligentRenderer.h"
#include "renderer/RenderBackend.h"
#include "terrain/TerrainInternal.h"

#include <Graphics/GraphicsEngine/interface/Buffer.h>
#include <Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <Graphics/GraphicsEngine/interface/SwapChain.h>
#include <Graphics/GraphicsTools/interface/GraphicsUtilities.h>
#include <Graphics/GraphicsTools/interface/MapHelper.hpp>
#include <Common/interface/RefCntAutoPtr.hpp>

#include <GLTF_PBR_Renderer.hpp>
#include <GLTFLoader.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace Diligent {
namespace HLSL {
#include <Shaders/Common/public/BasicStructures.fxh>
#include <Shaders/PBR/public/PBR_Structures.fxh>
#include <Shaders/PBR/private/RenderPBR_Structures.fxh>
}
}  // namespace Diligent

namespace engine::gltf {
using namespace Diligent;
using engine::renderer::diligent::device;
using engine::renderer::diligent::context;
using engine::renderer::diligent::swapChain;

static std::unique_ptr<GLTF::Model> model;
static std::unique_ptr<GLTF::ModelTransforms> transforms;
static std::unique_ptr<GLTF_PBR_Renderer> pbrRenderer;
static RefCntAutoPtr<IBuffer> frameAttribsCB;
static RefCntAutoPtr<ITexture> iblIrradiance;
static RefCntAutoPtr<ITexture> iblPrefiltered;
static GLTF_PBR_Renderer::ModelResourceBindings modelBindings;
static bool bindingsValid = false;
static Uint32 sceneIndex = 0;
static BoundBox modelBounds;
static bool haveBounds = false;
static std::vector<const GLTF::Node*> namedNodes;

bool gltfInitDiligent(void) {
    if (!device) {
        utils::warn("gltf: renderer not initialized");
        return false;
    }

    GLTF_PBR_Renderer::CreateInfo rendererCI;
    rendererCI.NumRenderTargets = 1;
    rendererCI.RTVFormats[0] = swapChain->GetDesc().ColorBufferFormat;
    rendererCI.DSVFormat = swapChain->GetDesc().DepthBufferFormat;
    rendererCI.FrontCounterClockwise = true;  // glTF front faces are CCW
    rendererCI.PackMatrixRowMajor = true;     // memcpy C++ float4x4 into shader CBs
    rendererCI.MaxLightCount = 1;

    pbrRenderer = std::make_unique<GLTF_PBR_Renderer>(device, nullptr, context, rendererCI);
    CreateUniformBuffer(device, pbrRenderer->GetPRBFrameAttribsSize(), "PBR frame attribs", &frameAttribsCB);
    if (!frameAttribsCB) {
        utils::warn("gltf: frame attribs buffer failed");
        return false;
    }

    utils::info("gltf: initialized (diligent GLTF_PBR_Renderer)");
    return true;
}

bool gltfLoadDiligent(const char* pakPath) {
    if (!pbrRenderer) {
        utils::warn("gltf: not initialized");
        return false;
    }

    // serve the .glb straight from the pak — no temp files
    GLTF::ModelCreateInfo modelCI;
    modelCI.ComputeBoundingBoxes = true;
    modelCI.FileName = pakPath;
    modelCI.ReadWholeFileCallback = [](const char* path, std::vector<unsigned char>& data,
                                        std::string& error) -> bool {
        utils::String bytes = utils::dataManagerRead(path);
        if (!bytes.data) {
            error = std::string("cannot read ") + path;
            return false;
        }
        data.assign(reinterpret_cast<const unsigned char*>(bytes.data),
                reinterpret_cast<const unsigned char*>(bytes.data) + bytes.size);
        utils::stringDestroy(&bytes);
        return true;
    };

    try {
        model = std::make_unique<GLTF::Model>(device, context, modelCI);
    } catch (const std::exception& e) {
        utils::warn("gltf: GLTF::Model failed for %s (%s)", pakPath, e.what());
        return false;
    }

    sceneIndex = std::min<Uint32>(model->DefaultSceneId, (Uint32)model->Scenes.size() - 1);
    transforms = std::make_unique<GLTF::ModelTransforms>();
    model->ComputeTransforms(sceneIndex, *transforms, float4x4::Identity());
    modelBounds = model->ComputeBoundingBox(sceneIndex, *transforms);
    haveBounds = true;

    namedNodes.clear();
    if (sceneIndex < model->Scenes.size()) {
        for (const GLTF::Node* node : model->Scenes[sceneIndex].LinearNodes) {
            if (!node->Name.empty()) {
                namedNodes.push_back(node);
            }
        }
    }

    modelBindings = pbrRenderer->CreateResourceBindings(*model, frameAttribsCB);
    bindingsValid = true;

    utils::info("gltf: %s — %zu meshes, %zu animations, bounds [%.2f %.2f %.2f]-[%.2f %.2f %.2f]",
            pakPath, model->Meshes.size(), model->Animations.size(), modelBounds.Min.x,
            modelBounds.Min.y, modelBounds.Min.z, modelBounds.Max.x, modelBounds.Max.y, modelBounds.Max.z);
    return true;
}

// ── constant-environment IBL (the fallback PBR path's "IBL" is a flat sky) ──
// The engine's ambient (color * lux, exposure 1.2 * 2^-15, /pi) is baked into
// two 1x1x6 RGBA8 cubes: the irradiance map and a single-mip prefiltered env
// (a constant color has no roughness gradient, so one mip suffices). The PBR
// PSOs bind both slots, so they must always carry a valid resource.
static RefCntAutoPtr<ITexture> makeConstantCube(const char* name, const float rgb[3]) {
    TextureDesc desc;
    desc.Name = name;
    desc.Type = RESOURCE_DIM_TEX_CUBE;
    desc.Usage = USAGE_IMMUTABLE;
    desc.BindFlags = BIND_SHADER_RESOURCE;
    desc.Format = TEX_FORMAT_RGBA8_UNORM;
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 6;

    u8 px[24];
    for (int face = 0; face < 6; face++) {
        for (int c = 0; c < 3; c++) {
            px[face * 4 + c] = (u8)(std::min(1.0f, rgb[c]) * 255.0f);
        }
        px[face * 4 + 3] = 255;
    }
    TextureSubResData subres[6];
    for (int face = 0; face < 6; face++) {
        subres[face] = TextureSubResData(px + face * 4, 4);
    }
    RefCntAutoPtr<ITexture> texture;
    TextureData initData(subres, 6);
    device->CreateTexture(desc, &initData, &texture);
    return texture;
}

void gltfIblUpdateDiligent(const f32 color[3], f32 intensity) {
    if (!pbrRenderer) {
        return;
    }

    // match the terrain/filament ambient: color * (lux * exposure) / pi
    const float exposure = 1.2f * (float)std::exp2(-15.0);
    const float k = std::max(0.0f, intensity) * exposure * 0.318309886f;  // 1/pi
    float rgb[3] = {std::min(1.0f, color[0] * k), std::min(1.0f, color[1] * k),
            std::min(1.0f, color[2] * k)};

    iblIrradiance = makeConstantCube("IBL irradiance", rgb);
    iblPrefiltered = makeConstantCube("IBL prefiltered env", rgb);
    if (!iblIrradiance || !iblPrefiltered) {
        utils::warn("gltf: IBL cube creation failed");
        return;
    }

    // immutable textures start in the transfer layout; force the shader
    // layout now so the first draw (possibly the next frame) is valid
    StateTransitionDesc barrier{iblIrradiance, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE,
            STATE_TRANSITION_FLAG_UPDATE_STATE};
    context->TransitionResourceStates(1, &barrier);
    barrier = {iblPrefiltered, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE,
            STATE_TRANSITION_FLAG_UPDATE_STATE};
    context->TransitionResourceStates(1, &barrier);

    auto* irrSRV = iblIrradiance->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    auto* pfSRV = iblPrefiltered->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    for (auto& srb : modelBindings.MaterialSRB) {
        pbrRenderer->SetIBLResourceViews(srb, irrSRV, pfSRV);
    }
    utils::info("gltf: constant IBL set for %zu material SRB(s)", (size_t)modelBindings.MaterialSRB.size());
}

void gltfUpdateDiligent(double elapsedSeconds) {
    if (!model || !transforms) {
        return;
    }

    model->ComputeTransforms(sceneIndex, *transforms, float4x4::Identity(),
            model->Animations.empty() ? -1 : 0, (float)elapsedSeconds);
}

bool gltfBoundingBoxDiligent(float min[3], float max[3]) {
    if (!haveBounds) {
        return false;
    }
    min[0] = modelBounds.Min.x;
    min[1] = modelBounds.Min.y;
    min[2] = modelBounds.Min.z;
    max[0] = modelBounds.Max.x;
    max[1] = modelBounds.Max.y;
    max[2] = modelBounds.Max.z;
    return true;
}

size_t gltfEntitiesNamedDiligent(const char* prefix, u64* out, size_t cap) {
    size_t found = 0;
    for (const GLTF::Node* node : namedNodes) {
        if (utils::strStartsWith(node->Name.c_str(), prefix)) {
            if (found < cap) {
                out[found] = (u64)node->Index;
            }
            found++;
        }
    }
    return found;
}

size_t gltfDiligentMeshNodeCount(void) {
    if (!model || model->Scenes.empty()) {
        return 0;
    }
    size_t count = 0;
    for (const GLTF::Node* node : model->Scenes[sceneIndex].LinearNodes) {
        if (node->pMesh) {
            count++;
        }
    }
    return count;
}

// fills the shared PBR frame attribs (camera + sun); the terrain path uses its
// own constant buffers, this one drives GLTF_PBR_Renderer
static void fillFrameAttribs(IDeviceContext* ctx) {
    const float4x4& view = engine::renderer::diligent::diligentFrameView();
    const float4x4& proj = engine::renderer::diligent::diligentFrameProj();
    const SwapChainDesc& scDesc = swapChain->GetDesc();

    MapHelper<HLSL::PBRFrameAttribs> frame(ctx, frameAttribsCB, MAP_WRITE, MAP_FLAG_DISCARD);

    HLSL::CameraAttribs& camera = frame->Camera;
    camera.mView = view;
    camera.mProj = proj;
    camera.mViewProj = view * proj;
    camera.mViewInv = view.Inverse();
    camera.mProjInv = proj.Inverse();
    camera.mViewProjInv = camera.mViewProj.Inverse();
    camera.f4Position = float4(float3::MakeVector(camera.mViewInv[3]), 1.0f);
    camera.f4ViewportSize = float4{(float)scDesc.Width, (float)scDesc.Height,
            1.0f / (float)scDesc.Width, 1.0f / (float)scDesc.Height};
    camera.SetClipPlanes(engine::renderer::kCameraNear, engine::renderer::kCameraFar);
    camera.fHandness = view.Determinant() > 0 ? 1.0f : -1.0f;
    frame->PrevCamera = frame->Camera;

    HLSL::PBRRendererShaderParameters& renderer = frame->Renderer;
    renderer.OcclusionStrength = 1.0f;
    renderer.EmissionScale = 1.0f;
    renderer.AverageLogLum = 0.25f;
    renderer.MiddleGray = 0.18f;
    renderer.WhitePoint = 3.0f;
    renderer.IBLScale = float4{1.0f, 1.0f, 1.0f, 1.0f};  // constant env, see gltfIblUpdateDiligent
    renderer.HighlightColor = float4{1.0f, 0.0f, 0.0f, 0.0f};
    renderer.UnshadedColor = float4{0.5f, 0.5f, 0.5f, 1.0f};
    renderer.PointSize = 1.0f;
    renderer.MipBias = 0.0f;
    renderer.LightCount = 1;
    renderer.DebugView = 0;

    // directional sun right after the frame attribs (layout owned by the renderer)
    HLSL::PBRLightAttribs* light = reinterpret_cast<HLSL::PBRLightAttribs*>(
            static_cast<HLSL::PBRFrameAttribs*>(frame) + 1);    const f32* sunDir = engine::renderer::diligent::diligentSunDirection();
    const f32* sunColor = engine::renderer::diligent::diligentSunColor();
    f32 sunIntensity = engine::renderer::diligent::diligentSunIntensity();

    GLTF::Light sun;
    sun.Type = GLTF::Light::TYPE::DIRECTIONAL;
    sun.Color = float3{sunColor[0], sunColor[1], sunColor[2]};
    // scale the engine's physical lux (110k) into the renderer's shader range
    sun.Intensity = sunIntensity > 0.0f ? sunIntensity * (3.0f / 110000.0f) : 0.0f;
    float3 direction = normalize(float3{sunDir[0], sunDir[1], sunDir[2]});
    GLTF_PBR_Renderer::WritePBRLightShaderAttribs({&sun, nullptr, &direction, 1.0f}, light);
}

// internal accessors for the render hook below (statics stay file-local)
GLTF::Model* worldModel(void) { return model.get(); }
GLTF::ModelTransforms* worldTransforms(void) { return transforms.get(); }
GLTF_PBR_Renderer* worldPbrRenderer(void) { return pbrRenderer.get(); }
GLTF_PBR_Renderer::ModelResourceBindings* worldModelBindings(void) { return &modelBindings; }
Uint32 worldSceneIndex(void) { return sceneIndex; }

}  // namespace engine::gltf

namespace engine::renderer::diligent {

void worldDraw(Diligent::IDeviceContext* ctx) {
    using namespace engine::gltf;

    GLTF::Model* model = worldModel();
    GLTF::ModelTransforms* transforms = worldTransforms();
    if (!model || !transforms) {
        return;
    }

    engine::renderer::diligent::setWorldDrew(true);

    if (engine::terrain::terrainDiligentOwnsDrawing()) {
        engine::terrain::terrainDiligentDrawWorld(ctx, *model, *transforms);
        return;
    }

    fillFrameAttribs(ctx);

    GLTF_PBR_Renderer* pbr = worldPbrRenderer();
    pbr->Begin(ctx);
    GLTF_PBR_Renderer::RenderInfo renderInfo;
    renderInfo.SceneIndex = worldSceneIndex();
    renderInfo.AlphaModes = GLTF_PBR_Renderer::RenderInfo::ALPHA_MODE_FLAG_ALL;
    renderInfo.Flags = GLTF_PBR_Renderer::PSO_FLAG_DEFAULT;
    pbr->Render(ctx, *model, *transforms, nullptr, renderInfo, worldModelBindings());
}

}  // namespace engine::renderer::diligent

namespace engine::gltf {

void gltfDestroyDiligent(void) {
    modelBindings.Clear();
    bindingsValid = false;
    transforms.reset();
    model.reset();
    frameAttribsCB.Release();
    iblIrradiance.Release();
    iblPrefiltered.Release();
    pbrRenderer.reset();
    namedNodes.clear();
    haveBounds = false;
    utils::info("gltf: destroyed");
}
}  // namespace engine::gltf
