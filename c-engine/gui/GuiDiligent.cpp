#include "gui/GuiBackend.h"

#include "Utils.h"
#include "logger/Logger.h"
#include "renderer/RenderBackend.h"
#include "renderer/diligent/DiligentRenderer.h"

#include <Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <Graphics/GraphicsEngine/interface/SwapChain.h>
#include <Graphics/GraphicsEngine/interface/Texture.h>
#include <Common/interface/RefCntAutoPtr.hpp>

// vulkan types via volk (Diligent initializes it in its factory); the imgui
// vulkan backend resolves the vk* loaders through the same globals
#define VK_NO_PROTOTYPES
#include <volk.h>

#include <Graphics/GraphicsEngineVulkan/interface/CommandQueueVk.h>
#include <Graphics/GraphicsEngineVulkan/interface/DeviceContextVk.h>
#include <Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h>
#include <Graphics/GraphicsEngineVulkan/interface/RenderDeviceVk.h>
#include <Graphics/GraphicsEngineVulkan/interface/TextureViewVk.h>
#include <Graphics/GraphicsEngineVulkan/interface/TextureVk.h>

#define IMGUI_IMPL_VULKAN_USE_VOLK
#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#include <backends/imgui_impl_vulkan.h>

#include <vector>

namespace engine::gui {

using namespace Diligent;
using engine::renderer::diligent::device;
using engine::renderer::diligent::context;
using engine::renderer::diligent::swapChain;

struct GuiTexture {
    RefCntAutoPtr<ITexture> texture;   // Diligent texture (owns the image)
    VkImageView imageView = VK_NULL_HANDLE;  // our own view for imgui's descriptor
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;  // imgui-owned
};
static std::vector<GuiTexture> uiTextures;

static bool vulkanReady = false;
static VkFormat uiColorFormat = VK_FORMAT_R8G8B8A8_SRGB;  // outlives imgui init
static RefCntAutoPtr<IDeviceContextVk> contextVk;
static VkQueue vkQueue = VK_NULL_HANDLE;
static uint32_t vkQueueFamily = 0;
static VkSampler linearSampler = VK_NULL_HANDLE;  // gui textures' sampler (destroyed in guiBackendDestroyDiligent)

static void checkVk(VkResult err) {
    if (err != VK_SUCCESS) {
        utils::warn("gui: vulkan error %d in imgui backend", (int)err);
    }
}

// Diligent's QueryInterface fills an IObject**; attach it to a typed smart
// pointer without an extra AddRef (all Diligent interfaces singly derive IObject)
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

void guiBackendInitDiligent(void) {
    RefCntAutoPtr<IRenderDeviceVk> deviceVk = queryVkInterface<IRenderDeviceVk>(device, IID_RenderDeviceVk);
    contextVk = queryVkInterface<IDeviceContextVk>(context, IID_DeviceContextVk);
    if (!deviceVk || !contextVk) {
        utils::warn("gui: vulkan interfaces unavailable");
        return;
    }

    // the context's command queue owns the VkQueue + family index imgui needs
    if (ICommandQueue* queue = context->LockCommandQueue()) {
        RefCntAutoPtr<ICommandQueueVk> queueVk = queryVkInterface<ICommandQueueVk>(queue, IID_CommandQueueVk);
        if (queueVk) {
            vkQueue = queueVk->GetVkQueue();
            vkQueueFamily = queueVk->GetQueueFamilyIndex();
        }
        context->UnlockCommandQueue();
    }

    // Diligent records into dynamic rendering (Vulkan 1.3+), never a legacy
    // VkRenderPass — imgui must create its pipeline for the same mode, with
    // the swapchain color format (the surface may substitute BGRA for RGBA)
    switch (swapChain->GetDesc().ColorBufferFormat) {
        case TEX_FORMAT_RGBA8_UNORM_SRGB: uiColorFormat = VK_FORMAT_R8G8B8A8_SRGB; break;
        case TEX_FORMAT_BGRA8_UNORM_SRGB: uiColorFormat = VK_FORMAT_B8G8R8A8_SRGB; break;
        case TEX_FORMAT_RGBA8_UNORM: uiColorFormat = VK_FORMAT_R8G8B8A8_UNORM; break;
        case TEX_FORMAT_BGRA8_UNORM: uiColorFormat = VK_FORMAT_B8G8R8A8_UNORM; break;
        default: uiColorFormat = VK_FORMAT_B8G8R8A8_SRGB; break;
    }

    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = deviceVk->GetVkVersion();
    init.Instance = deviceVk->GetVkInstance();
    init.PhysicalDevice = deviceVk->GetVkPhysicalDevice();
    init.Device = deviceVk->GetVkDevice();
    init.QueueFamily = vkQueueFamily;
    init.Queue = vkQueue;
    init.DescriptorPoolSize = 16;  // let imgui allocate its own pool
    const SwapChainDesc& scDesc = swapChain->GetDesc();
    init.MinImageCount = scDesc.BufferCount;
    init.ImageCount = scDesc.BufferCount;
    init.UseDynamicRendering = true;
    init.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &uiColorFormat;
    init.CheckVkResultFn = checkVk;

    vulkanReady = ImGui_ImplVulkan_Init(&init);
    if (!vulkanReady) {
        utils::warn("gui: ImGui_ImplVulkan_Init failed");
    }
}

void guiBackendDestroyDiligent(void) {
    static char destroyed = 0;  // removed() may run twice (see ecsDestroy)
    if (destroyed) {
        return;
    }
    destroyed = 1;
    // textures must be removed BEFORE ImGui_ImplVulkan_Shutdown: the backend
    // owns the descriptor pool the RemoveTexture call writes to
    RefCntAutoPtr<IRenderDeviceVk> deviceVk;
    if (device) {
        deviceVk = queryVkInterface<IRenderDeviceVk>(device, IID_RenderDeviceVk);
    }
    for (GuiTexture& t : uiTextures) {
        if (t.descriptorSet != VK_NULL_HANDLE && vulkanReady) {
            ImGui_ImplVulkan_RemoveTexture(t.descriptorSet);
        }
        if (t.imageView != VK_NULL_HANDLE && deviceVk) {
            vkDestroyImageView(deviceVk->GetVkDevice(), t.imageView, nullptr);
        }
        t.texture.Release();
    }
    uiTextures.clear();

    // sampler created in guiTextureCreateDiligent — must be gone before the
    // device (destroyed right after this hook in DiligentBackend::destroy)
    if (linearSampler != VK_NULL_HANDLE && deviceVk) {
        vkDestroySampler(deviceVk->GetVkDevice(), linearSampler, nullptr);
        linearSampler = VK_NULL_HANDLE;
    }

    if (vulkanReady) {
        ImGui_ImplVulkan_Shutdown();
        vulkanReady = false;
    }

    contextVk.Release();
    ImGui::DestroyContext(ImGui::GetCurrentContext());
}

void guiBackendFrameDiligent(float dt, u32 width, u32 height, void (*drawGuis)()) {
    if (!vulkanReady) {
        return;
    }
    (void)dt;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)width, (float)height);

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    drawGuis();
    ImGui::Render();
}

