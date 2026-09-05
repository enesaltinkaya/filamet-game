#include "renderer/PropsRender.h"

namespace engine {

/*
 * The props (vegetation) render pass currently has no backend
 * implementation: the filament half was removed with the filament
 * backend, and the diligent mirror is still TODO (the old pass lives on
 * as reference at game-001-cpp's VulkanAzgaarPropsPass; the filament
 * implementation in git history is the look/noise parity baseline).
 * Every entry point below is a documented no-op so the game's scatter
 * bridge keeps compiling and calling; nothing reaches the GPU.
 */

void propsRenderSetMesh(const float* /*verts*/, u32 /*vertCount*/, const u32* /*idx*/, u32 /*idxCount*/) {
}

void propsRenderSetVariants(const PropsRenderMeshVariant* /*variants*/, u32 /*variantCount*/) {
}

void propsRenderSetTile(i32 /*tileX*/, i32 /*tileZ*/, u64 /*readyStamp*/,
        const PropsRenderInstance* /*instances*/, u32 /*instanceCount*/,
        const PropsRenderRange* /*ranges*/, u32 /*rangeCount*/) {
}

void propsRenderClearAll(void) {
}

void propsRenderSetWind(float /*dirX*/, float /*dirZ*/, float /*speed*/, float /*strength*/) {
}

void propsRenderSetEnabled(bool /*enabled*/) {
}

PropsRenderStats propsRenderStats(void) {
    return {};
}

void propsRenderUpdate(void) {
}

void propsRenderDestroy(void) {
}
}  // namespace engine
