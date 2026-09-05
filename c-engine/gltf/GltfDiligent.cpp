#include "gltf/GltfInternal.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "logger/Logger.h"
#include "renderer/diligent/DiligentRenderer.h"
#include "renderer/RenderBackend.h"

#include <Graphics/GraphicsEngine/interface/Buffer.h>
#include <Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <Graphics/GraphicsEngine/interface/SwapChain.h>
#include <Graphics/GraphicsTools/interface/GraphicsUtilities.h>
#include <Graphics/GraphicsTools/interface/MapHelper.hpp>
#include <Common/interface/RefCntAutoPtr.hpp>

#include <GLTF_PBR_Renderer.hpp>
#include <GLTFLoader.hpp>

#include <zstd.h>

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
static BoundBox localBounds;
static bool haveBounds = false;

// ── Placement (gltfPlaceAt / gltfPlaceAtFacing) ─────────────────────────────
// The feet anchor is the min corner of the LOCAL AABB (the header contract:
// "min corner lands at (x, y, z)", yaw pivoted on the feet). Root matrix
// (row-vector convention, applied left to right):
//     p_local → (p - minc) → yaw → +(pos - minc_as_double)
// so minc lands exactly at pos for every yaw (lessons 2026-09-04: a pivot on
// any other local point swings the visible model around the target).
static double placePos[3] = {0.0, 0.0, 0.0};
static f32 placeYaw = 0.0f;
static char placementDirty = true;
static float4x4 placementMatrix = float4x4::Identity();

static const float4x4& placementRootMatrix(void) {
    if (placementDirty && haveBounds) {
        placementDirty = false;
        const float3 minc = localBounds.Min;
        const float3 offset{(float)(placePos[0] - minc.x), (float)(placePos[1] - minc.y),
                (float)(placePos[2] - minc.z)};
        placementMatrix = float4x4::Translation(-minc) * float4x4::RotationY(placeYaw) *
                          float4x4::Translation(offset);
    }
    return placementMatrix;
}

// ── Animation playback (clips live in a separate animation-source asset) ────
// gltfUpdate samples the active clip (and, during a crossfade, the clip being
// blended out), blends the per-node TRS, then rebuilds the visible model's
// local → global → joint matrices with the placement root. The pose is driven
// ONLY by the anim source; the visible model itself carries no clips.
static std::unique_ptr<GLTF::Model> animSource;
static std::unique_ptr<GLTF::ModelTransforms> animPoseA;  // active clip TRS
static std::unique_ptr<GLTF::ModelTransforms> animPoseB;  // blend-out clip TRS
static Uint32 animSourceSceneIndex = 0;
static char animNodeCountsMatch = 0;

struct ClipPlay {
    int index = -1;  // index in the animation source's Animations
    f32 time = 0.0f;
    f32 speed = 1.0f;
    bool loop = true;
};
static ClipPlay active;
static ClipPlay blendFrom;
static f32 blendElapsed = 0.0f;
static f32 blendDuration = 0.0f;
static char playing = 0;

// seconds since animation start, wrapped/clamped into the clip's key range
static f32 clipResolveTime(const GLTF::Animation& anim, f32 time) {
    if (anim.End <= anim.Start) {
        return anim.Start;
    }
    if (time < anim.Start) {
        return anim.Start;
    }
    if (time > anim.End) {
        return anim.End;  // non-looping clips hold their last frame
    }
    return time;
}

static void clipAdvance(ClipPlay& clip, const GLTF::Model& src, f32 dt) {
    const GLTF::Animation& anim = src.Animations[clip.index];
    clip.time += dt * clip.speed;
    if (clip.loop && anim.End > anim.Start) {
        const f32 len = anim.End - anim.Start;
        clip.time = anim.Start + std::fmod(clip.time - anim.Start, len);
        if (clip.time < anim.Start) {
            clip.time += len;
        }
    }
}

