#pragma once

// The Diligent half of the rmlui render path (the old engine's
// VulkanRmluiPass, re-implemented for the Diligent backend — .pi/ledger
// approach A).
//
// The crmlui wrapper is render-agnostic: RmlUi compiles UI geometry once
// (compileGeometry) and, every frame during rmlRenderVulkan (called from
// ecsPostUpdate), calls the RmlParams.vulkan callbacks (renderGeometry,
// scissor, transform, viewport). Those callbacks QUEUE draw work CPU-side;
// the actual upload + draw happens in rmluiDraw(context), a renderer hook
// beside guiDraw (the same queue/draw split the ImGui pass uses):
//
//   - geometry: a CPU pool keyed by handle (RmlUi keeps geometries alive
//     across frames and re-renders them every frame);
//   - textures: a pool of Diligent RGBA8 textures (loaded pak images +
//     font-atlas generateTexture), one texture batch per draw (consecutive
//     geometry sharing a texture + scissor merges into one DrawIndexed);
//   - upload: ONE per-frame dynamic vertex buffer + index buffer (the
//     docs/lessons.md "Diligent dynamic buffers are per-frame scratch"
//     rule: USAGE_DYNAMIC, re-uploaded every frame) and a 128-byte dynamic
//     frame cbuffer (ortho + transform).
//
// RmlVertex (crmlui.h) maps 1:1 onto the shader's 20-byte input layout —
// see shaders/rmlui_ui_vs.hlsl.

#include "Defines.h"

#include <cstddef>

namespace Diligent {
struct IDeviceContext;
}

struct RmlVertex;  // crmlui.h

namespace engine::renderer::diligent {

// Frame-loop hooks (DiligentRenderer::draw / destroy, beside guiDraw /
// guiOnBackendDestroy; no-ops while no rmlui geometry is queued).
void rmluiDraw(Diligent::IDeviceContext* ctx);
void rmluiOnBackendDestroy(void);

// Force-create the GPU pass (shaders + PSO + buffers); also runs lazily on
// the first frame with queued geometry. Safe from the update phase.
void rmluiPassInit(void);

// RmlParams.vulkan callback targets (wired by whoever builds the RmlParams:
// the smoke system for now, GuiManagerRmlUi later). All run on the main
// thread during rmlUpdate/rmlRenderVulkan — none of them may touch the
// device context.
void rmlPassBeginFrame(void);
void rmlPassEndFrame(void);
uintptr_t rmlPassCompileGeometry(RmlVertex* vertices, int numVertices, const int* indices, int numIndices);
void rmlPassReleaseGeometry(uintptr_t geometryHandle);
void rmlPassRenderGeometry(uintptr_t geometryHandle, float translationX, float translationY, uintptr_t texture);
uintptr_t rmlPassLoadTexture(int* outX, int* outY, const char* path);
uintptr_t rmlPassGenerateTexture(const unsigned char* data, size_t size, int x, int y);
void rmlPassReleaseTexture(uintptr_t textureHandle);
void rmlPassEnableScissorRegion(char enable);
void rmlPassSetScissorRegion(int x, int y, int width, int height);
void rmlPassSetTransform(void* transform);
void rmlPassSetViewport(int width, int height);

}  // namespace engine::renderer::diligent
