#include "gui/rmlui/RmluiDiligent.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "image/Image.h"
#include "logger/Logger.h"
#include "renderer/diligent/DiligentRenderer.h"
#include "renderer/RenderBackend.h"

#include "crmlui.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "DiligentFXShaderSourceStreamFactory.hpp"
#include "Graphics/GraphicsEngine/interface/Buffer.h"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/PipelineResourceSignature.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Sampler.h"
#include "Graphics/GraphicsEngine/interface/SwapChain.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"
#include "Graphics/GraphicsTools/interface/GraphicsUtilities.h"
// vulkan entry points via volk (Diligent initializes volk in its factory) —
// the pass opens its own dynamic-rendering scope like the imgui pass does
#define VK_NO_PROTOTYPES
#include <volk.h>

#include "Graphics/GraphicsEngineVulkan/interface/DeviceContextVk.h"
#include "Graphics/GraphicsEngineVulkan/interface/TextureViewVk.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::renderer::diligent {
using namespace Diligent;

using engine::renderer::diligent::context;
using engine::renderer::diligent::device;
using engine::renderer::diligent::swapChain;

namespace {

static_assert(sizeof(RmlVertex) == 20, "RmlVertex layout: float2 + 4B colour + float2, 4-byte aligned");

// ── Pools / frame state ────────────────────────────────────────────────────

// A texture pool entry (handle = 1-based index). Font atlases and loaded
// images land here; RmlUi hands us back the handle per geometry batch.
struct RmlTexture {
    ITexture*               texture = nullptr;
    ITextureView*           view    = nullptr;  // borrowed default view (no Release)
    IShaderResourceBinding* srb     = nullptr;  // this texture's own SRB (g_UiTex set once at creation)
    int                     width = 0, height = 0;
    std::string             path;               // "" for generated (font) textures
    u32                     framesLeft = 0;     // >0: release queued (deferred GPU destroy)
};

// A compiled geometry: the wrapper's CPU vertex/index storage, re-rendered
// every frame it is visible (RmlUi owns the lifetime — compileGeometry /
// releaseGeometry bracket it).
struct RmlGeometry {
    std::vector<u8>  verts;    // packed RmlVertex
    std::vector<u32> indices;
    bool             inUse = false;
};

// One queued draw (renderGeometry callback, fired during rmlRenderVulkan).
// The scissor state and the ACTIVE RmlUi transform matrix are snapshotted per
// command — RmlUi interleaves setScissorRegion/setTransform between geometry
// (e.g. the milligram active button's `transform: scale(1.2)` fires
// SetTransform for exactly the frames of that one element), and the pass
// bakes both into the vertices during staging.
struct FrameCmd {
    u32   geometry;   // geometry pool index
    u32   texture;    // texture pool index (0 = fallback white)
    float tx, ty;
    bool  scissor;
    i32   sx, sy, sw, sh;
    float transform[16];  // RmlUi column-major (p' = M·p), identity default
};

// A frame command's staged upload range
struct CmdRange {
    u32 vertOffset = 0, vertCount = 0;
    u32 idxOffset = 0, idxCount = 0;
};

// Diligent's QueryInterface fills an IObject**; attach it to a typed smart
// pointer without an extra AddRef (same trick as the imgui backend)
template <typename VkIface, typename DiligentIface>
static RefCntAutoPtr<VkIface> queryVkInterface(DiligentIface* obj, const INTERFACE_ID& iid) {
    IObject* raw = nullptr;
    obj->QueryInterface(iid, &raw);
    RefCntAutoPtr<VkIface> result;
    if (raw) {
        result.Attach(static_cast<VkIface*>(raw));
    }
    return result;
}

// Frame cbuffer (128 B, matches the HLSL cbRmluiFrame): both matrices are
// stored TRANSPOSED — the runtime-compiled HLSL (glslang) path consumes
// cbuffer matrices transposed relative to Diligent's row-major math
// (docs/lessons.md, the 2026-09-05 transpose entry).
struct RmluiFrameAttribs {
    float4x4 orthoProj;
    float4x4 transform;
};
static_assert(sizeof(RmluiFrameAttribs) == 128, "rmlui frame attribs layout");
u8 frameAttribsStaging[128];

constexpr u32 kMaxGeometries       = 1000;  // the old pass' pool size
constexpr u32 kDeferredFrames      = 3;
constexpr u32 kBufferStartBytes    = 256 * 1024;
constexpr u32 kBufferMaxBytes      = 64 * 1024 * 1024;

// GPU pass state
IShader*                    vs = nullptr;
IShader*                    ps = nullptr;
IPipelineState*             pipeline = nullptr;
IPipelineResourceSignature* prs = nullptr;
IShaderResourceBinding*     fallbackSrb = nullptr;  // the fallback texture's SRB
IBuffer*                    frameAttribsCB = nullptr;  // dynamic: re-uploaded per frame
IBuffer*                    vbo = nullptr;             // dynamic: re-uploaded per frame
IBuffer*                    ibo = nullptr;             // dynamic: re-uploaded per frame
u32                         vboBytes = 0, iboBytes = 0;
ISampler*                   uiSampler = nullptr;
ITexture*                   fallbackTex = nullptr;  // 1x1 white (texture 0)
ITextureView*               fallbackView = nullptr;
RefCntAutoPtr<IDeviceContextVk> contextVk;
bool                        passReady = false;
bool                        initFailed = false;

// Frame state (written by the callbacks, consumed by rmluiDraw)
// The ACTIVE RmlUi transform: Rml::Matrix4f is ColumnMajorMatrix4f by default
// (RmlUi/Config/Config.h — RMLUI_MATRIX4_TYPE), applied p' = M·p, translation
// in the last column (data[12..14]). Raw storage; snapshotted per command.
float frameTransform[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
};
static void setFrameTransformIdentity(void) {
    for (u32 i = 0; i < 16; i++) {
        frameTransform[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
}
i32      viewportW = 0, viewportH = 0;
bool     scissorEnabled = false;
i32      scissorX = 0, scissorY = 0, scissorW = 0, scissorH = 0;
std::vector<FrameCmd> frameCommands;

// Pools
std::vector<RmlTexture>            textures;
std::unordered_map<std::string, u32> texturePaths;
std::vector<RmlGeometry>           geometryPool(kMaxGeometries);
u32                                geometryCursor = 0;
std::vector<u32>                   geometriesToRemove;

// Per-frame upload staging
std::vector<RmlVertex> stageVerts;
std::vector<u32>       stageIndices;

// ── Resource helpers ─────────────────────────────────────────────────────

void transitionToShaderResource(ITexture* tex) {
    StateTransitionDesc barrier{tex, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE,
            STATE_TRANSITION_FLAG_UPDATE_STATE};
    context->TransitionResourceStates(1, &barrier);
}

// Upload RGBA8 pixels (one mip level). Same shape as the terrain pass'
// createRgba8 (immutable texture, initial data — the context's pContext in
// the TextureData makes the driver-side copy safe, see the terrain pass).
ITexture* createRgba8(const u8* pixels, u32 w, u32 h, const char* name) {
    TextureDesc desc;
    desc.Name      = name;
    desc.Type      = RESOURCE_DIM_TEX_2D;
    desc.Usage     = USAGE_IMMUTABLE;
    desc.BindFlags = BIND_SHADER_RESOURCE;
    desc.Format    = TEX_FORMAT_RGBA8_UNORM;
    desc.Width     = w;
    desc.Height    = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;

    TextureSubResData subres((const void*)pixels, (Uint64)w * 4u);
    TextureData data;
    data.pSubResources   = &subres;
    data.NumSubresources = 1;
    data.pContext        = context;

    RefCntAutoPtr<ITexture> tex;
    device->CreateTexture(desc, &data, &tex);
    if (!tex) return nullptr;
    transitionToShaderResource(tex);
    tex->AddRef();  // keep one ref past this scope (the pool holds the raw pointer)
    return tex;
}

u32 textureAcquire(ITexture* tex, int w, int h, const std::string& path) {
    textures.emplace_back();
    RmlTexture& t = textures.back();
    t.texture   = tex;
    t.view      = tex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    t.width     = w;
    t.height    = h;
    t.path      = path;
    // One SRB per texture: the g_UiTex (mutable) variable is set exactly once,
    // here, before the SRB is ever committed. This version of Diligent has no
    // update-after-bind flag — updating a committed mutable set in place
    // invalidates the whole command buffer — so each texture's set is created
    // once and only re-bound per batch, never re-updated.
    if (prs) {
        RefCntAutoPtr<IShaderResourceBinding> b;
        prs->CreateShaderResourceBinding(&b, true);
        if (b) {
            if (IShaderResourceVariable* v = b->GetVariableByName(SHADER_TYPE_PIXEL, "g_UiTex")) {
                v->Set(t.view);
            }
            b->AddRef();  // keep one ref past this scope (the pool holds the raw pointer)
            t.srb = b;
        }
    }
    return (u32)textures.size();  // 1-based handle
}

// ── RmlParams.vulkan callbacks (CPU-side queueing — no context access) ────

void releaseGeometryNow(u32 handle) {
    u32 idx = handle - 1;
    if (idx < geometryPool.size() && geometryPool[idx].inUse) {
        geometryPool[idx] = RmlGeometry{};
    }
}

}  // namespace

// ── Public callback targets ───────────────────────────────────────────────

void rmlPassBeginFrame(void) {
    frameCommands.clear();
    for (u32 g : geometriesToRemove) {
        releaseGeometryNow(g);
    }
    geometriesToRemove.clear();
    viewportW = 0;
    viewportH = 0;
    setFrameTransformIdentity();
    scissorEnabled = false;
    scissorX = scissorY = scissorW = scissorH = 0;
}

void rmlPassEndFrame(void) {
    // The queued commands are consumed in rmluiDraw; nothing else to do.
}

uintptr_t rmlPassCompileGeometry(RmlVertex* vertices, int numVertices, const int* indices, int numIndices) {
    if (!vertices || numVertices <= 0 || !indices || numIndices <= 0) {
        return 0;
    }
    u32 idx = geometryCursor;
    for (u32 i = 0; i < kMaxGeometries; i++) {
        if (!geometryPool[idx].inUse) {
            geometryCursor = (idx + 1) % kMaxGeometries;
            break;
        }
        idx = (idx + 1) % kMaxGeometries;
    }
    if (geometryPool[idx].inUse) {
        utils::warn("rmlui: geometry pool exhausted (%u entries) — dropping %d-vertex geometry",
                kMaxGeometries, numVertices);
        return 0;
    }
    RmlGeometry* g = &geometryPool[idx];
    g->inUse = true;
    g->verts.resize((size_t)numVertices * sizeof(RmlVertex));
    memcpy(g->verts.data(), vertices, g->verts.size());
    g->indices.assign(indices, indices + numIndices);
    return idx + 1;
}

void rmlPassReleaseGeometry(uintptr_t geometryHandle) {
    if (!geometryHandle) {
        return;
    }
    geometriesToRemove.push_back((u32)geometryHandle);  // handle; releaseGeometryNow converts to slot
}

void rmlPassRenderGeometry(uintptr_t geometryHandle, float translationX, float translationY, uintptr_t texture) {
    if (!geometryHandle) {
        return;
    }
    u32 gidx = (u32)geometryHandle - 1;
    if (gidx >= geometryPool.size() || !geometryPool[gidx].inUse) {
        // A released slot must never still be referenced by RmlUi — if this fires, the pool's
        // release bookkeeping is off (silently dropping visible geometry looks like "random
        // elements turn black/invisible", see docs/lessons.md 2026-09-05).
        utils::warn("rmlui: renderGeometry %llu on free slot %u — release bookkeeping bug", (unsigned long long)geometryHandle, gidx);
        return;
    }
    FrameCmd cmd;
    cmd.geometry  = gidx;
    cmd.texture   = (u32)texture;
    cmd.tx        = translationX;
    cmd.ty        = translationY;
    cmd.scissor   = scissorEnabled;
    cmd.sx        = scissorX;
    cmd.sy        = scissorY;
    cmd.sw        = scissorW;
    cmd.sh        = scissorH;
    memcpy(cmd.transform, frameTransform, sizeof(cmd.transform));
    frameCommands.push_back(cmd);
}

uintptr_t rmlPassLoadTexture(int* outX, int* outY, const char* path) {
    if (outX) *outX = 0;
    if (outY) *outY = 0;

    auto it = texturePaths.find(path);
    if (it != texturePaths.end()) {
        const RmlTexture& t = textures[it->second - 1];
        if (outX) *outX = t.width;
        if (outY) *outY = t.height;
        return it->second;
    }

    utils::Image image = utils::imageLoad(path);  // pak PNG/JPG → stb → RGBA8
    if (!image.data || image.width <= 0 || image.height <= 0) {
        utils::warn("rmlui: texture load failed: %s", path);
        if (image.data) {
            utils::imageDestory(&image);
        }
        return 0;
    }
    ITexture* tex = createRgba8((const u8*)image.data, image.width, image.height, path);
    utils::imageDestory(&image);
    if (!tex) {
        return 0;
    }
    const u32 handle = textureAcquire(tex, image.width, image.height, path);
    texturePaths[path] = handle;
    if (outX) *outX = image.width;
    if (outY) *outY = image.height;
    return handle;
}

uintptr_t rmlPassGenerateTexture(const unsigned char* data, size_t size, int x, int y) {
    if (!data || x <= 0 || y <= 0 || size != (size_t)x * y * 4u) {
        utils::warn("rmlui: generateTexture got %zu bytes for %dx%d", size, x, y);
        return 0;
    }
    // Font-atlas pages (the RmlUi FreeType font engine's output).
    static int generatedCounter = 0;
    std::string name = "rml generated " + std::to_string(generatedCounter++);
    ITexture* tex = createRgba8(data, (u32)x, (u32)y, name.c_str());
    if (!tex) {
        return 0;
    }
    return textureAcquire(tex, x, y, name);
}

void rmlPassReleaseTexture(uintptr_t textureHandle) {
    if (!textureHandle) {
        return;
    }
    u32 idx = (u32)textureHandle - 1;
    if (idx >= textures.size() || !textures[idx].texture || textures[idx].framesLeft) {
        return;
    }
    textures[idx].framesLeft = kDeferredFrames;
    for (auto it = texturePaths.begin(); it != texturePaths.end();) {
        if (it->second == idx + 1) {
            it = texturePaths.erase(it);
        } else {
            ++it;
        }
    }
}

void rmlPassEnableScissorRegion(char enable) {
    scissorEnabled = (bool)enable;
}

void rmlPassSetScissorRegion(int x, int y, int width, int height) {
    scissorX = x;
    scissorY = y;
    scissorW = width;
    scissorH = height;
}

void rmlPassSetTransform(void* transform) {
    // Rml::Matrix4f defaults to ColumnMajorMatrix4f (RMLUI_MATRIX4_TYPE),
    // applied p' = M·p with the translation in the last column (data[12..14]).
    // Raw storage is snapshotted per renderGeometry call and baked into the
    // vertices during staging (2D affine + CPU perspective divide); the frame
    // cbuffer's g_Transform stays identity.
    if (transform) {
        memcpy(frameTransform, transform, sizeof(frameTransform));
    } else {
        setFrameTransformIdentity();
    }
}

void rmlPassSetViewport(int width, int height) {
    viewportW = width;
    viewportH = height;
}

// ── Pass init / buffers ───────────────────────────────────────────────────

namespace {

IShader* createHlslShader(const char* pakPath, const char* name, SHADER_TYPE type) {
    utils::String blob = utils::dataManagerRead(pakPath);
    if (!blob.data || blob.size == 0) {
        utils::warn("rmlui: shader source missing from pak: %s", pakPath);
        utils::stringDestroy(&blob);
        return nullptr;
    }
    ShaderCreateInfo ci;
    ci.Desc.Name         = name;
    ci.Desc.ShaderType   = type;
    ci.EntryPoint         = "main";
    ci.Source             = blob.data;
    ci.SourceLength       = blob.size;
    ci.SourceLanguage     = SHADER_SOURCE_LANGUAGE_HLSL;
    ci.pShaderSourceStreamFactory = &DiligentFXShaderSourceStreamFactory::GetInstance();

    RefCntAutoPtr<IShader> shader;
    RefCntAutoPtr<IDataBlob> output;
    device->CreateShader(ci, &shader, &output);
    utils::stringDestroy(&blob);
    if (!shader) {
        const char* msg = output ? (const char*)output->GetConstDataPtr() : "(no compiler output)";
        utils::warn("rmlui: shader compile failed %s: %s", name, msg);
        return nullptr;
    }
    shader->AddRef();  // keep one ref past this scope
    return shader;
}

void initPassImpl(void) {
    if (passReady || initFailed || !device || !context) {
        return;
    }

    vs = createHlslShader("materials/rmlui_ui_vs.hlsl", "rmluiUIVS", SHADER_TYPE_VERTEX);
    ps = createHlslShader("materials/rmlui_ui_ps.hlsl", "rmluiUIPS", SHADER_TYPE_PIXEL);
    if (!vs || !ps) {
        initFailed = true;
        return;
    }

    SamplerDesc sd;
    sd.Name       = "rmlui linear clamp";
    sd.MinFilter  = FILTER_TYPE_LINEAR;
    sd.MagFilter  = FILTER_TYPE_LINEAR;
    sd.MipFilter  = FILTER_TYPE_LINEAR;
    sd.AddressU   = TEXTURE_ADDRESS_CLAMP;
    sd.AddressV   = TEXTURE_ADDRESS_CLAMP;
    sd.AddressW   = TEXTURE_ADDRESS_CLAMP;
    device->CreateSampler(sd, &uiSampler);

    const u8 white[4] = {255, 255, 255, 255};
    fallbackTex = createRgba8(white, 1, 1, "rmlui fallback");
    if (!fallbackTex) {
        initFailed = true;
        return;
    }
    fallbackView = fallbackTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);  // borrowed: no Release (GetDefaultView note)

    // Per-frame dynamic cbuffer (the props/terrain frame-attribs pattern:
    // CreateUniformBuffer defaults are USAGE_DYNAMIC + CPU_ACCESS_WRITE —
    // exactly the "re-uploaded every frame" buffers the lessons.md ring
    // clobbering rule says are safe).
    CreateUniformBuffer(device, sizeof(RmluiFrameAttribs), "rmlui frame attribs", &frameAttribsCB);
    if (!frameAttribsCB) {
        initFailed = true;
        return;
    }

    // The texture SRV is the per-batch resource; each texture owns its own
    // SRB (g_UiTex set once at creation — see textureAcquire) and the batch
    // simply re-binds that SRB. The cbuffer + sampler are static, shared
    // across all SRBs through the PRS.
    PipelineResourceDesc resources[] = {
            {SHADER_TYPE_VERTEX | SHADER_TYPE_PIXEL, "cbRmluiFrame", 1,
                    SHADER_RESOURCE_TYPE_CONSTANT_BUFFER, SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_UiSampler", 1, SHADER_RESOURCE_TYPE_SAMPLER,
                    SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
            {SHADER_TYPE_PIXEL, "g_UiTex", 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV,
                    SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
    };
    PipelineResourceSignatureDesc prsDesc;
    prsDesc.Resources      = resources;
    prsDesc.NumResources   = (Uint32)(sizeof(resources) / sizeof(resources[0]));
    prsDesc.BindingIndex   = 0;
    device->CreatePipelineResourceSignature(prsDesc, &prs);
    if (!prs) {
        initFailed = true;
        return;
    }
    if (IShaderResourceVariable* v = prs->GetStaticVariableByName(SHADER_TYPE_VERTEX, "cbRmluiFrame")) {
        v->Set(frameAttribsCB, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    }
    if (IShaderResourceVariable* v = prs->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_UiSampler")) {
        v->Set(uiSampler, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    }
    // Fallback (texture 0): 1x1 white, with its own SRB
    RefCntAutoPtr<IShaderResourceBinding> fb;
    prs->CreateShaderResourceBinding(&fb, true);
    if (fb) {
        if (IShaderResourceVariable* v = fb->GetVariableByName(SHADER_TYPE_PIXEL, "g_UiTex")) {
            v->Set(fallbackView);
        }
        fb->AddRef();  // keep one ref past this scope
        fallbackSrb = fb;
    }

    GraphicsPipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name             = "rmluiUI";
    psoCI.ppResourceSignatures     = &prs;
    psoCI.ResourceSignaturesCount  = 1;

    GraphicsPipelineDesc& gp = psoCI.GraphicsPipeline;
    gp.BlendDesc.RenderTargets[0].BlendEnable    = True;
    gp.BlendDesc.RenderTargets[0].SrcBlend       = BLEND_FACTOR_SRC_ALPHA;
    gp.BlendDesc.RenderTargets[0].DestBlend      = BLEND_FACTOR_INV_SRC_ALPHA;
    gp.BlendDesc.RenderTargets[0].BlendOp        = BLEND_OPERATION_ADD;
    gp.BlendDesc.RenderTargets[0].SrcBlendAlpha  = BLEND_FACTOR_ONE;
    gp.BlendDesc.RenderTargets[0].DestBlendAlpha = BLEND_FACTOR_INV_SRC_ALPHA;
    gp.BlendDesc.RenderTargets[0].BlendOpAlpha   = BLEND_OPERATION_ADD;
    gp.RasterizerDesc.CullMode                = CULL_MODE_NONE;  // UI quads: draw both faces
    gp.DepthStencilDesc.DepthEnable           = False;            // UI floats over the world
    gp.DepthStencilDesc.DepthWriteEnable      = False;
    gp.PrimitiveTopology                      = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    gp.NumRenderTargets                       = 1;
    gp.RTVFormats[0]                          = swapChain->GetDesc().ColorBufferFormat;

    // The 20-byte RmlVertex (float2 position, 4-byte colour, float2 texcoord)
    // maps 1:1 — the colour's bit pattern is read through a float element
    // (asuint in the VS), so no CPU-side vertex conversion is needed.
    static const LayoutElement inputLayout[] = {
            {"ATTRIB", 0, 0, 2, VT_FLOAT32, False, 0u, 20u},   // float2 position
            {"ATTRIB", 1, 0, 1, VT_FLOAT32, False, 8u, 20u},  // float colour bits
            {"ATTRIB", 2, 0, 2, VT_FLOAT32, False, 12u, 20u},  // float2 texcoord
    };
    gp.InputLayout.LayoutElements = inputLayout;
    gp.InputLayout.NumElements   = 3;

    psoCI.pVS = vs;
    psoCI.pPS = ps;
    // No depth attachment: the pass draws inside the backend's own dynamic-
    // rendering scope (color + the swapchain's depth image — see drawImpl),
    // so the pipeline must declare the depth format the scope uses (VUID
    // dynamicRenderingUnusedAttachments), while depth test/write stay off.
    gp.DSVFormat = swapChain->GetDesc().DepthBufferFormat;
    device->CreateGraphicsPipelineState(psoCI, &pipeline);
    if (!pipeline) {
        initFailed = true;
        return;
    }

    passReady = true;
    utils::info("rmlui: diligent pass initialized (shader + PSO + dynamic buffers)");
}

// Grow (2x) + (re)create the per-frame dynamic VBO/IBO. Per-frame scratch:
// re-uploaded every frame (docs/lessons.md, the 2026-09 dynamic-buffer
// entry) — never read back, never longer-lived than one frame.
bool ensureBuffers(u32 vbytes, u32 ibytes) {
    auto grow = [](u32 cur, u32 need) -> u32 {
        u32 n = cur ? cur * 2u : kBufferStartBytes;
        while (n < need && n < kBufferMaxBytes) {
            n *= 2u;
        }
        return n;
    };

    u32 nv = vboBytes, ni = iboBytes;
    if (vbytes > nv) nv = grow(nv, vbytes);
    if (ibytes > ni) ni = grow(ni, ibytes);
    if (vbo && ibo && vbytes <= vboBytes && ibytes <= iboBytes) {
        return true;
    }
    if (vbytes > nv || ibytes > ni) {
        utils::warn("rmlui: frame geometry exceeds buffer capacity (%u verts / %u indices, max %u bytes) — "
                "dropping frame",
                vbytes / 20u, ibytes / 4u, kBufferMaxBytes);
        return false;
    }
    if (vbo) { vbo->Release(); vbo = nullptr; }
    if (ibo) { ibo->Release(); ibo = nullptr; }

    BufferDesc desc;
    desc.Usage          = USAGE_DYNAMIC;
    desc.CPUAccessFlags = CPU_ACCESS_WRITE;
    desc.Name           = "rmlui vertex buffer";
    desc.BindFlags      = BIND_VERTEX_BUFFER;
    desc.Size           = nv;
    device->CreateBuffer(desc, nullptr, &vbo);
    desc.Name      = "rmlui index buffer";
    desc.BindFlags = BIND_INDEX_BUFFER;
    desc.Size      = ni;
    device->CreateBuffer(desc, nullptr, &ibo);
    vboBytes = nv;
    iboBytes = ni;
    return vbo && ibo;
}

// ── Draw ─────────────────────────────────────────────────────────────────

void tickDeferredTextureReleases(void) {
    for (size_t i = 0; i < textures.size(); i++) {
        RmlTexture& t = textures[i];
        if (!t.framesLeft) {
            continue;
        }
        if (t.framesLeft > 1) {
            t.framesLeft--;
            continue;
        }
        // The last committed SRB may still reference the texture in flight —
        // the 3-frame deferral covers that (the terrain pass' deferred destroy
        // pattern). The texture + its SRB are owned; the view is the borrowed
        // default view (no Release — GetDefaultView does not add a ref).
        if (t.srb) t.srb->Release();
        if (t.texture) t.texture->Release();
        textures[i] = RmlTexture{};
    }
}

void drawImpl(void) {
    if (!passReady) {
        initPassImpl();
        if (!passReady) {
            return;
        }
    }
    if (frameCommands.empty()) {
        return;
    }

    tickDeferredTextureReleases();

    const SwapChainDesc& scDesc = swapChain->GetDesc();
    const i32 vw = viewportW > 0 ? viewportW : (i32)scDesc.Width;
    const i32 vh = viewportH > 0 ? viewportH : (i32)scDesc.Height;

    // 1) Stage the frame's geometry: bake the per-geometry translation AND
    //    the per-command RmlUi transform into the vertices, rebase the indices
    //    (they are per-geometry local); one dynamic VBO + IBO carry the frame.
    stageVerts.clear();
    stageIndices.clear();
    std::vector<CmdRange> ranges(frameCommands.size());
    for (size_t i = 0; i < frameCommands.size(); i++) {
        const FrameCmd& cmd = frameCommands[i];
        RmlGeometry* g = cmd.geometry < geometryPool.size() ? &geometryPool[cmd.geometry] : nullptr;
        if (!g || !g->inUse || g->verts.empty() || g->indices.empty()) {
            continue;
        }
        const float* m = cmd.transform;
        const RmlVertex* src = (const RmlVertex*)g->verts.data();
        const u32 nVerts = (u32)(g->verts.size() / sizeof(RmlVertex));
        CmdRange& r = ranges[i];
        r.vertOffset = (u32)stageVerts.size();
        for (u32 v = 0; v < nVerts; v++) {
            RmlVertex out;
            const float px = src[v].position.x + cmd.tx;
            const float py = src[v].position.y + cmd.ty;
            // column-major M·p with p = (px, py, 0, 1)
            float x = m[0] * px + m[4] * py + m[12];
            float y = m[1] * px + m[5] * py + m[13];
            const float w = m[3] * px + m[7] * py + m[15];
            if (w != 1.0f && fabsf(w) > 1e-6f) {
                x /= w;
                y /= w;
            }
            out.position.x   = x;
            out.position.y   = y;
            memcpy(&out.colour, &src[v].colour, sizeof(out.colour));
            out.texCoord.x   = src[v].texCoord.x;
            out.texCoord.y   = src[v].texCoord.y;
            stageVerts.push_back(out);
        }
        r.vertCount = nVerts;
        r.idxOffset = (u32)stageIndices.size();
        for (u32 idx : g->indices) {
            stageIndices.push_back(idx + r.vertOffset);
        }
        r.idxCount = (u32)g->indices.size();
    }

    const u32 vbytesAll = (u32)stageVerts.size() * (u32)sizeof(RmlVertex);
    const u32 ibytesAll = (u32)stageIndices.size() * 4u;
    if (getenv("ENGINE_RML_PROBE")) {
        for (size_t i = 0; i < frameCommands.size(); i++) {
            const FrameCmd& c = frameCommands[i];
            const CmdRange& r = ranges[i];
            float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
            if (r.idxCount > 0 && r.vertOffset + r.vertCount <= stageVerts.size()) {
                for (u32 k = 0; k < r.idxCount; k++) {
                    const RmlVertex& v = stageVerts[stageIndices[r.idxOffset + k]];
                    mnx = std::min(mnx, v.position.x);
                    mny = std::min(mny, v.position.y);
                    mxx = std::max(mxx, v.position.x);
                    mxy = std::max(mxy, v.position.y);
                }
            }
            utils::info("rmlprobe: cmd %zu geom %u tx %d ty %d tex %u idx %u scis %d [%d %d %d %d] bbox [%g %g]-[%g %g]",
                    i, c.geometry, (int)c.tx, (int)c.ty, c.texture, r.idxCount, c.scissor ? 1 : 0,
                    c.sx, c.sy, c.sw, c.sh, mnx, mny, mxx, mxy);
        }
    }
    if (getenv("ENGINE_RML_PROBE")) {
        utils::info("rmlprobe: textures:");
        for (size_t t = 1; t <= textures.size(); t++) {
            const RmlTexture& tt = textures[t - 1];
            utils::info("rmlprobe:   tex %zu '%s' %dx%d srb %d rel %u", t, tt.path.c_str(), tt.width, tt.height, tt.srb ? 1 : 0, tt.framesLeft);
        }
    }
    if (vbytesAll == 0 || ibytesAll == 0 || !ensureBuffers(vbytesAll, ibytesAll)) {
        frameCommands.clear();
        return;
    }

    // Per-frame dynamic uploads MUST go through MapBuffer/UnmapBuffer — this
    // Diligent build explicitly forbids UpdateBuffer on USAGE_DYNAMIC buffers
    // (DEV_CHECK_ERR in DeviceContextVkImpl::UpdateBuffer, compiled out of the
    // release prebuilt libs). A suballocated dynamic buffer's bind offset
    // comes from m_MappedBuffers — set ONLY by MapBuffer — while UpdateBuffer
    // blind-copies to the shared dynamic heap's byte 0: draws then read
    // whatever bytes sit at the last-written offset (other buffers' data,
    // stale frames) — the garbled menu / full-screen wedges this pass shipped
    // with. Same rule as docs/lessons.md "dynamic buffers are per-frame
    // scratch"; the map/draw offset contract is the other half of it.
    void* vboMapPtr = nullptr;
    void* iboMapPtr = nullptr;
    {
        void* dst = nullptr;
        context->MapBuffer(vbo, MAP_WRITE, MAP_FLAG_DISCARD, dst);
        vboMapPtr = dst;
        memcpy(dst, stageVerts.data(), vbytesAll);
        context->UnmapBuffer(vbo, MAP_WRITE);
        context->MapBuffer(ibo, MAP_WRITE, MAP_FLAG_DISCARD, dst);
        iboMapPtr = dst;
        memcpy(dst, stageIndices.data(), ibytesAll);
        context->UnmapBuffer(ibo, MAP_WRITE);
    }
    // 2) Frame cbuffer: y-down ortho over the RmlUi viewport (RmlUi units →
    //    clip). The per-element RmlUi transform is baked into the staged
    //    vertices (column-major M·p + CPU divide, see staging), so the
    //    shader's g_Transform stays identity.
    //
    // The Y mapping is the OLD engine's GLM y-down ortho (glm_ortho(0, W, H, 0)
    // → scale -2/H, translation +1), NOT a naive "Vulkan NDC is y-down" one:
    // this pipeline's effective behaviour flips Y again, so a +2/H y-scale
    // renders the whole document upside-down. Verified by A/B screenshots
    // (round 4, task 4): transpose + y-down ortho renders upright; every other
    // combination is flipped or off-screen.
    {
        RmluiFrameAttribs* frame = (RmluiFrameAttribs*)frameAttribsStaging;
        float4x4 ortho = float4x4::Identity();
        ortho._11 = 2.0f / (float)vw;
        ortho._22 = -2.0f / (float)vh;
        ortho._33 = 0.0f;
        ortho._34 = 0.5f;  // clip.z = 0.5 for everything (depth test is off)
        ortho._41 = -1.0f;
        ortho._42 = 1.0f;
        ortho._43 = 0.0f;
        frame->orthoProj = ortho.Transpose();
        frame->transform = float4x4::Identity();
        void* dst = nullptr;
        context->MapBuffer(frameAttribsCB, MAP_WRITE, MAP_FLAG_DISCARD, dst);
        memcpy(dst, frameAttribsStaging, sizeof(RmluiFrameAttribs));
        context->UnmapBuffer(frameAttribsCB, MAP_WRITE);
    }

    // 3) Draw through the backend's OWN dynamic-rendering scope, public
    //    context API only — no raw vkCmdBeginRendering. The world pass
    //    (SetRenderTargets + deferred ClearRenderTarget) leaves the backend
    //    in a half-committed state it manages around its own bookkeeping;
    //    a raw scope on the same command buffer nests inside whatever the
    //    backend has open (VUID-vkCmdBeginRendering-renderpass, and the
    //    driver drops most of the frame's geometry as a result). Re-binding
    //    the targets here is deliberate: the imgui pass (when active) ends in
    //    Flush + InvalidateState, which unbinds everything — and the default
    //    attachment load op is LOAD, so re-binding never wipes the world or
    //    the imgui content already drawn on top of it.
    {
        ITextureView* rtv = swapChain->GetCurrentBackBufferRTV();
        ITextureView* dsv = swapChain->GetDepthBufferDSV();
        context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->SetRenderTargets(1, &rtv, dsv, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        if (!engine::renderer::diligent::diligentWorldDrew()) {
            // No world geometry this frame: clear like the imgui pass does.
            context->ClearRenderTarget(rtv, engine::renderer::kClearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            context->ClearDepthStencil(dsv, CLEAR_DEPTH_FLAG, 1.0f, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        // A zero-vertex draw commits the render pass (opens the scope, with
        // the pending clear applied) without drawing, so the viewport / scissor
        // below land inside an active rendering instance — dynamic state is
        // per-scope in dynamic rendering. The pipeline must be bound first:
        // PrepareForDraw dereferences it even for an empty draw.
        context->SetPipelineState(pipeline);
        // Vertex/index buffers must be bound for the pipeline even for an
        // empty draw (committing zero buffers would violate the draw VUIDs).
        context->SetVertexBuffers(0, 1, &vbo, nullptr,
                RESOURCE_STATE_TRANSITION_MODE_TRANSITION, SET_VERTEX_BUFFERS_FLAG_RESET);
        context->SetIndexBuffer(ibo, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->Draw(DrawAttribs{0, DRAW_FLAG_NONE, 0});
    }

    // Viewport per rendering instance (dynamic state is per-scope in dynamic
    // rendering) — always set, never conditional.
    Viewport uiVp(0.0f, 0.0f, (float)vw, (float)vh, 0.0f, 1.0f);
    context->SetViewports(1, &uiVp, 0, 0);

    // 4) Batches: consecutive commands sharing texture + scissor merge into
    //    one DrawIndexed (the staged ranges are contiguous, so the merged
    //    index range is one span).

    size_t i = 0;
    while (i < frameCommands.size()) {
        const FrameCmd& head = frameCommands[i];
        size_t j = i;
        while (j + 1 < frameCommands.size() &&
                frameCommands[j + 1].texture == head.texture &&
                frameCommands[j + 1].scissor == head.scissor &&
                frameCommands[j + 1].sx == head.sx &&
                frameCommands[j + 1].sy == head.sy &&
                frameCommands[j + 1].sw == head.sw &&
                frameCommands[j + 1].sh == head.sh) {
            j++;
        }

        u32 batchIdxOffset = 0, batchIdxCount = 0;
        for (size_t k = i; k <= j; k++) {
            if (ranges[k].idxCount == 0) {
                continue;
            }
            if (batchIdxCount == 0) {
                batchIdxOffset = ranges[k].idxOffset;
            }
            batchIdxCount += ranges[k].idxCount;
        }

        if (batchIdxCount > 0) {
            // Re-bind this texture's SRB (created once, g_UiTex fixed — the
            // set is never updated after its first commit, so the command
            // buffer stays valid between batches).
            IShaderResourceBinding* batchSrb = fallbackSrb;
            if (head.texture && head.texture <= textures.size()) {
                const RmlTexture& t = textures[head.texture - 1];
                if (t.srb) {
                    batchSrb = t.srb;
                }
            }
            if (getenv("ENGINE_RML_PROBE")) {
                const RmlVertex& v0 = stageVerts[stageIndices[batchIdxOffset]];
                utils::info("rmlprobe: batch %zu-%zu tex %u (%s) idx [%u..%u] firstv [%g %g]",
                        i, j, head.texture,
                        head.texture && head.texture <= textures.size() ? textures[head.texture - 1].path.c_str() : "?",
                        batchIdxOffset, batchIdxOffset + batchIdxCount, v0.position.x, v0.position.y);
            }
            context->CommitShaderResources(batchSrb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

            // Scissor is clamped to the rendering area (a rect outside it is
            // undefined behaviour in dynamic rendering).
            i32 x = head.sx, y = head.sy, x2 = head.sx + head.sw, y2 = head.sy + head.sh;
            if (head.scissor && head.sw > 0 && head.sh > 0) {
                x = std::max(x, (i32)0);
                y = std::max(y, (i32)0);
                x2 = std::min(x2, (i32)scDesc.Width);
                y2 = std::min(y2, (i32)scDesc.Height);
            }
            Rect rect(0, 0, (i32)scDesc.Width, (i32)scDesc.Height);
            if (x2 > x && y2 > y) {
                rect = Rect(x, y, x2, y2);
            }
            context->SetScissorRects(1, &rect, 0, 0);

            context->SetVertexBuffers(0, 1, &vbo, nullptr,
                    RESOURCE_STATE_TRANSITION_MODE_TRANSITION, SET_VERTEX_BUFFERS_FLAG_RESET);
            context->SetIndexBuffer(ibo, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            context->DrawIndexed(DrawIndexedAttribs{
                    batchIdxCount, VT_UINT32, DRAW_FLAG_NONE, 1, batchIdxOffset, 0, 0});
        }

        i = j + 1;
    }

    // Close the scope (the deferred clear, if any, commits with it) and
    // submit the frame.
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->Flush();

    frameCommands.clear();
}

// ── Destroy ───────────────────────────────────────────────────────────────

void destroyImpl(void) {
    if (!device) {
        textures.clear();
        texturePaths.clear();
        geometryPool.clear();
        geometriesToRemove.clear();
        frameCommands.clear();
        passReady = false;
        return;
    }

    // Only the textures + their SRBs are owned; the pool views and
    // fallbackView are the borrowed default views (GetDefaultView adds no ref
    // — no Release).
    for (size_t i = 0; i < textures.size(); i++) {
        if (textures[i].srb) textures[i].srb->Release();
        if (textures[i].texture) textures[i].texture->Release();
    }
    textures.clear();
    texturePaths.clear();
    geometryPool.clear();
    geometriesToRemove.clear();
    frameCommands.clear();

    if (fallbackSrb) { fallbackSrb->Release(); fallbackSrb = nullptr; }
    contextVk.Release();
    if (pipeline) { pipeline->Release(); pipeline = nullptr; }
    if (prs) { prs->Release(); prs = nullptr; }
    if (vs) { vs->Release(); vs = nullptr; }
    if (ps) { ps->Release(); ps = nullptr; }
    if (vbo) { vbo->Release(); vbo = nullptr; }
    if (ibo) { ibo->Release(); ibo = nullptr; }
    if (frameAttribsCB) { frameAttribsCB->Release(); frameAttribsCB = nullptr; }
    if (fallbackTex) { fallbackTex->Release(); fallbackTex = nullptr; }
    if (uiSampler) { uiSampler->Release(); uiSampler = nullptr; }
    fallbackView = nullptr;

    vboBytes = iboBytes = 0;
    passReady = false;
    initFailed = false;
}

}  // namespace

// ── Public entry points (see RmluiDiligent.h) ─────────────────────────────

void rmluiPassInit(void) {
    initPassImpl();
}

void rmluiDraw(Diligent::IDeviceContext* ctx) {
    (void)ctx;  // the pass uses the shared context (same as the hook's argument)
    drawImpl();
}

void rmluiOnBackendDestroy(void) {
    static char destroyed = 0;  // the backend destroy must not run this twice
    if (destroyed) {
        return;
    }
    destroyed = 1;
    destroyImpl();
}

}  // namespace engine::renderer::diligent
