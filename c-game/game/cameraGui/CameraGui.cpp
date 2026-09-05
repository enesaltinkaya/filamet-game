#include "CameraGui.h"
#include "Utils.h"
#include "renderer/Renderer.h"

#include "crmlui.h"

#include <cmath>
#include <cstdio>

namespace game {

CameraGui cameraGui;

CameraGui::CameraGui() : engine::System("cameraGui") {}

static void* document = nullptr;
static void* model    = nullptr;
static float posX, posY, posZ;
static float rotX, rotY, rotZ, rotW;

// The renderer only exposes the camera as lookAt state (position + forward
// vector), so rebuild the look rotation the same way the backends do: a
// right-handed basis from forward x world-up, converted to a quaternion.
// This reproduces the old engine's readout, which bound the camera entity's
// world-transform quaternion (x, y, z, w) directly.
static void lookQuaternion(const float f[3], float q[4]) {
    float up[3] = {0.0f, 1.0f, 0.0f};
    float d     = f[0] * up[0] + f[1] * up[1] + f[2] * up[2];
    // looking straight up/down degenerates the basis; fall back to the -Z up
    // the topdown ENGINE_CAMERA vantage itself uses
    if (d > 0.9999f || d < -0.9999f) {
        up[0] = 0.0f;
        up[1] = 0.0f;
        up[2] = -1.0f;
    }

    auto cross = [](const float a[3], const float b[3], float out[3]) {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    };

    float z[3] = {-f[0], -f[1], -f[2]};  // the camera looks along its local -Z
    float x[3];
    cross(f, up, x);
    float xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (xl > 0.0f) {
        x[0] /= xl;
        x[1] /= xl;
        x[2] /= xl;
    }
    float y[3];
    cross(z, x, y);

    // rotation matrix with columns x, y, z -> quaternion (Shepperd's method)
    const float m00 = x[0], m10 = x[1], m20 = x[2];
    const float m01 = y[0], m11 = y[1], m21 = y[2];
    const float m02 = z[0], m12 = z[1], m22 = z[2];
    const float tr  = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m21 - m12) / s;
        q[1] = (m02 - m20) / s;
        q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (m21 - m12) / s;
        q[0] = 0.25f * s;
        q[1] = (m01 + m10) / s;
        q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (m02 - m20) / s;
        q[0] = (m01 + m10) / s;
        q[1] = 0.25f * s;
        q[2] = (m12 + m21) / s;
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (m10 - m01) / s;
        q[0] = (m02 + m20) / s;
        q[1] = (m12 + m21) / s;
        q[2] = 0.25f * s;
    }
}

void CameraGui::added() {
    document = rmlNewDocument("gui/camera/camera.html");
    model    = rmlCreateModel("camera");
    rmlBindFloat(model, "posX", &posX);
    rmlBindFloat(model, "posY", &posY);
    rmlBindFloat(model, "posZ", &posZ);
    rmlBindFloat(model, "rotX", &rotX);
    rmlBindFloat(model, "rotY", &rotY);
    rmlBindFloat(model, "rotZ", &rotZ);
    rmlBindFloat(model, "rotW", &rotW);

    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void CameraGui::removed() {
    if (document) {
        rmlUnloadDocument(document);
        document = nullptr;
    }
    if (model) {
        rmlUnloadModel(model);
        model = nullptr;
    }
}

void CameraGui::update() {
    static double lastShown;
    double now = utils::millies();
    if (now <= lastShown + 50) return;  // 50ms, like the old gui

    f32 pos[3], fwd[3];
    engine::renderer::rendererCameraGet(pos, fwd);
    posX = pos[0];
    posY = pos[1];
    posZ = pos[2];

    float q[4];
    lookQuaternion(fwd, q);
    rotX = q[0];
    rotY = q[1];
    rotZ = q[2];
    rotW = q[3];

    lastShown = now;
    rmlUpdateDirtyAll(model);
}

}  // namespace game
