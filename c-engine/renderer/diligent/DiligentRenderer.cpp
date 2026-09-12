#include "renderer/diligent/DiligentRenderer.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "DebugOutput.h"
#include "Engine.h"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/Query.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/SwapChain.h"
#include "Graphics/GraphicsTools/interface/ScopedDebugGroup.hpp"
#include "Platforms/interface/NativeWindow.h"
#include "Utils.h"
#include "gltf/GltfInternal.h"
#include "gui/GuiManager.h"
#include "gui/rmlui/GuiManagerRmlUi.h"
#include "logger/Logger.h"
#include "renderer/RenderBackend.h"
#include "renderer/Window.h"
#include "renderer/diligent/IblDiligent.h"
#include "renderer/diligent/ShadowDiligent.h"
#include "renderer/diligent/SsaoDiligent.h"
#include "renderer/diligent/SsrDiligent.h"
#include "renderer/diligent/BloomDiligent.h"
#include "renderer/diligent/TaaDiligent.h"

#include "Graphics/GraphicsEngine/interface/Texture.h"

#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

// vulkan types for the Diligent Vk interfaces below (resolved through volk,
// which Diligent initializes inside its engine factory)
#define VK_NO_PROTOTYPES
#include <volk.h>

#include "Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"

using namespace Diligent;

namespace engine::renderer::diligent {

    IRenderDevice* device   = nullptr;
    IDeviceContext* context = nullptr;
    ISwapChain* swapChain   = nullptr;

    static bool worldDrewThisFrame = false;

    bool diligentWorldDrew(void) {
        return worldDrewThisFrame;
    }

    void setWorldDrew(bool drew) {
        worldDrewThisFrame = drew;
    }

    // Without a callback, Diligent's default handler prints its own
    // "Diligent Engine: ..." lines straight to stdout; this routes them
    // through the game logger instead.
    // One suppression: RADV emits WARNING-Shader-OutputNotConsumed when the
    // DiligentFX post-FX pipelines are built (Precompute BRDF LUT, TAA
    // reprojection depth / closest motion / temporal accumulation). Their
    // pixel shaders never read FullScreenTriangleVS' `uInstID : INSTANCE_ID`
    // output, so the driver discards the write — the message states this is
    // not invalid, but it repeats on every world entry, so it is filtered.
    static void diligentMessageCallback(DEBUG_MESSAGE_SEVERITY severity,
                                        const char* message,
                                        const char*,
                                        const char*,
                                        int) {
        if (severity == DEBUG_MESSAGE_SEVERITY_WARNING && message &&
            strstr(message, "WARNING-Shader-OutputNotConsumed"))
            return;
        switch (severity) {
            case DEBUG_MESSAGE_SEVERITY_INFO:
                utils::info("diligent: %s", message);
                break;
            case DEBUG_MESSAGE_SEVERITY_WARNING:
                utils::warn("diligent: %s", message);
                break;
            default:
                utils::error("diligent: %s", message);
                break;
        }
    }

    namespace {

        // Vulkan format enum values (vulkan/core.h) — kept as literal constants so
        // this TU doesn't pull the Vulkan headers (volk in the passes already does).
        // Only the payloads the c-utils decode can emit are mapped.
        TEXTURE_FORMAT texFormatFromVk(i64 vkFormat, bool srgbForUncompressed) {
            switch (vkFormat) {
                case 37:  // VK_FORMAT_R8G8B8A8_UNORM — libktx reports RGBA32 transcodes
                          // UNORM even for sRGB DFDs, so the caller's flag decides
                    return srgbForUncompressed ? TEX_FORMAT_RGBA8_UNORM_SRGB
                                               : TEX_FORMAT_RGBA8_UNORM;
                case 43:
                    return TEX_FORMAT_RGBA8_UNORM_SRGB;  // VK_FORMAT_R8G8B8A8_SRGB
                case 126:
                    return TEX_FORMAT_BC4_UNORM;  // VK_FORMAT_BC4_UNORM_BLOCK
                case 128:
                    return TEX_FORMAT_BC4_SNORM;  // VK_FORMAT_BC4_SNORM_BLOCK
                case 129:
                    return TEX_FORMAT_BC5_UNORM;  // VK_FORMAT_BC5_UNORM_BLOCK
                case 131:
                    return TEX_FORMAT_BC5_SNORM;  // VK_FORMAT_BC5_SNORM_BLOCK
                case 145:
                    return TEX_FORMAT_BC7_UNORM;  // VK_FORMAT_BC7_UNORM_BLOCK
                case 146:
                    return TEX_FORMAT_BC7_UNORM_SRGB;  // VK_FORMAT_BC7_SRGB_BLOCK (DFD sRGB)
                default:
                    return TEX_FORMAT_UNKNOWN;
            }
        }

