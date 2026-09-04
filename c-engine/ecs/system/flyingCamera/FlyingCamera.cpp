#include "FlyingCamera.h"
#include "Utils.h"
#include "renderer/Renderer.h"
#include "renderer/Window.h"

#include <SDL.h>

#include <cmath>

namespace engine {

FlyingCameraSystem::FlyingCameraSystem() : System("FlyingCamera") {}

static char flying;
static float flySpeed    = 100.0f;  // units/second
static float sensitivity = 0.002f;  // radians per pixel

struct Vec3 {
    float x, y, z;

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3& operator+=(const Vec3& o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    Vec3& operator-=(const Vec3& o) {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }
};

static float yaw;
static float pitch;
static Vec3 pos;

static float originalYaw;
static float originalPitch;
static Vec3 originalPos;

// camera looks along local -Z
static Vec3 forwardDir(void) {
    float cp = std::cos(pitch);
    return {-cp * std::sin(yaw), std::sin(pitch), -cp * std::cos(yaw)};
}

static Vec3 rightDir(void) {
    return {std::cos(yaw), 0.0f, -std::sin(yaw)};
}

static void applyCamera(void) {
    // keep world up; pitch is clamped below 88 degrees so (0,1,0) stays valid
    Vec3 fwd = forwardDir();
    f32 eye[3] = {pos.x, pos.y, pos.z};
    f32 center[3] = {pos.x + fwd.x * 1000.0f, pos.y + fwd.y * 1000.0f, pos.z + fwd.z * 1000.0f};
    const f32 up[3] = {0.0f, 1.0f, 0.0f};
    renderer::rendererCameraLookAt(eye, center, up);
}

static void syncStateFromCamera(void) {
    f32 p[3];
    f32 f[3];
    renderer::rendererCameraGet(p, f);
    pos = {p[0], p[1], p[2]};
    yaw   = std::atan2f(-f[0], -f[2]);
    pitch = std::asin(f[1]);
}

static void setFlying(char on) {
    if (flying == on) return;
    flying = on;
    if (on) {
        syncStateFromCamera();
        windowSetRelativeMouseMode(1);
        // drop any delta accumulated before the mode switch, and the poll
        // of this frame which already ran
        float dx, dy;
        SDL_GetRelativeMouseState(&dx, &dy);
        input.mouseDx = 0.0f;
        input.mouseDy = 0.0f;
        utils::info("flying camera: on (F to re-enter, ESC to exit)");
    } else {
        windowSetRelativeMouseMode(0);
        utils::info("flying camera: off");
    }
}

void FlyingCameraSystem::added() {
    syncStateFromCamera();
    originalYaw   = yaw;
    originalPitch = pitch;
    originalPos   = pos;
}

void FlyingCameraSystem::removed() {
    if (flying) windowSetRelativeMouseMode(0);
}

void FlyingCameraSystem::preUpdate() {
    if (input.pressed == SDL_SCANCODE_F) {
        setFlying(flying ? 0 : 1);
    }
    if (flying && input.pressed == SDL_SCANCODE_ESCAPE) {
        setFlying(0);
    }
    if (flying && input.pressed == SDL_SCANCODE_R) {
        yaw   = originalYaw;
        pitch = originalPitch;
        pos   = originalPos;
        applyCamera();
    }
}

void FlyingCameraSystem::update() {
    if (!flying) return;

    if (input.mouseDx != 0.0f || input.mouseDy != 0.0f) {
        yaw   -= input.mouseDx * sensitivity;
        pitch -= input.mouseDy * sensitivity;
        const float pitchLimit = 88.0f * (float)M_PI / 180.0f;
        pitch = pitch > pitchLimit ? pitchLimit : pitch < -pitchLimit ? -pitchLimit : pitch;
    }

    float speedMult = input.shift ? 10.0f : (input.ctrl ? 0.25f : 1.0f);
    float step      = flySpeed * speedMult * utils::timer.dt;

    Vec3 fwd   = forwardDir();
    Vec3 right = rightDir();

    Vec3 move = {0.0f, 0.0f, 0.0f};
    if (input.keys[SDL_SCANCODE_W]) move += fwd * step;
    if (input.keys[SDL_SCANCODE_S]) move -= fwd * step;
    if (input.keys[SDL_SCANCODE_D]) move += right * step;
    if (input.keys[SDL_SCANCODE_A]) move -= right * step;
    if (input.keys[SDL_SCANCODE_SPACE]) move += Vec3{0.0f, step, 0.0f};
    if (input.keys[SDL_SCANCODE_X])     move -= Vec3{0.0f, step, 0.0f};
    if (input.scrollY) move += fwd * (input.scrollY * step * 2.0f);

    if (move.x != 0.0f || move.y != 0.0f || move.z != 0.0f) {
        pos += move;
    }
    applyCamera();
}

FlyingCameraSystem flyingCameraSystem;

bool flyingCameraFlying(void) { return flying != 0; }

void flyingCameraStop(void) { setFlying(0); }
}  // namespace engine