// Sample one clip's per-node TRS into out.NodeAnimations (indexed by the
// global node index). Un-animated components keep the node's static TRS —
// which is exactly what the export pipeline standardizes constant channels to
// (scripts/gltf-singlekey-fix.py), so skipping single-key LINEAR samplers
// (Diligent's own behaviour) is safe.
static void clipSampleTRS(const GLTF::Model& src, Uint32 sceneIdx, const ClipPlay& clip,
        GLTF::ModelTransforms& out) {
    if (sceneIdx >= src.Scenes.size() || clip.index < 0 ||
        (size_t)clip.index >= src.Animations.size()) {
        return;
    }
    const GLTF::Scene& scene = src.Scenes[sceneIdx];
    if (out.NodeAnimations.size() != src.Nodes.size()) {
        out.NodeAnimations.resize(src.Nodes.size());
    }
    for (const GLTF::Node* n : scene.LinearNodes) {
        GLTF::ModelTransforms::AnimationTransforms& A = out.NodeAnimations[n->Index];
        A.Translation = n->Translation;
        A.Rotation = n->Rotation;
        A.Scale = n->Scale;
    }

    const GLTF::Animation& animation = src.Animations[clip.index];
    const f32 time = clipResolveTime(animation, clip.time);
    for (const GLTF::AnimationChannel& channel : animation.Channels) {
        const GLTF::AnimationSampler& sampler = animation.Samplers[channel.SamplerIndex];
        if (sampler.Inputs.size() > sampler.OutputsVec4.size() ||
            (sampler.Interpolation == GLTF::AnimationSampler::INTERPOLATION_TYPE::LINEAR &&
                    sampler.Inputs.size() < 2)) {
            continue;
        }

        GLTF::ModelTransforms::AnimationTransforms& NodeAnim = out.NodeAnimations[channel.pNode->Index];
        size_t idx = sampler.FindKeyFrame(time);
        f32 u = 0.0f;
        if (sampler.Interpolation == GLTF::AnimationSampler::INTERPOLATION_TYPE::LINEAR) {
            idx = std::min(idx, sampler.Inputs.size() - 2);
            u = (time - sampler.Inputs[idx]) / (sampler.Inputs[idx + 1] - sampler.Inputs[idx]);
        }
        u = std::clamp(u, 0.0f, 1.0f);

        switch (channel.PathType) {
            case GLTF::AnimationChannel::PATH_TYPE::TRANSLATION:
                NodeAnim.Translation = lerp(float3{sampler.OutputsVec4[idx]},
                        float3{sampler.OutputsVec4[idx + 1]}, u);
                break;
            case GLTF::AnimationChannel::PATH_TYPE::SCALE:
                NodeAnim.Scale = lerp(float3{sampler.OutputsVec4[idx]},
                        float3{sampler.OutputsVec4[idx + 1]}, u);
                break;
            case GLTF::AnimationChannel::PATH_TYPE::ROTATION: {
                QuaternionF q1;
                q1.q.x = sampler.OutputsVec4[idx].x;
                q1.q.y = sampler.OutputsVec4[idx].y;
                q1.q.z = sampler.OutputsVec4[idx].z;
                q1.q.w = sampler.OutputsVec4[idx].w;
                QuaternionF q2;
                q2.q.x = sampler.OutputsVec4[idx + 1].x;
                q2.q.y = sampler.OutputsVec4[idx + 1].y;
                q2.q.z = sampler.OutputsVec4[idx + 1].z;
                q2.q.w = sampler.OutputsVec4[idx + 1].w;
                NodeAnim.Rotation = normalize(slerp(q1, q2, u));
                break;
            }
            case GLTF::AnimationChannel::PATH_TYPE::WEIGHTS:
                break;  // morph weights not used by the character assets
        }
    }
}