        // Per-mip row pitch in bytes. Block-compressed formats store 4x4 texel
        // blocks: BC4 packs a block into 8 bytes, BC5/BC7 into 16; rows are whole
        // blocks. Uncompressed RGBA8 is one 4-byte texel per pixel.
        u64 mipPitch(u32 width, TEXTURE_FORMAT fmt) {
            const u32 blocks = (width + 3u) / 4u;
            switch (fmt) {
                case TEX_FORMAT_BC4_UNORM:
                case TEX_FORMAT_BC4_SNORM:
                    return u64(blocks) * 8u;
                case TEX_FORMAT_BC5_UNORM:
                case TEX_FORMAT_BC5_SNORM:
                case TEX_FORMAT_BC7_UNORM:
                case TEX_FORMAT_BC7_UNORM_SRGB:
                    return u64(blocks) * 16u;
                default:
                    return u64(width) * 4u;  // RGBA8
            }
        }

    }

    ITexture* diligentCreateImageTexture(const utils::Image& image,
                                         const char* name,
                                         bool srgbForUncompressed) {
        if (!image.isKtx || !image.data || image.width <= 0 || image.height <= 0 ||
            image.mips <= 0) {
            return nullptr;
        }
        const TEXTURE_FORMAT fmt = texFormatFromVk(image.vkFormat, srgbForUncompressed);
        if (fmt == TEX_FORMAT_UNKNOWN) {
            utils::warn("diligent: texture '%s': unmapped vkFormat %d", name, image.vkFormat);
            return nullptr;
        }

        TextureDesc desc;
        desc.Name      = name;
        desc.Type      = RESOURCE_DIM_TEX_2D;
        desc.Usage     = USAGE_IMMUTABLE;
        desc.BindFlags = BIND_SHADER_RESOURCE;
        desc.Format    = fmt;
        desc.Width     = (Uint32)image.width;
        desc.Height    = (Uint32)image.height;
        desc.MipLevels = (Uint32)image.mips;
        desc.ArraySize = 1;

        // utils::Image.mipSizes holds per-level BYTE OFFSETS into image.data
        // (ktxTexture_GetImageOffset results, despite the field name); levels are
        // tightly packed after the transcode, each row whole blocks/texels.
        std::vector<TextureSubResData> subres((size_t)image.mips);
        for (i32 i = 0; i < image.mips; i++) {
            const u64 offset = i < (i32)image.mipSizes.size() ? image.mipSizes[i] : 0u;
            const u32 w      = (u32)std::max(1, image.width >> i);
            subres[i] =
                TextureSubResData((const void*)((const u8*)image.data + offset), mipPitch(w, fmt));
        }
        TextureData data;
        data.pSubResources   = subres.data();
        data.NumSubresources = (Uint32)image.mips;
        data.pContext        = context;  // driver-side copy (the passes' pattern)

        RefCntAutoPtr<ITexture> tex;
        device->CreateTexture(desc, &data, &tex);
        if (!tex) {
            utils::warn("diligent: texture creation failed: %s", name);
            return nullptr;
        }
        StateTransitionDesc barrier{tex,
                                    RESOURCE_STATE_UNKNOWN,
                                    RESOURCE_STATE_SHADER_RESOURCE,
                                    STATE_TRANSITION_FLAG_UPDATE_STATE};
        context->TransitionResourceStates(1, &barrier);
        tex->AddRef();  // keep one ref past this scope (the caller holds the raw pointer)

        // Upload size = sum of the per-mip pitches x rows (RGBA8: w*h*4). Log it so
        // the compressed-vs-RGBA8 win is visible in every run's log.
        u64 bytes = 0;
        for (i32 i = 0; i < image.mips; i++) {
            const u32 w    = (u32)std::max(1, image.width >> i);
            const u32 h    = (u32)std::max(1, image.height >> i);
            const u64 rows = (fmt >= TEX_FORMAT_BC4_UNORM && fmt <= TEX_FORMAT_BC7_UNORM_SRGB)
                                 ? (h + 3u) / 4u
                                 : h;
            bytes += mipPitch(w, fmt) * rows;
        }
        utils::info("diligent: texture '%s' %dx%d vkFormat %d mips %d (%.1f KB)",
                    name,
                    image.width,
                    image.height,
                    image.vkFormat,
                    image.mips,
                    (double)bytes / 1024.0);
        return tex;
    }

