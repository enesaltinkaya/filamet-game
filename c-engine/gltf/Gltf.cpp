#include "gltf/Gltf.h"

#include "Utils.h"
#include "gltf/GltfInternal.h"
#include "logger/Logger.h"
#include "renderer/Renderer.h"

#include <cmath>

namespace engine::gltf {

static bool diligent(void) {
    return renderer::rendererBackend() == renderer::Backend::Diligent;
}

bool gltfInit(void) {
    return diligent() ? gltfInitDiligent() : gltfInitFilament();
}

bool gltfLoad(const char* pakPath) {
    return diligent() ? gltfLoadDiligent(pakPath) : gltfLoadFilament(pakPath);
}

bool gltfPlaceAt(f32 x, f32 y, f32 z) {
    return diligent() ? gltfPlaceAtDiligent(x, y, z) : gltfPlaceAtFilament(x, y, z);
}

void gltfUpdate(double elapsedSeconds) {
    diligent() ? gltfUpdateDiligent(elapsedSeconds) : gltfUpdateFilament(elapsedSeconds);
}

bool gltfBoundingBox(f32 min[3], f32 max[3]) {
    return diligent() ? gltfBoundingBoxDiligent(min, max) : gltfBoundingBoxFilament(min, max);
}


void gltfFrameCamera(void) {
    f32 min[3];
    f32 max[3];
    if (!gltfBoundingBox(min, max)) {
        return;
    }

    f32 center[3] = {(min[0] + max[0]) * 0.5f, (min[1] + max[1]) * 0.5f, (min[2] + max[2]) * 0.5f};
    f32 extent[3] = {max[0] - min[0], max[1] - min[1], max[2] - min[2]};
    f32 radius = sqrtf(extent[0] * extent[0] + extent[1] * extent[1] + extent[2] * extent[2]) * 0.5f;
    if (radius <= 0.0f) {
        radius = 1.0f;
    }

    // rendererInit uses a 60 degree vertical fov
    f32 distance = (radius / tanf(30.0f * (f32)M_PI / 180.0f)) * 1.2f;
    f32 dirLen = sqrtf(0.5f * 0.5f + 0.3f * 0.3f + 1.0f);
    f32 direction[3] = {0.5f / dirLen, 0.3f / dirLen, 1.0f / dirLen};
    f32 eye[3] = {center[0] + direction[0] * distance, center[1] + direction[1] * distance,
            center[2] + direction[2] * distance};
    const f32 up[3] = {0.0f, 1.0f, 0.0f};
    renderer::rendererCameraLookAt(eye, center, up);
}

void gltfDestroy(void) {
    diligent() ? gltfDestroyDiligent() : gltfDestroyFilament();
}

}  // namespace engine::gltf