// Blend the two sampled TRS sets into the visible model's NodeAnimations
// (weight w = active clip share), then rebuild local → global → joint matrices
// with the placement root. Mirrors Model::ComputeTransforms' post-animation
// stages (UpdateNodeGlobalTransform + joint matrices are file-local there).
// poseA/poseB may be null or undersized (e.g. the no-clip path re-derives the
// pose from out's own last-applied TRS, statics before the first sample).
static void posePipeline(GLTF::Model& dst, GLTF::ModelTransforms& out,
        const GLTF::ModelTransforms* poseA, const GLTF::ModelTransforms* poseB, f32 w,
        const float4x4& root) {
    if (sceneIndex >= dst.Scenes.size()) {
        return;
    }
    const GLTF::Scene& scene = dst.Scenes[sceneIndex];

    out.NodeAnimations.resize(dst.Nodes.size());
    out.NodeLocalMatrices.resize(dst.Nodes.size());
    out.NodeGlobalMatrices.resize(dst.Nodes.size());
    const bool haveA = poseA && poseA->NodeAnimations.size() == dst.Nodes.size() && poseA != &out;
    const bool haveB = poseB && poseB->NodeAnimations.size() == dst.Nodes.size() && w < 1.0f;
    for (const GLTF::Node* n : scene.LinearNodes) {
        GLTF::ModelTransforms::AnimationTransforms blended;
        if (haveA) {
            blended = poseA->NodeAnimations[n->Index];
        } else {
            blended.Translation = n->Translation;
            blended.Rotation = n->Rotation;
            blended.Scale = n->Scale;
        }
        if (haveB) {
            const GLTF::ModelTransforms::AnimationTransforms& B = poseB->NodeAnimations[n->Index];
            blended.Translation = lerp(B.Translation, blended.Translation, w);
            blended.Scale = lerp(B.Scale, blended.Scale, w);
            blended.Rotation = normalize(slerp(B.Rotation, blended.Rotation, w));
        }
        out.NodeAnimations[n->Index] = blended;
        out.NodeLocalMatrices[n->Index] =
                GLTF::ComputeNodeLocalMatrix(blended.Scale, blended.Rotation, blended.Translation, n->Matrix);
    }

    // global matrices (row-vector convention: global = local * parent)
    struct Frame {
        const GLTF::Node* node;
        const float4x4* parent;
    };
    constexpr int kMaxStack = 256;
    Frame stack[kMaxStack];
    int sp = 0;
    for (const GLTF::Node* root_ : scene.RootNodes) {
        stack[sp++] = {root_, &root};
    }
    while (sp > 0) {
        const Frame f = stack[--sp];
        float4x4& global = out.NodeGlobalMatrices[f.node->Index];
        global = out.NodeLocalMatrices[f.node->Index] * (*f.parent);
        for (const GLTF::Node* child : f.node->Children) {
            if (sp < kMaxStack) {
                stack[sp++] = {child, &global};
            }
        }
    }

    // joint matrices (skinning)
    out.Skins.resize(dst.SkinTransformsCount);
    for (const GLTF::Node* pNode : scene.LinearNodes) {
        const GLTF::Mesh* pMesh = pNode->pMesh;
        const GLTF::Skin* pSkin = pNode->pSkin;
        if (pMesh == nullptr || pSkin == nullptr) {
            continue;
        }
        const float4x4& nodeGlobal = out.NodeGlobalMatrices[pNode->Index];
        std::vector<float4x4>& jointMatrices = out.Skins[pNode->SkinTransformsIndex].JointMatrices;
        if (jointMatrices.size() != pSkin->Joints.size()) {
            jointMatrices.resize(pSkin->Joints.size());
        }
        const float4x4 inverseTransform = nodeGlobal.Inverse();
        for (size_t i = 0; i < pSkin->Joints.size(); i++) {
            jointMatrices[i] =
                    pSkin->InverseBindMatrices[i] * out.NodeGlobalMatrices[pSkin->Joints[i]->Index] *
                    inverseTransform;
        }
    }
}

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
    rendererCI.MaxJointCount = 128;  // default 64 < eve's 65 joints (would clip silently)

    pbrRenderer = std::make_unique<GLTF_PBR_Renderer>(device, nullptr, context, rendererCI);
    CreateUniformBuffer(device, pbrRenderer->GetPRBFrameAttribsSize(), "PBR frame attribs", &frameAttribsCB);
    if (!frameAttribsCB) {
        utils::warn("gltf: frame attribs buffer failed");
        return false;
    }

    utils::info("gltf: initialized (diligent GLTF_PBR_Renderer)");
    return true;
}

// pak assets may be plain glb or zstd-compressed glb (sniffed by magic).
// tinygltf picks its GLB-vs-JSON parser from the FileName EXTENSION, so the
// loader stages a "<path>.glb" name while the callback serves the real pak
// entry (g_currentModelPakPath) — models ship as <name>.zstd (compressed glb).
static const char* g_currentModelPakPath = nullptr;