    static RefCntAutoPtr<IRenderDevice> deviceRef;
    static RefCntAutoPtr<IDeviceContext> contextRef;
    static RefCntAutoPtr<ISwapChain> swapChainRef;
    static RefCntAutoPtr<IEngineFactoryVk> factoryRef;

    // Whole-frame GPU time (the FPS HUD's gpu row): a RING of
    // QUERY_TYPE_DURATION queries. The Vulkan backend implements one as two
    // vkCmdWriteTimestamps at VK_PIPELINE_STAGE_BOTTOM_OF_PIPE — begin and
    // end — so BeginQuery at the top of draw() + EndQuery before Present
    // measures the entire frame's GPU work (clear + all passes + screenshot
    // copy). A ring is REQUIRED: QueryVkImpl::GetData only returns data once
    // the GPU retired the frame that recorded EndQuery (m_QueryEndFenceValue)
    // and the completed-fence value only advances when Diligent polls it
    // during Present — so the just-ended frame's result is never ready yet,
    // and reading a single reused query every frame either froze or never
    // returned data. Each frame we read the query that ended LAP_AHEAD frames
    // ago, which is always retired by then (same pattern as Diligent's own
    // DurationQueryHelper pending ring).
    static constexpr int GPU_TIME_RING = 4;
    static RefCntAutoPtr<IQuery> gpuTimeQuery[GPU_TIME_RING];
    static int gpuTimeRingIndex       = 0;   // slot used this frame
    static int gpuTimeFramesSeen      = 0;   // warm-up: slots before this were never ended
    static double gpuTimeNs   = 0.0;         // last COMPLETED frame; 0 until first result