// hooks for the diligent frame loop (called via GuiBackend's export) — the
// UI pass runs in its own dynamic-rendering scope on the context's command
// buffer, between the world batch and Present
void guiDiligentDraw(Diligent::IDeviceContext* ctx) {
    if (!vulkanReady) {
        return;
    }
    ImDrawData* drawData = ImGui::GetDrawData();
    if (!drawData || drawData->CmdListsCount == 0) {
        return;
    }

    RefCntAutoPtr<ITextureViewVk> rtvVk = queryVkInterface<ITextureViewVk>(
            swapChain->GetCurrentBackBufferRTV(), IID_TextureViewVk);
    if (!rtvVk) {
        return;
    }

    // Diligent lazily opens its render scope and closes it on flush; end the
    // world batch so our raw scope is the only one open
    const bool worldDrew = engine::renderer::diligent::diligentWorldDrew();
    context->Flush();

    const SwapChainDesc& scDesc = swapChain->GetDesc();

    VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttachment.imageView = rtvVk->GetVulkanImageView();
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // with no world geometry (menu), the backbuffer is still UNDEFINED and the
    // UI scope performs the clear itself (Diligent's clear is deferred until
    // its first draw, which never comes)
    colorAttachment.loadOp = worldDrew ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.clearValue.color = {
            {engine::renderer::kClearColor[0], engine::renderer::kClearColor[1],
                    engine::renderer::kClearColor[2], engine::renderer::kClearColor[3]}};
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea = {{0, 0}, {scDesc.Width, scDesc.Height}};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

    VkCommandBuffer cmd = contextVk->GetVkCommandBuffer();
    vkCmdBeginRendering(cmd, &renderingInfo);
    ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
    vkCmdEndRendering(cmd);

    // imgui recorded raw vulkan commands — Diligent's state tracking is stale,
    // so submit the batch and reset the context to a clean slate for the next
    // frame (Present then only handles the backbuffer transition)
    context->Flush();
    context->InvalidateState();
}

