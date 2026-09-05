#include "renderer/diligent/DiligentRenderer.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Engine.h"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/SwapChain.h"
#include "Platforms/interface/NativeWindow.h"
#include "Utils.h"
#include "ecs/system/heightmap/HeightmapTerrainRender.h"
#include "gltf/GltfInternal.h"
#include "gui/GuiManager.h"
#include "logger/Logger.h"
#include "renderer/RenderBackend.h"
#include "renderer/Window.h"
#include "renderer/PropsRender.h"
#include "renderer/diligent/HeightmapTerrainDiligent.h"
#include "renderer/diligent/PropsRenderDiligent.h"

#include <SDL.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

// vulkan types for the Diligent Vk interfaces below (resolved through volk,
// which Diligent initializes inside its engine factory)
#define VK_NO_PROTOTYPES
#include <volk.h>

#include "Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"

using namespace Diligent;

namespace engine::renderer::diligent {

IRenderDevice* device = nullptr;
IDeviceContext* context = nullptr;
ISwapChain* swapChain = nullptr;

static bool worldDrewThisFrame = false;

bool diligentWorldDrew(void) {
    return worldDrewThisFrame;
}

void setWorldDrew(bool drew) {
    worldDrewThisFrame = drew;
}

static RefCntAutoPtr<IRenderDevice> deviceRef;
static RefCntAutoPtr<IDeviceContext> contextRef;
static RefCntAutoPtr<ISwapChain> swapChainRef;
static RefCntAutoPtr<IEngineFactoryVk> factoryRef;

class DiligentBackend final : public RenderBackend {
public:
    bool init() override {
        factoryRef = GetEngineFactoryVk();
        if (!factoryRef) {
            utils::error("renderer: GetEngineFactoryVk failed");
            return false;
        }

        EngineVkCreateInfo engineCI;
#ifndef NDEBUG
        engineCI.EnableValidation = true;
#endif
        factoryRef->CreateDeviceAndContextsVk(engineCI, &deviceRef, &contextRef);
        if (!deviceRef || !contextRef) {
            utils::error("renderer: CreateDeviceAndContextsVk failed");
            return false;
        }
        device = deviceRef;
        context = contextRef;
        utils::info("renderer: diligent device created (%s)", device->GetAdapterInfo().Description);

        // Attach the swapchain to the SDL window's native handle (Xlib on
        // linux, HWND on windows)
        SwapChainDesc scDesc;
        scDesc.ColorBufferFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
        scDesc.DepthBufferFormat = TEX_FORMAT_D32_FLOAT;
        scDesc.Width = window.width;
        scDesc.Height = window.height;
        // COPY_SOURCE: the screenshot path copies the backbuffer to a staging
        // texture (vkCmdCopyImageToBuffer requires VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        // on the swapchain image, and the TRANSFER_SRC layout barriers trip the
        // validation layer otherwise)
        scDesc.Usage = SWAP_CHAIN_USAGE_RENDER_TARGET | SWAP_CHAIN_USAGE_COPY_SOURCE;

        NativeWindow native;
#ifdef _WIN32
        native.hWnd = windowNativeHandle();
        native.hDC = nullptr;
#else
        SDL_PropertiesID props = SDL_GetWindowProperties(window.handle);
        Sint64 xwindow = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        void* display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        if (!xwindow || !display) {
            utils::error("renderer: no X11 window/display handle (force SDL_VIDEO_BACKEND=x11)");
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

        const double eye[3] = {0.0, 0.0, 5.0};
        const double center[3] = {0.0, 0.0, 0.0};
        const double up[3] = {0.0, 1.0, 0.0};
        cameraLookAt(eye, center, up);
        resize(window.width, window.height);
        return true;
    }

    void resize(u32 width, u32 height) override {
        if (swapChain && (swapChain->GetDesc().Width != width || swapChain->GetDesc().Height != height)) {
            swapChain->Resize(width, height);
        }
        const float aspect = height != 0 ? (float)width / (float)height : 1.0f;
        // Diligent normalizes to D3D-style NDC on all backends (the Vulkan
        // backend applies the y flip internally), so a plain LH perspective
        // matches what GLTF_PBR_Renderer expects
        proj = float4x4::Projection(kCameraFovYDeg * (float)M_PI / 180.0f, aspect,
                kCameraNear, kCameraFar, device ? device->GetDeviceInfo().NDC.MinZ == -1 : false);
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
        //
        // Diligent uses the D3D (left-handed) matrix convention: the camera
        // looks towards view +Z (GLTF_PBR_Renderer / GLTFViewer expect it), and
        // the projection maps the near plane at view-z = +near to NDC 0. With z
        // forward the screen-right axis is f × up (the GL cross(up, f) gives the
        // left vector and horizontally mirrors the whole scene).
        //
        // The basis is built in double (the eye/center magnitudes are ~4e4 m;
        // f32 differences there would quantize the aim to millimetres).
        double fx = camCenter[0] - camEye[0], fy = camCenter[1] - camEye[1], fz = camCenter[2] - camEye[2];
        const double fl = std::sqrt(fx * fx + fy * fy + fz * fz);
        fx /= fl; fy /= fl; fz /= fl;  // forward, maps to view +Z
        double xx = fy * camUp[2] - fz * camUp[1];
        double xy = fz * camUp[0] - fx * camUp[2];
        double xz = fx * camUp[1] - fy * camUp[0];  // right = f × up
        const double xl = std::sqrt(xx * xx + xy * xy + xz * xz);
        xx /= xl; xy /= xl; xz /= xl;
        double yx = xy * fz - xz * fy;
        double yy = xz * fx - xx * fz;
        double yz = xx * fy - xy * fx;  // up = x × f (re-orthogonalized)
        float4x4 view = float4x4::Identity();
        view._11 = (f32)xx; view._12 = (f32)yx; view._13 = (f32)fx;
        view._21 = (f32)xy; view._22 = (f32)yy; view._23 = (f32)fy;
        view._31 = (f32)xz; view._32 = (f32)yz; view._33 = (f32)fz;
        return view;
    }

    void draw() override {
        worldDrewThisFrame = false;

        auto* rtv = swapChain->GetCurrentBackBufferRTV();
        auto* dsv = swapChain->GetDepthBufferDSV();

        context->SetRenderTargets(1, &rtv, dsv, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        const SwapChainDesc& scDesc = swapChain->GetDesc();
        Viewport vp(0.0f, 0.0f, (float)scDesc.Width, (float)scDesc.Height, 0.0f, 1.0f);
        context->SetViewports(1, &vp, 0, 0);
        Rect scissor(0, 0, (i32)scDesc.Width, (i32)scDesc.Height);        context->SetScissorRects(1, &scissor, 0, 0);

        context->ClearRenderTarget(rtv, kClearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->ClearDepthStencil(dsv, CLEAR_DEPTH_FLAG, 1.0f, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        frameView = viewMatrix();
        if (getenv("ENGINE_DEBUG_CAM")) {
            utils::warn("cam: eye %f %f %f center %f %f %f up %f %f %f",
                    camEye[0], camEye[1], camEye[2], camCenter[0], camCenter[1], camCenter[2],
                    camUp[0], camUp[1], camUp[2]);
            utils::warn("cam: view %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
                    frameView._11, frameView._12, frameView._13, frameView._14,
                    frameView._21, frameView._22, frameView._23, frameView._24,
                    frameView._31, frameView._32, frameView._33, frameView._34,
                    frameView._41, frameView._42, frameView._43, frameView._44);
            utils::warn("cam: proj %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
                    proj._11, proj._12, proj._13, proj._14,
                    proj._21, proj._22, proj._23, proj._24,
                    proj._31, proj._32, proj._33, proj._34,
                    proj._41, proj._42, proj._43, proj._44);
            utils::warn("cam: fmt color=%d depth=%d bufferCount=%d", (int)swapChain->GetDesc().ColorBufferFormat,
                    (int)swapChain->GetDesc().DepthBufferFormat, (int)swapChain->GetDesc().BufferCount);
        }
        // heightmap terrain: budgeted GPU tile uploads (the pass lazily
        // inits on the first update)
        heightmapTerrainRenderUpdate();
        // azgaar props: apply queued scatter tiles (budgeted, nearest first)
        propsRenderUpdate();

        worldDraw(context);
        // terrain draws over the same render targets after the glTF PBR draw;
        // it calls setWorldDrew(true) itself when it actually drew
        heightmapTerrainDiligentDraw();
        // props draw last among the opaque world passes so the depth test
        // resolves against terrain + model (contact bases sit exactly on the
        // physics surface; the PSO depth func is LESS_EQUAL for them)
        propsRenderDiligentDraw();

        bool uiDrew = false;
        if (gui::guiIsActive()) {
            guiDraw(context);
            uiDrew = true;
        }
        // rmlui pass: after the ImGui pass (draws on top of it), before the
        // screenshot capture so GUI shows up in ENGINE_SCREENSHOT frames.
        // No-op unless the wrapper queued geometry this frame.
        rmluiDraw(context);

        if (rendererScreenshotShouldCapture()) {
            captureScreenshot();
        }

        swapChain->Present();
        (void)uiDrew;  // the gui pass flushed + invalidated its own state
    }

    void captureScreenshot() {
        const SwapChainDesc& scDesc = swapChain->GetDesc();
        const u32 width = scDesc.Width;
        const u32 height = scDesc.Height;

        // staging texture in the backbuffer's own format (Vulkan surfaces are
        // usually B8G8R8A8); the copy is a raw texel copy, so channel order
        // follows the backbuffer's byte layout
        const TEXTURE_FORMAT backFormat = scDesc.ColorBufferFormat;
        TextureDesc stagingDesc;
        stagingDesc.Name = "screenshot staging";
        stagingDesc.Type = RESOURCE_DIM_TEX_2D;
        stagingDesc.Usage = USAGE_STAGING;
        stagingDesc.BindFlags = BIND_NONE;
        stagingDesc.CPUAccessFlags = CPU_ACCESS_READ;
        stagingDesc.Format = backFormat;
        stagingDesc.Width = width;
        stagingDesc.Height = height;
        stagingDesc.MipLevels = 1;
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
        StateTransitionDesc toCopy{backbuffer, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_COPY_SOURCE,
                STATE_TRANSITION_FLAG_UPDATE_STATE};
        context->TransitionResourceStates(1, &toCopy);
        CopyTextureAttribs copyAttrs(backbuffer, RESOURCE_STATE_TRANSITION_MODE_NONE,
                staging, RESOURCE_STATE_TRANSITION_MODE_NONE);
        context->CopyTexture(copyAttrs);
        StateTransitionDesc back{backbuffer, RESOURCE_STATE_COPY_SOURCE, RESOURCE_STATE_RENDER_TARGET,
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

        u8* buffer = (u8*)malloc((size_t)width * height * 4);
        const u8* src = static_cast<const u8*>(mapped.pData);
        const bool bFirst = backFormat == TEX_FORMAT_BGRA8_UNORM_SRGB || backFormat == TEX_FORMAT_BGRA8_UNORM;
        for (u32 y = 0; y < height; y++) {
            const u8* row = src + (size_t)y * mapped.Stride;
            u8* dst = buffer + (size_t)y * width * 4;
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
        // terrain + props GPU state first (the props' borrowed glTF GGX LUT
        // texture view must be released while the glTF pass — and the device
        // — still live)
        heightmapTerrainRenderDestroy();
        propsRenderDestroy();
        guiOnBackendDestroy();
        rmluiOnBackendDestroy();
        if (context) {
            context->Flush();
            context->WaitForIdle();
        }

        swapChain = nullptr;
        device = nullptr;
        context = nullptr;

        swapChainRef.Release();
        contextRef.Release();
        deviceRef.Release();
        utils::info("renderer: diligent device released");
    }

    void cameraLookAt(const double eye[3], const double center[3], const double up[3]) override {
        // The camera is the world anchor: world state stays f64 here, the
        // view matrix is rotation-only, and f32 render space never carries
        // the ~4e4 m absolute magnitude (docs/lessons.md, the 2026-09-04
        // f32 entry). Passes subtract the (f64) anchor from their own world
        // state and round the SMALL difference to f32 once — a f32 anchor
        // copy would re-quantize to the 3.9 mm grid and shimmer on every
        // camera ULP crossing.
        for (int i = 0; i < 3; i++) {
            camEye[i] = eye[i];
            camCenter[i] = center[i];
            camUp[i] = up[i];
        }
    }

    double worldAnchorX() override {
        return camEye[0];
    }

    double worldAnchorZ() override {
        return camEye[2];
    }

    void cameraGet(f32 pos[3], f32 forward[3]) override {
        pos[0] = (f32)camEye[0];
        pos[1] = (f32)camEye[1];
        pos[2] = (f32)camEye[2];
        double fx = camCenter[0] - camEye[0], fy = camCenter[1] - camEye[1], fz = camCenter[2] - camEye[2];
        const double fl = std::sqrt(fx * fx + fy * fy + fz * fz);
        forward[0] = (f32)(fx / fl);
        forward[1] = (f32)(fy / fl);
        forward[2] = (f32)(fz / fl);
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

public:
    float4x4 frameView;
    float4x4 proj;
    f64 camEye[3] = {0.0, 0.0, 5.0};  // public: the world anchor (diligentWorldAnchor)
    f32 sunDirection[3] = {-0.6f, -1.0f, -0.5f};
    f32 sunColor[3] = {1.0f, 0.97f, 0.92f};
    f32 sunIntensity = 0.0f;
    f32 ambientColor[3] = {0.32f, 0.35f, 0.38f};
    f32 ambientIntensity = 0.0f;

private:
    f64 camCenter[3] = {0.0, 0.0, 0.0};
    f64 camUp[3] = {0.0, 1.0, 0.0};
};

static DiligentBackend* gDiligentBackend = nullptr;

}  // namespace engine::renderer::diligent

namespace engine::renderer {

RenderBackend* diligentBackendCreate(void) {
    engine::renderer::diligent::gDiligentBackend = new engine::renderer::diligent::DiligentBackend();
    return engine::renderer::diligent::gDiligentBackend;
}

}  // namespace engine::renderer
// ── world/gui access to the current frame state (GltfDiligent/TerrainDiligent) ──
namespace engine::renderer::diligent {

const float4x4& diligentFrameView(void) {
    return gDiligentBackend->frameView;
}

// The world anchor (f64 camera eye): the origin of the render space. The view
// matrix is rotation-only; passes subtract this from their own f64 world
// state and round the small difference to f32 (the glTF placement on the
// CPU, the per-tile terrain translation in the instance buffer).
void diligentWorldAnchor(f64 out[3]) {
    out[0] = gDiligentBackend->camEye[0];
    out[1] = gDiligentBackend->camEye[1];
    out[2] = gDiligentBackend->camEye[2];
}
const float4x4& diligentFrameProj(void) {
    return gDiligentBackend->proj;
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

}  // namespace engine::renderer::diligent