    // The driver must expose GPU timestamps; otherwise Diligent never creates
    // the timestamp query pool and BeginQuery would submit with a null pool.
    // Mirrors QueryManagerVk's check (a graphics queue with timestampValidBits
    // > 0). Safe to call once volk is loaded (it is, after the device exists).
    static bool vulkanTimestampsSupported(void) {
        // The driver rejects a null instance here, so pass the instance volk
        // loaded when the Diligent factory created it
        VkInstance instance = volkGetLoadedInstance();
        uint32_t physCount  = 0;
        if (!instance || vkEnumeratePhysicalDevices(instance, &physCount, nullptr) != VK_SUCCESS ||
            physCount == 0)
            return true;  // cannot check; assume supported
        std::vector<VkPhysicalDevice> phys(physCount);
        vkEnumeratePhysicalDevices(instance, &physCount, phys.data());
        std::vector<VkQueueFamilyProperties> fams;
        for (uint32_t i = 0; i < physCount; i++) {
            uint32_t famCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &famCount, nullptr);
            fams.resize(famCount);
            vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &famCount, fams.data());
            for (const auto& f : fams)
                if ((f.queueFlags & VK_QUEUE_GRAPHICS_BIT) && f.timestampValidBits > 0) return true;
        }
        return false;
    }

    class DiligentBackend final : public RenderBackend {
       public:
        bool init() override {
            if (!getenv("VK_ICD_FILENAMES"))
                setenv("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/radeon_icd.json", 0);

            factoryRef = GetEngineFactoryVk();
            if (!factoryRef) {
                utils::error("renderer: GetEngineFactoryVk failed");
                return false;
            }
            factoryRef->SetMessageCallback(diligentMessageCallback);

            EngineVkCreateInfo engineCI;
            // The shadow-caster PSOs set RasterizerDesc.DepthBiasClamp, which
            // VUID-vkGraphicsPipelineCreateInfo-pDepthBiasClamp-03460 requires
            // the core depthBiasClamp device feature for.
            engineCI.Features.DepthBiasClamp = DEVICE_FEATURE_STATE_ENABLED;
#ifndef NDEBUG
            engineCI.EnableValidation = true;
#endif
            factoryRef->CreateDeviceAndContextsVk(engineCI, &deviceRef, &contextRef);
            if (!deviceRef || !contextRef) {
                utils::error("renderer: CreateDeviceAndContextsVk failed");
                return false;
            }
            device  = deviceRef;
            context = contextRef;
            utils::info("renderer: diligent device created (%s)",
                        device->GetAdapterInfo().Description);

            SwapChainDesc scDesc;
            scDesc.ColorBufferFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
            scDesc.DepthBufferFormat = TEX_FORMAT_D32_FLOAT;
            scDesc.Width             = window.width;
            scDesc.Height            = window.height;
            // COPY_SOURCE: the screenshot path copies the backbuffer to a staging
            // texture (vkCmdCopyImageToBuffer requires VK_IMAGE_USAGE_TRANSFER_SRC_BIT
            // on the swapchain image, and the TRANSFER_SRC layout barriers trip the
            // validation layer otherwise)
            scDesc.Usage = SWAP_CHAIN_USAGE_RENDER_TARGET | SWAP_CHAIN_USAGE_COPY_SOURCE;

            NativeWindow native;
#ifdef _WIN32
            native.hWnd = windowNativeHandle();
            native.hDC  = nullptr;
#else
            SDL_PropertiesID props = SDL_GetWindowProperties(window.handle);
            Sint64 xwindow = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
            void* display =
                SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
            if (!xwindow || !display) {
                utils::error(
                    "renderer: no X11 window/display handle (force SDL_VIDEO_BACKEND=x11)");
                return false;
            }
            native.WindowId = static_cast<Uint32>(xwindow);
            native.pDisplay = display;
#endif

            factoryRef->CreateSwapChainVk(device, context, scDesc, native, &swapChainRef);
            if (!swapChainRef) {
                utils::error("renderer: CreateSwapChainVk failed");
                return false;
            }
            swapChain = swapChainRef;

            // GPU-time query for the HUD (see gpuTimeQuery above);
            // ENGINE_NO_GPU_TIME=1 disables it (A/B timing overhead)
            if (getenv("ENGINE_NO_GPU_TIME") == nullptr) {
                if (vulkanTimestampsSupported()) {
                    QueryDesc qt{QUERY_TYPE_DURATION};
                    qt.Name = "frame gpu time";
                    for (auto& q : gpuTimeQuery)
                        device->CreateQuery(qt, &q);
                    if (!gpuTimeQuery[0])
                        utils::warn("renderer: gpu time query creation failed");
                } else {
                    utils::warn("renderer: no GPU timestamps on this driver; gpu time HUD stays 0");
                }
            }

            const double eye[3]    = {0.0, 0.0, 5.0};
            const double center[3] = {0.0, 0.0, 0.0};
            const double up[3]     = {0.0, 1.0, 0.0};
            cameraLookAt(eye, center, up);
            resize(window.width, window.height);
            taaInit();
            ssaoInit();
            ssrInit();
            bloomInit();
            iblDiligentInit();
            return true;
        }

        void resize(u32 width, u32 height) override {
            if (swapChain &&
                (swapChain->GetDesc().Width != width || swapChain->GetDesc().Height != height)) {
                swapChain->Resize(width, height);
            }
            const float aspect = height != 0 ? (float)width / (float)height : 1.0f;
            // Diligent normalizes to D3D-style NDC on all backends (the Vulkan
            // backend applies the y flip internally), so a plain LH perspective
            // matches what GLTF_PBR_Renderer expects
            baseProj =
                float4x4::Projection(kCameraFovYDeg * (float)M_PI / 180.0f,
                                     aspect,
                                     kCameraNear,
                                     kCameraFar,
                                     device ? device->GetDeviceInfo().NDC.MinZ == -1 : false);
            proj = baseProj;
            taaOnResized();
        }

        float4x4 viewMatrix(void) const {
            // Camera-anchored: the matrix carries ONLY the basis rotation — the
            // translation row is zero because every renderable is placed relative
            // to the anchor (the camera eye; see diligentWorldAnchor), so the eye
            // is the origin of the rendered space. Baking the eye into the
            // matrix re-introduces the f32 cancellation at |eye| ~ 4e4 m: at
            // 39 km f32 sits on a 3.9 mm grid and view-space positions quantize,
            // so the character animation and the ground shimmer (docs/lessons.md,
            // the 2026-09-04 f32 entry — the old engine fixed exactly this with
            // the relative-to-anchor rework this mirrors).
            // Diligent uses the D3D (left-handed) matrix convention: the camera
            // looks towards view +Z (GLTF_PBR_Renderer / GLTFViewer expect it), and
            // the projection maps the near plane at view-z = +near to NDC 0. With z
            // forward the screen-right axis is f × up (the GL cross(up, f) gives the
            // left vector and horizontally mirrors the whole scene).
            // The basis is built in double (the eye/center magnitudes are ~4e4 m;
            // f32 differences there would quantize the aim to millimetres).
            double fx = camCenter[0] - camEye[0], fy = camCenter[1] - camEye[1],
                   fz       = camCenter[2] - camEye[2];
            const double fl = std::sqrt(fx * fx + fy * fy + fz * fz);
            fx /= fl;
            fy /= fl;
            fz /= fl;  // forward, maps to view +Z
            double xx       = fy * camUp[2] - fz * camUp[1];
            double xy       = fz * camUp[0] - fx * camUp[2];
            double xz       = fx * camUp[1] - fy * camUp[0];  // right = f × up
            const double xl = std::sqrt(xx * xx + xy * xy + xz * xz);
            xx /= xl;
            xy /= xl;
            xz /= xl;
            double yx     = xy * fz - xz * fy;
            double yy     = xz * fx - xx * fz;
            double yz     = xx * fy - xy * fx;  // up = x × f (re-orthogonalized)
            float4x4 view = float4x4::Identity();
            view._11      = (f32)xx;
            view._12      = (f32)yx;
            view._13      = (f32)fx;
            view._21      = (f32)xy;
            view._22      = (f32)yy;
            view._23      = (f32)fy;
            view._31      = (f32)xz;
            view._32      = (f32)yz;
            view._33      = (f32)fz;
            return view;
        }

        void draw() override {
            worldDrewThisFrame = false;

            // Read the ring slot from GPU_TIME_RING frames ago (see comment
            // at gpuTimeQuery[]): by now that frame's fence has been signaled
            // (the ring is longer than the frames in flight), so GetData
            // succeeds every frame once warmed up. Must happen BEFORE this
            // frame's BeginQuery, which discards that slot's pool queries.
            {
                IQuery* old = gpuTimeQuery[(gpuTimeRingIndex + 1) % GPU_TIME_RING].RawPtr();
                // GetData on a query that was never ended dereferences a null
                // query manager (the DEV_CHECK_ERR guard is debug-only), so
                // skip the warm-up frames until every ring slot has been ended.
                QueryDataDuration qd{};
                if (old && gpuTimeFramesSeen > GPU_TIME_RING &&
                    old->GetData(&qd, sizeof(qd), true)) {
                    gpuTimeNs = qd.Frequency ? 1e9 * (double)qd.Duration / (double)qd.Frequency : 0.0;
                    if (getenv("ENGINE_DEBUG_GPUTIME"))
                        utils::warn("gputime: %.3f ms (freq %llu dur %llu)",
                                    gpuTimeNs / 1e6,
                                    (unsigned long long)qd.Frequency,
                                    (unsigned long long)qd.Duration);
                }
            }

            gpuTimeRingIndex = (gpuTimeRingIndex + 1) % GPU_TIME_RING;
            gpuTimeFramesSeen++;
            if (gpuTimeQuery[gpuTimeRingIndex]) {
                context->BeginQuery(gpuTimeQuery[gpuTimeRingIndex]);
            }

            auto* rtv = swapChain->GetCurrentBackBufferRTV();

            const SwapChainDesc& scDesc = swapChain->GetDesc();

            frameView = viewMatrix();
            // TAA: create/resize the offscreen chain, pick this frame's jitter
            // and jitter the projection. Everything below (gltf) consumes
            // proj through diligentFrameProj().
            proj = baseProj;
            taaFrameBegin(context, frameView, proj);

            // The CSM shadow pass binds its OWN render targets (the world
            // targets are set after it), so it must run before the world RT
            // setup below.
            shadowDiligentUpdateFrame();
            {
                Diligent::ScopedDebugGroup shadowPass(context, "shadow");
                shadowDiligentRenderCascades();
            }

            if (ITextureView* worldRtv = taaColorRTV()) {
                // The world renders into the offscreen chain (linear RGBA16F +
                // RG16F motion vectors + D32 depth); the TAA resolve / blit
                // below brings it to the backbuffer before the UI passes.
                // Target size = swapchain size * renderScale (taoFrameBegin just
                // sized it) — the world viewport/scissor must match the target,
                // not the backbuffer.
                ITextureView* worldRTVs[3] = {worldRtv, taaMotionRTV(), taaNormalRTV()};
                ITextureView* worldDSV     = taaDepthDSV();
                context->SetRenderTargets(3,
                                          worldRTVs,
                                          worldDSV,
                                          RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

                u32 tw = 0, th = 0;
                taaTargetSize(&tw, &th);
                Viewport vp(0.0f, 0.0f, (float)tw, (float)th, 0.0f, 1.0f);
                context->SetViewports(1, &vp, 0, 0);
                Rect scissor(0, 0, (i32)tw, (i32)th);
                context->SetScissorRects(1, &scissor, 0, 0);

                context->ClearRenderTarget(worldRTVs[0],
                                           kClearColor,
                                           RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                context->ClearRenderTarget(worldRTVs[1],
                                           zero,
                                           RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                context->ClearRenderTarget(worldRTVs[2],
                                           zero,
                                           RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                context->ClearDepthStencil(worldDSV,
                                           CLEAR_DEPTH_FLAG,
                                           1.0f,
                                           0,
                                           RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            } else {
                // TAA chain unavailable (pre-init failure): legacy direct path.
                auto* dsv = swapChain->GetDepthBufferDSV();
                context->SetRenderTargets(1, &rtv, dsv, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                Viewport vp(0.0f, 0.0f, (float)scDesc.Width, (float)scDesc.Height, 0.0f, 1.0f);
                context->SetViewports(1, &vp, 0, 0);
                Rect scissor(0, 0, (i32)scDesc.Width, (i32)scDesc.Height);
                context->SetScissorRects(1, &scissor, 0, 0);
                context->ClearRenderTarget(rtv,
                                           kClearColor,
                                           RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                context->ClearDepthStencil(dsv,
                                           CLEAR_DEPTH_FLAG,
                                           1.0f,
                                           0,
                                           RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            }
            if (getenv("ENGINE_DEBUG_CAM")) {
                utils::warn("cam: eye %f %f %f center %f %f %f up %f %f %f",
                            camEye[0],
                            camEye[1],
                            camEye[2],
                            camCenter[0],
                            camCenter[1],
                            camCenter[2],
                            camUp[0],
                            camUp[1],
                            camUp[2]);
                utils::warn(
                    "cam: view %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | "
                    "%.4f %.4f %.4f %.4f",
                    frameView._11,
                    frameView._12,
                    frameView._13,
                    frameView._14,
                    frameView._21,
                    frameView._22,
                    frameView._23,
                    frameView._24,
                    frameView._31,
                    frameView._32,
                    frameView._33,
                    frameView._34,
                    frameView._41,
                    frameView._42,
                    frameView._43,
                    frameView._44);
                utils::warn(
                    "cam: proj %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | "
                    "%.4f %.4f %.4f %.4f",
                    proj._11,
                    proj._12,
                    proj._13,
                    proj._14,
                    proj._21,
                    proj._22,
                    proj._23,
                    proj._24,
                    proj._31,
                    proj._32,
                    proj._33,
                    proj._34,
                    proj._41,
                    proj._42,
                    proj._43,
                    proj._44);
                utils::warn("cam: fmt color=%d depth=%d bufferCount=%d",
                            (int)swapChain->GetDesc().ColorBufferFormat,
                            (int)swapChain->GetDesc().DepthBufferFormat,
                            (int)swapChain->GetDesc().BufferCount);
            }
            {
                Diligent::ScopedDebugGroup worldPass(context, "world");
                worldDraw(context);
            }
            // Resolve the offscreen world into the backbuffer: TAA accumulation
            // (when enabled) or a plain blit of the scene color. Only when the
            // world drew — the UI passes' load op (LOAD over the world, CLEAR
            // for the bare menu) keys on diligentWorldDrew().
            if (worldDrewThisFrame) {
                Diligent::ScopedDebugGroup taaResolve(context, "taa_resolve");
                taaWorldResolve(context, rtv);
            }

            bool uiDrew = false;
            if (gui::guiIsActive()) {
                {
                    Diligent::ScopedDebugGroup guiPass(context, "gui");
                    guiDraw(context);
                }
                uiDrew = true;
            }
            // rmlui pass: after the ImGui pass (draws on top of it), before the
            // screenshot capture so GUI shows up in ENGINE_SCREENSHOT frames.
            // No-op unless the wrapper queued geometry this frame.
            if (!engine::rmluiDisabled()) {
                Diligent::ScopedDebugGroup rmluiPass(context, "rmlui");
                rmluiDraw(context);
            }

            if (rendererScreenshotShouldCapture()) {
                captureScreenshot();
            }

            if (gpuTimeQuery[gpuTimeRingIndex]) {
                context->EndQuery(gpuTimeQuery[gpuTimeRingIndex]);
            }

            // The video-settings "vsync" option drives the present interval:
            // 1 = vsync (FIFO), 0 = uncapped (MAILBOX/IMMEDIATE, driver-dependent).
            // Diligent recreates the swapchain once when the value changes, so
            // toggling it in the video settings takes effect on the next frame.
            swapChain->Present(utils::settingsGetBool("vsync") ? 1 : 0);

            (void)uiDrew;  // the gui pass flushed + invalidated its own state
        }

        void captureScreenshot() {
            const SwapChainDesc& scDesc = swapChain->GetDesc();
            const u32 width             = scDesc.Width;
            const u32 height            = scDesc.Height;

            // staging texture in the backbuffer's own format (Vulkan surfaces are
            // usually B8G8R8A8); the copy is a raw texel copy, so channel order
            // follows the backbuffer's byte layout
            const TEXTURE_FORMAT backFormat = scDesc.ColorBufferFormat;
            TextureDesc stagingDesc;
            stagingDesc.Name           = "screenshot staging";
            stagingDesc.Type           = RESOURCE_DIM_TEX_2D;
            stagingDesc.Usage          = USAGE_STAGING;
            stagingDesc.BindFlags      = BIND_NONE;
            stagingDesc.CPUAccessFlags = CPU_ACCESS_READ;
            stagingDesc.Format         = backFormat;
            stagingDesc.Width          = width;
            stagingDesc.Height         = height;
            stagingDesc.MipLevels      = 1;
            RefCntAutoPtr<ITexture> staging;
            device->CreateTexture(stagingDesc, nullptr, &staging);
            if (!staging) {
                utils::warn("renderer: screenshot staging texture failed");
                return;
            }

            ITexture* backbuffer = swapChain->GetCurrentBackBufferRTV()->GetTexture();
            // backbuffer is a swapchain image in RENDER_TARGET (color-attach) layout
            // after SetRenderTargets; use explicit old-states so the barriers match
            // the tracked layouts (UNKNOWN trips the validation layer here)
            StateTransitionDesc toCopy{backbuffer,
                                       RESOURCE_STATE_RENDER_TARGET,
                                       RESOURCE_STATE_COPY_SOURCE,
                                       STATE_TRANSITION_FLAG_UPDATE_STATE};
            context->TransitionResourceStates(1, &toCopy);
            CopyTextureAttribs copyAttrs(backbuffer,
                                         RESOURCE_STATE_TRANSITION_MODE_NONE,
                                         staging,
                                         RESOURCE_STATE_TRANSITION_MODE_NONE);
            context->CopyTexture(copyAttrs);
            StateTransitionDesc back{backbuffer,
                                     RESOURCE_STATE_COPY_SOURCE,
                                     RESOURCE_STATE_RENDER_TARGET,
                                     STATE_TRANSITION_FLAG_UPDATE_STATE};
            context->TransitionResourceStates(1, &back);

            // make sure the copy landed before mapping
            context->WaitForIdle();

            MappedTextureSubresource mapped;
            context->MapTextureSubresource(staging, 0, 0, MAP_READ, MAP_FLAG_NONE, nullptr, mapped);
            if (!mapped.pData) {
                utils::warn("renderer: screenshot map failed");
                return;
            }

            u8* buffer        = (u8*)malloc((size_t)width * height * 4);
            const u8* src     = static_cast<const u8*>(mapped.pData);
            const bool bFirst = backFormat == TEX_FORMAT_BGRA8_UNORM_SRGB ||
                                backFormat == TEX_FORMAT_BGRA8_UNORM;
            for (u32 y = 0; y < height; y++) {
                const u8* row = src + (size_t)y * mapped.Stride;
                u8* dst       = buffer + (size_t)y * width * 4;
                if (bFirst) {
                    for (u32 x = 0; x < width; x++) {
                        dst[x * 4 + 0] = row[x * 4 + 2];
                        dst[x * 4 + 1] = row[x * 4 + 1];
                        dst[x * 4 + 2] = row[x * 4 + 0];
                        dst[x * 4 + 3] = row[x * 4 + 3];
                    }
                } else {
                    memcpy(dst, row, (size_t)width * 4);
                }
            }
            context->UnmapTextureSubresource(staging, 0, 0);
            rendererScreenshotDeliver(buffer);
        }

        void destroy() override {
            shadowDiligentDestroy();
            iblDiligentDestroy();
            guiOnBackendDestroy();
            if (!engine::rmluiDisabled()) {
                rmluiOnBackendDestroy();
            }
            if (context) {
                context->Flush();
                context->WaitForIdle();
            }

            taaDestroy();
            ssaoDestroy();
            ssrDestroy();
            bloomDestroy();

            swapChain = nullptr;
            device    = nullptr;
            context   = nullptr;

            for (auto& q : gpuTimeQuery)
                q.Release();
            swapChainRef.Release();
            contextRef.Release();
            deviceRef.Release();
            utils::info("renderer: diligent device released");
        }

        void cameraLookAt(const double eye[3],
                          const double center[3],
                          const double up[3]) override {
            // The camera is the world anchor: world state stays f64 here, the
            // view matrix is rotation-only, and f32 render space never carries
            // the ~4e4 m absolute magnitude (docs/lessons.md, the 2026-09-04
            // f32 entry). Passes subtract the (f64) anchor from their own world
            // state and round the SMALL difference to f32 once — a f32 anchor
            // copy would re-quantize to the 3.9 mm grid and shimmer on every
            // camera ULP crossing.
            for (int i = 0; i < 3; i++) {
                camEye[i]    = eye[i];
                camCenter[i] = center[i];
                camUp[i]     = up[i];
            }
        }

        double worldAnchorX() override { return camEye[0]; }

        double worldAnchorZ() override { return camEye[2]; }

        void cameraGet(f32 pos[3], f32 forward[3]) override {
            pos[0]    = (f32)camEye[0];
            pos[1]    = (f32)camEye[1];
            pos[2]    = (f32)camEye[2];
            double fx = camCenter[0] - camEye[0], fy = camCenter[1] - camEye[1],
                   fz       = camCenter[2] - camEye[2];
            const double fl = std::sqrt(fx * fx + fy * fy + fz * fz);
            forward[0]      = (f32)(fx / fl);
            forward[1]      = (f32)(fy / fl);
            forward[2]      = (f32)(fz / fl);
        }

        void setSun(const f32 direction[3], const f32 color[3], f32 intensity) override {
            memcpy(sunDirection, direction, sizeof(sunDirection));
            memcpy(sunColor, color, sizeof(sunColor));
            sunIntensity = intensity;
        }

        void setAmbient(const f32 color[3], f32 intensity) override {
            memcpy(ambientColor, color, sizeof(ambientColor));
            ambientIntensity = intensity;
            engine::gltf::gltfIblUpdateDiligent(color, intensity);
        }

        // Distance fog is not implemented yet on this path (no-op; the clear
        // color matches the sky so the horizon still reads correctly).
        void setFog(const f32 color[3], f32 density) override {
            (void)color;
            (void)density;
        }

        // The graphics settings page's AA section: TAA enable + history weight.
        // Other settings have no diligent equivalent yet (RenderBackend default
        // no-op documents the contract).
        void applyGraphicsSettings(const GraphicsSettings& s) override {
            taaSettingsApply(s.taa, s.taaWeight, s.casStrength, s.renderScale);
            ssaoSettingsApply(s.ssao, s.ssaoRadius, s.ssaoAlgorithm, s.ssaoIntensity);
            ssrSettingsApply(s.ssr, s.ssrStrength);
            bloomSettingsApply(s.bloom);
        }

       public:
        float4x4 frameView;
        float4x4 proj;
        float4x4 baseProj;  // unjittered (taaFrameBegin writes the jittered one into proj)
        f64 camEye[3]        = {0.0, 0.0, 5.0};
        f32 sunDirection[3]  = {-0.6f, -1.0f, -0.5f};
        f32 sunColor[3]      = {1.0f, 0.97f, 0.92f};
        f32 sunIntensity     = 0.0f;
        f32 ambientColor[3]  = {0.32f, 0.35f, 0.38f};
        f32 ambientIntensity = 0.0f;

       private:
        f64 camCenter[3] = {0.0, 0.0, 0.0};
        f64 camUp[3]     = {0.0, 1.0, 0.0};
    };

    static DiligentBackend* gDiligentBackend = nullptr;

}

namespace engine::renderer {

    RenderBackend* diligentBackendCreate(void) {
        engine::renderer::diligent::gDiligentBackend =
            new engine::renderer::diligent::DiligentBackend();
        return engine::renderer::diligent::gDiligentBackend;
    }

}

namespace engine::renderer::diligent {

    const float4x4& diligentFrameView(void) {
        return gDiligentBackend->frameView;
    }

    double diligentGpuTimeNs(void) {
        return gpuTimeNs;
    }

    bool diligentGpuTimeSupported(void) {
        return gpuTimeQuery[0] != nullptr;
    }

    void diligentWorldAnchor(f64 out[3]) {
        out[0] = gDiligentBackend->camEye[0];
        out[1] = gDiligentBackend->camEye[1];
        out[2] = gDiligentBackend->camEye[2];
    }

    const float4x4& diligentFrameProj(void) {
        return gDiligentBackend->proj;
    }

    // Unjittered projection: the cascade distribution must not follow the
    // TAA jitter (stable cascades, the jitter only perturbs the main view).
    const float4x4& diligentBaseProj(void) {
        return gDiligentBackend->baseProj;
    }

    const f32* diligentSunDirection(void) {
        return gDiligentBackend->sunDirection;
    }

    const f32* diligentSunColor(void) {
        return gDiligentBackend->sunColor;
    }

    f32 diligentSunIntensity(void) {
        return gDiligentBackend->sunIntensity;
    }

    const f32* diligentAmbientColor(void) {
        return gDiligentBackend->ambientColor;
    }

    f32 diligentAmbientIntensity(void) {
        return gDiligentBackend->ambientIntensity;
    }

}
