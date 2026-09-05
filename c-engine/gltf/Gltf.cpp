#include "gltf/Gltf.h"

#include "Utils.h"
#include "gltf/GltfInternal.h"
#include "logger/Logger.h"
#include "renderer/Renderer.h"

#include <cmath>

namespace engine::gltf {

bool gltfInit(void) {
    return gltfInitDiligent();
}

bool gltfLoad(const char* pakPath) {
    return gltfLoadDiligent(pakPath);
}

bool gltfPlaceAt(double x, double y, double z) {
    return gltfPlaceAtDiligent(x, y, z);
}

bool gltfPlaceAtFacing(double x, double y, double z, f32 yaw) {
    return gltfPlaceAtFacingDiligent(x, y, z, yaw);
}

void gltfUpdate(double elapsedSeconds) {
    gltfUpdateDiligent(elapsedSeconds);
}

bool gltfBoundingBox(f32 min[3], f32 max[3]) {
    return gltfBoundingBoxDiligent(min, max);
}

bool gltfLocalBoundingBox(f32 min[3], f32 max[3]) {
    return gltfLocalBoundingBoxDiligent(min, max);
}

bool gltfLoadAnimations(const char* pakPath) {
    return gltfLoadAnimationsDiligent(pakPath);
}

u32 gltfAnimationCount(void) {
    return gltfAnimationCountDiligent();
}

const char* gltfAnimationName(u32 index) {
    return gltfAnimationNameDiligent(index);
}

f32 gltfAnimationDuration(u32 index) {
    return gltfAnimationDurationDiligent(index);
}

bool gltfPlayAnimation(const char* name, f32 speed, bool loop) {
    return gltfPlayAnimationDiligent(name, speed, loop);
}

bool gltfPlayAnimationBlended(const char* name, f32 speed, bool loop, f32 blendSeconds) {
    return gltfPlayAnimationBlendedDiligent(name, speed, loop, blendSeconds);
}

void gltfStopAnimation(void) {
    gltfStopAnimationDiligent();
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
    gltfDestroyDiligent();
}

}  // namespace engine::gltf