static bool readModelBytes(const char* path, std::vector<unsigned char>& data, std::string& error) {
    utils::String bytes = utils::dataManagerRead(path);
    if (!bytes.data) {
        error = std::string("cannot read ") + path;
        return false;
    }
    static const unsigned char kZstdMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};
    if (bytes.size >= 4 && std::memcmp(bytes.data, kZstdMagic, 4) == 0) {
        const unsigned long long raw = ZSTD_getFrameContentSize(bytes.data, bytes.size);
        if (raw == ZSTD_CONTENTSIZE_ERROR || raw == ZSTD_CONTENTSIZE_UNKNOWN) {
            error = std::string("bad zstd frame in ") + path;
            return false;
        }
        std::vector<unsigned char> decompressed((size_t)raw);
        const size_t written =
                ZSTD_decompress(decompressed.data(), decompressed.size(), bytes.data, bytes.size);
        utils::stringDestroy(&bytes);
        if (ZSTD_isError(written)) {
            error = std::string("zstd decompress failed: ") + ZSTD_getErrorName(written);
            return false;
        }
        decompressed.resize(written);
        data = std::move(decompressed);
        return true;
    }
    data.assign(reinterpret_cast<const unsigned char*>(bytes.data),
            reinterpret_cast<const unsigned char*>(bytes.data) + bytes.size);
    utils::stringDestroy(&bytes);
    return true;
}

static std::unique_ptr<GLTF::Model> loadModelBytes(const char* pakPath, std::string& error,
        IRenderDevice* loadDevice = nullptr, IDeviceContext* loadContext = nullptr) {
    GLTF::ModelCreateInfo modelCI;
    modelCI.ComputeBoundingBoxes = true;
    const std::string stagedName = std::string(pakPath) + ".glb";
    modelCI.FileName = stagedName.c_str();
    g_currentModelPakPath = pakPath;
    modelCI.ReadWholeFileCallback =
            [](const char* /*path*/, std::vector<unsigned char>& data, std::string& cbError) -> bool {
        return readModelBytes(g_currentModelPakPath, data, cbError);
    };
    try {
        std::unique_ptr<GLTF::Model> loaded;
        if (loadDevice) {
            loaded = std::make_unique<GLTF::Model>(loadDevice, loadContext, modelCI);
        } else {
            // CPU-only parse: the plain Model(CI) ctor never reads the file;
            // null device/context runs the full load but skips GPU resources
            // (textures, vertex/index buffers) — right for the animation source
            loaded = std::make_unique<GLTF::Model>(nullptr, nullptr, modelCI);
        }
        g_currentModelPakPath = nullptr;
        return loaded;
    } catch (const std::exception& e) {
        g_currentModelPakPath = nullptr;
        error = e.what();
        return nullptr;
    }
}