ImTextureID guiTextureCreateDiligent(u32 width, u32 height, u8* rgbaPixels) {
    if (!device || !rgbaPixels) {
        free(rgbaPixels);
        return ImTextureID_Invalid;
    }

    TextureDesc texDesc;
    texDesc.Name = "gui texture";
    texDesc.Type = RESOURCE_DIM_TEX_2D;
    texDesc.Usage = USAGE_IMMUTABLE;
    texDesc.BindFlags = BIND_SHADER_RESOURCE;
    texDesc.Format = TEX_FORMAT_RGBA8_UNORM;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;

    std::vector<TextureSubResData> subresource{TextureSubResData(rgbaPixels, (Uint64)width * 4)};
    TextureData initData(subresource.data(), 1);
    RefCntAutoPtr<ITexture> texture;
    device->CreateTexture(texDesc, &initData, &texture);
    free(rgbaPixels);  // Diligent copies the init data synchronously
    if (!texture) {
        return ImTextureID_Invalid;
    }

    // imgui samples it through a raw descriptor: Diligent must complete the
    // upload and transition the image to shader-readable first
    StateTransitionDesc barrier{texture, RESOURCE_STATE_UNKNOWN,
            RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE};
    context->TransitionResourceStates(1, &barrier);

    RefCntAutoPtr<ITextureVk> textureVk = queryVkInterface<ITextureVk>(texture.RawPtr(), IID_TextureVk);
    RefCntAutoPtr<IRenderDeviceVk> deviceVk = queryVkInterface<IRenderDeviceVk>(device, IID_RenderDeviceVk);
    if (!textureVk || !deviceVk) {
        return ImTextureID_Invalid;
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = textureVk->GetVkImage();
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(deviceVk->GetVkDevice(), &viewInfo, nullptr, &view) != VK_SUCCESS) {
        return ImTextureID_Invalid;
    }

    if (linearSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        checkVk(vkCreateSampler(deviceVk->GetVkDevice(), &samplerInfo, nullptr, &linearSampler));
    }

    VkDescriptorSet descriptor =
            ImGui_ImplVulkan_AddTexture(linearSampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (descriptor == VK_NULL_HANDLE) {
        vkDestroyImageView(deviceVk->GetVkDevice(), view, nullptr);
        return ImTextureID_Invalid;
    }

    GuiTexture guiTex;
    guiTex.texture = std::move(texture);
    guiTex.imageView = view;
    guiTex.descriptorSet = descriptor;
    uiTextures.push_back(std::move(guiTex));
    return ImTextureID((size_t)descriptor);
}

void guiTextureDestroyDiligent(ImTextureID texture) {
    if (texture == ImTextureID_Invalid) {
        return;
    }
    VkDescriptorSet descriptor = (VkDescriptorSet)(size_t)texture;
    for (size_t i = 0; i < uiTextures.size(); i++) {
        if (uiTextures[i].descriptorSet == descriptor) {
            RefCntAutoPtr<IRenderDeviceVk> deviceVk = queryVkInterface<IRenderDeviceVk>(device, IID_RenderDeviceVk);
            ImGui_ImplVulkan_RemoveTexture(descriptor);
            if (deviceVk && uiTextures[i].imageView != VK_NULL_HANDLE) {
                vkDestroyImageView(deviceVk->GetVkDevice(), uiTextures[i].imageView, nullptr);
            }
            uiTextures.erase(uiTextures.begin() + (i64)i);
            return;
        }
    }
}

}  // namespace engine::gui

// frame-loop hooks (declared in renderer/diligent/DiligentRenderer.h); the
// gui work lives in engine::gui, normally torn down with the gui system before
// the renderer dies
namespace engine::renderer::diligent {

void guiDraw(Diligent::IDeviceContext* ctx) {
    engine::gui::guiDiligentDraw(ctx);
}

void guiOnBackendDestroy(void) {
    // nothing extra: ImGui_ImplVulkan resources are released by
    // guiBackendDestroyDiligent (GuiManagerSystem::removed, which runs before
    // rendererDestroy via ecsDestroy)
}

}  // namespace engine::renderer::diligent