bool gltfLoadDiligent(const char* pakPath) {
    if (!pbrRenderer) {
        utils::warn("gltf: not initialized");
        return false;
    }

    std::string error;
    model = loadModelBytes(pakPath, error, device, context);
    if (!model) {
        utils::warn("gltf: GLTF::Model failed for %s (%s)", pakPath, error.c_str());
        return false;
    }

    sceneIndex = std::min<Uint32>(model->DefaultSceneId, (Uint32)model->Scenes.size() - 1);
    transforms = std::make_unique<GLTF::ModelTransforms>();
    model->ComputeTransforms(sceneIndex, *transforms, float4x4::Identity());
    localBounds = model->ComputeBoundingBox(sceneIndex, *transforms);
    haveBounds = true;
    placementDirty = true;

    if (getenv("ENGINE_GLTF_DEBUG")) {
        const GLTF::Scene& sc = model->Scenes[sceneIndex];
        utils::info("glb: nodes %zu, scenes %zu, roots %zu, linear %zu, meshes %zu, skins %zu",
                model->Nodes.size(), model->Scenes.size(), sc.RootNodes.size(), sc.LinearNodes.size(),
                model->Meshes.size(), model->Skins.size());
        for (size_t i = 0; i < model->Nodes.size() && i < 8; i++) {
            const GLTF::Node* n = &model->Nodes[i];
            utils::info("glb: node %d '%s' mesh %d skin %d parent %d", n->Index, n->Name.c_str(),
                    n->pMesh ? 1 : 0, n->pSkin ? 1 : 0, n->Parent ? n->Parent->Index : -1);
        }
        if (!model->Meshes.empty()) {
            const GLTF::Mesh& m = model->Meshes[0];
            utils::info("glb: mesh BB [%.3f %.3f %.3f]-[%.3f %.3f %.3f] valid %d, prims %zu",
                    m.BB.Min.x, m.BB.Min.y, m.BB.Min.z, m.BB.Max.x, m.BB.Max.y, m.BB.Max.z,
                    m.IsValidBB() ? 1 : 0, m.Primitives.size());
        }
        const size_t mi = model->Scenes[sceneIndex].LinearNodes.size();
        for (size_t i = 0; i < mi; i++) {
            const GLTF::Node* n = model->Scenes[sceneIndex].LinearNodes[i];
            if (n->pMesh) {
                const float4x4& g = transforms->NodeGlobalMatrices[n->Index];
                utils::info("glb: mesh node lin %zu idx %d global row3 %.4f %.4f %.4f %.4f", i,
                        n->Index, g._31, g._32, g._33, g._34);
            }
        }
    }

    modelBindings = pbrRenderer->CreateResourceBindings(*model, frameAttribsCB);
    bindingsValid = true;

    utils::info("gltf: %s — %zu meshes, %zu animations, bounds [%.2f %.2f %.2f]-[%.2f %.2f %.2f]",
            pakPath, model->Meshes.size(), model->Animations.size(), localBounds.Min.x,
            localBounds.Min.y, localBounds.Min.z, localBounds.Max.x, localBounds.Max.y,
            localBounds.Max.z);
    return true;
}

bool gltfPlaceAtDiligent(double x, double y, double z) {
    if (!model) {
        return false;
    }
    placePos[0] = x;
    placePos[1] = y;
    placePos[2] = z;
    placementDirty = true;
    return true;
}

bool gltfPlaceAtFacingDiligent(double x, double y, double z, f32 yaw) {
    placeYaw = yaw;
    return gltfPlaceAtDiligent(x, y, z);
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

    // ambient contract: color * (lux * exposure) / pi
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
    if (!model || !transforms || !haveBounds) {
        return;
    }

    if (playing && active.index >= 0) {
        const GLTF::Model& src = animSource ? *animSource : *model;
        const Uint32 srcScene = animSource ? animSourceSceneIndex : sceneIndex;
        const f32 dt = (f32)elapsedSeconds;
        clipAdvance(active, src, dt);
        if (blendDuration > 0.0f && blendFrom.index >= 0) {
            clipAdvance(blendFrom, src, dt);
            blendElapsed += dt;
            if (blendElapsed >= blendDuration) {
                blendDuration = 0.0f;
                blendFrom.index = -1;
            }
        }

        clipSampleTRS(src, srcScene, active, *animPoseA);
        GLTF::ModelTransforms* poseB = nullptr;
        f32 w = 1.0f;
        if (blendDuration > 0.0f && blendFrom.index >= 0) {
            clipSampleTRS(src, srcScene, blendFrom, *animPoseB);
            poseB = animPoseB.get();
            w = std::clamp(blendElapsed / blendDuration, 0.0f, 1.0f);
        }
        const float4x4& root = placementRootMatrix();
        posePipeline(*model, *transforms, animPoseA.get(), poseB, w, root);
        static int dbgFrames = 0;
        if (getenv("ENGINE_GLTF_DEBUG") && dbgFrames++ < 3) {
            utils::info("pose: root row4 %.2f %.2f %.2f %.2f", root._41, root._42, root._43, root._44);
            utils::info("pose: clips — active '%s' t %.2f (idx %d), blend %d", src.Animations[active.index].Name.c_str(),
                    active.time, active.index, (int)(poseB != nullptr));
            const GLTF::Skin& skin = model->Skins[0];
            const GLTF::Node* meshNode = model->Scenes[sceneIndex].LinearNodes[1];
            utils::info("pose: mesh node skin idx %d, model skins %zu, skin joints %zu",
                    meshNode->SkinTransformsIndex, transforms->Skins.size(), skin.Joints.size());
            // walk up from joint0 to the root, printing local/global validity
            const GLTF::Node* chain = skin.Joints[0];
            for (const GLTF::Node* n = chain; n; n = n->Parent) {
                const float4x4& lm = transforms->NodeLocalMatrices[n->Index];
                const float4x4& gm = transforms->NodeGlobalMatrices[n->Index];
                char nanL = std::isnan(lm._11) || std::isnan(lm._42) || std::isnan(lm._33) ? 'Y' : 'n';
                char nanG = std::isnan(gm._11) || std::isnan(gm._42) || std::isnan(gm._33) ? 'Y' : 'n';
                utils::info("pose: chain '%s' local nan %c [%.4f %.4f %.4f | %.4f] global nan %c [%.2f %.2f %.2f | %.2f]",
                        n->Name.c_str(), nanL, lm._11, lm._21, lm._31, lm._41, nanG, gm._11, gm._21, gm._31, gm._41);
                if (nanL == 'Y' || nanG == 'Y') break;
            }
            f32 minScale = 1e9f, maxScale = -1e9f;
            for (size_t j = 0; j < skin.Joints.size(); j++) {
                const float4x4& bm = transforms->Skins[0].JointMatrices[j];
                const f32 sx = sqrtf(bm._11 * bm._11 + bm._12 * bm._12 + bm._13 * bm._13);
                const f32 sy = sqrtf(bm._21 * bm._21 + bm._22 * bm._22 + bm._23 * bm._23);
                const f32 sz = sqrtf(bm._31 * bm._31 + bm._32 * bm._32 + bm._33 * bm._33);
                minScale = std::min(minScale, std::min(sx, std::min(sy, sz)));
                maxScale = std::max(maxScale, std::max(sx, std::max(sy, sz)));
            }
            utils::info("pose: %zu joints, bone-matrix scale [%.4f .. %.4f]", skin.Joints.size(), minScale, maxScale);
            const float4x4& mg = transforms->NodeGlobalMatrices[model->Scenes[sceneIndex].LinearNodes[1]->Index];
            utils::info("pose: mesh node global row4 %.2f %.2f %.2f %.2f", mg._41, mg._42, mg._43, mg._44);
        }
    } else if (placementDirty) {
        // no clip playing: re-derive the pose from the last applied TRS so a
        // placement change (teleport / facing) still rebuilds the matrices
        posePipeline(*model, *transforms, nullptr, nullptr, 1.0f, placementRootMatrix());
    }
}

bool gltfBoundingBoxDiligent(float min[3], float max[3]) {
    if (!haveBounds) {
        return false;
    }
    const BoundBox placed = localBounds.Transform(placementRootMatrix());
    min[0] = placed.Min.x;
    min[1] = placed.Min.y;
    min[2] = placed.Min.z;
    max[0] = placed.Max.x;
    max[1] = placed.Max.y;
    max[2] = placed.Max.z;
    return true;
}

bool gltfLocalBoundingBoxDiligent(float min[3], float max[3]) {
    if (!haveBounds) {
        return false;
    }
    min[0] = localBounds.Min.x;
    min[1] = localBounds.Min.y;
    min[2] = localBounds.Min.z;
    max[0] = localBounds.Max.x;
    max[1] = localBounds.Max.y;
    max[2] = localBounds.Max.z;
    return true;
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

// fills the shared PBR frame attribs (camera + sun) for GLTF_PBR_Renderer
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
            static_cast<HLSL::PBRFrameAttribs*>(frame) + 1);
    const f32* sunDir = engine::renderer::diligent::diligentSunDirection();
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

namespace engine::gltf {

void* gltfDiligentPreintegratedGGX(void) {
    if (!pbrRenderer) {
        return nullptr;
    }
    ITextureView* view = pbrRenderer->GetPreintegratedGGX_SRV();
    if (view) {
        view->AddRef();
    }
    return view;
}

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
    animPoseA.reset();
    animPoseB.reset();
    animSource.reset();
    playing = 0;
    active = ClipPlay{};
    blendFrom = ClipPlay{};
    blendDuration = 0.0f;
    placementDirty = true;
    haveBounds = false;
    frameAttribsCB.Release();
    iblIrradiance.Release();
    iblPrefiltered.Release();
    pbrRenderer.reset();
    utils::info("gltf: destroyed");
}

// ── Animation source asset (the old engine's models/animations.dat): a glb
// carrying the same skeleton + all clips but no GPU resources (CPU-only
// Model load). Node indices must line up with the visible model's — both are
// exported from the same blend by scripts/export-models.sh. ──
bool gltfLoadAnimationsDiligent(const char* pakPath) {
    if (!model) {
        utils::warn("gltf: load the character model before the animation source (%s)", pakPath);
        return false;
    }

    std::string error;
    animSource = loadModelBytes(pakPath, error);
    if (!animSource) {
        utils::warn("gltf: animation source failed for %s (%s)", pakPath, error.c_str());
        return false;
    }
    animSourceSceneIndex =
            std::min<Uint32>(animSource->DefaultSceneId, (Uint32)animSource->Scenes.size() - 1);
    animPoseA = std::make_unique<GLTF::ModelTransforms>();
    animPoseB = std::make_unique<GLTF::ModelTransforms>();
    animNodeCountsMatch = animSource->Nodes.size() == model->Nodes.size();
    if (!animNodeCountsMatch) {
        utils::warn("gltf: animation source skeleton mismatch (%zu nodes vs %zu) — animations disabled",
                animSource->Nodes.size(), model->Nodes.size());
        animSource.reset();
        animPoseA.reset();
        animPoseB.reset();
        return false;
    }

    utils::info("gltf: animation source %s — %zu clips", pakPath, animSource->Animations.size());
    return true;
}

static GLTF::Model* animationModel(void) {
    return animSource ? animSource.get() : model.get();
}

u32 gltfAnimationCountDiligent(void) {
    GLTF::Model* src = animationModel();
    return src ? (u32)src->Animations.size() : 0;
}

const char* gltfAnimationNameDiligent(u32 index) {
    GLTF::Model* src = animationModel();
    if (!src || index >= src->Animations.size()) {
        return "";
    }
    return src->Animations[index].Name.c_str();
}

f32 gltfAnimationDurationDiligent(u32 index) {
    GLTF::Model* src = animationModel();
    if (!src || index >= src->Animations.size()) {
        return 0.0f;
    }
    const GLTF::Animation& anim = src->Animations[index];
    return std::max(0.0f, anim.End - anim.Start);
}

static int gltfFindAnimation(const char* name) {
    GLTF::Model* src = animationModel();
    if (!src) {
        return -1;
    }
    for (size_t i = 0; i < src->Animations.size(); i++) {
        if (src->Animations[i].Name == name) {
            return (int)i;
        }
    }
    return -1;
}

bool gltfPlayAnimationDiligent(const char* name, f32 speed, bool loop) {
    const int index = gltfFindAnimation(name);
    if (index < 0) {
        utils::warn("gltf: animation '%s' not found", name);
        return false;
    }
    active = ClipPlay{index, 0.0f, speed, loop};
    blendFrom = ClipPlay{};
    blendDuration = 0.0f;
    blendElapsed = 0.0f;
    playing = 1;
    return true;
}

bool gltfPlayAnimationBlendedDiligent(const char* name, f32 speed, bool loop, f32 blendSeconds) {
    const int index = gltfFindAnimation(name);
    if (index < 0) {
        utils::warn("gltf: animation '%s' not found", name);
        return false;
    }
    if (!playing || blendSeconds <= 0.0f) {
        return gltfPlayAnimationDiligent(name, speed, loop);
    }
    // the currently playing clip becomes the blend-out source; both advance
    // during the crossfade so loop blends (run → idle) stay fluid
    blendFrom = active;
    blendElapsed = 0.0f;
    blendDuration = blendSeconds;
    active = ClipPlay{index, 0.0f, speed, loop};
    playing = 1;
    return true;
}

void gltfStopAnimationDiligent(void) {
    playing = 0;
    active = ClipPlay{};
    blendFrom = ClipPlay{};
    blendDuration = 0.0f;
}
}  // namespace engine::gltf
