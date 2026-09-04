#include "Player.h"
#include "Utils.h"
#include "renderer/Renderer.h"
#include "renderer/Window.h"
#include "ecs/system/flyingCamera/FlyingCamera.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "gltf/Gltf.h"

#include <SDL.h>

#include <cmath>

namespace engine {

PlayerSystem playerSystem;

PlayerSystem::PlayerSystem() : System("Player") {}

// ── Character + physics constants (old engine Player.cpp / jolt_c_api) ──────
static const float CAPSULE_HALF    = 0.70f;  // feet → centre (old: 0.45 + 0.25)
static const float RUN_SPEED      = 4.0f;   // m/s
static const float WALK_SPEED     = 2.0f;   // shift-held
static const float JUMP_SPEED     = 4.0f;   // vertical impulse
static const float GRAVITY        = -9.81f; // Jolt's default world gravity
static const float MAX_FALL       = 40.0f;  // terminal velocity
static const float MAX_SLOPE_TAN  = 1.0f;   // 45° climb limit (old joltCharacterCreate)
static const float STEP_HEIGHT    = 0.25f;  // Jolt mWalkStairsStepUp / mStickToFloorStepDown
static const float GROUND_EPSILON = 0.02f;

// ── Third-person orbit camera (old engine's orbit-mode ranges) ──────────────
static const float CAM_SENS       = 0.002f; // rad/px (same as the flying camera)
static const float PITCH_MIN      = -20.0f * (float)M_PI / 180.0f;
static const float PITCH_MAX      = 60.0f * (float)M_PI / 180.0f;
static const float DIST_MIN       = 1.5f;
static const float DIST_MAX       = 20.0f;
static const float DIST_DEFAULT   = 10.0f;

// ENGINE_AUTO_RUN=1: auto-run forward from spawn (the old engine's temporal
// test hook — the third-person camera follows, producing real running motion
// for automated runs). Any W/S key cancels it, like the old engine.
static char autoRunEnabled(void) {
    static char v    = 0;
    static char init = 0;
    if (!init) {
        init = 1;
        const char* env = getenv("ENGINE_AUTO_RUN");
        if (env && *env && atoi(env)) v = 1;
    }
    return v;
}

static struct {
    f32 pos[3];  // capsule centre, world metres
    f32 vy       = 0.0f;
    f32 camYaw   = 0.0f;
    f32 camPitch = 20.0f * (float)M_PI / 180.0f;
    f32 camDist  = DIST_DEFAULT;
    f32 spawn[3] = {0.0f, 0.0f, 0.0f};
    char active  = 0;
    char spawned = 0;
    char autoRun = 0;
} p = {};

void playerSetSpawn(f32 x, f32 y, f32 z) {
    p.spawn[0]   = x;
    p.spawn[1]   = y;
    p.spawn[2]   = z;
    p.spawned    = 0;
}

char playerMode(void) { return p.active; }

// Orbit angles of the current renderer camera, in this system's convention
// (pitch > 0 = camera above the target, looking down). Same yaw math as
// FlyingCamera::syncStateFromCamera; the pitch sign is flipped because there
// pitch > 0 is the camera looking UP.
static void syncOrbitFromCamera(f32* outYaw, f32* outPitch) {
    f32 pos[3];
    f32 f[3];
    renderer::rendererCameraGet(pos, f);
    *outYaw   = atan2f(-f[0], -f[2]);
    *outPitch = -asin(f[1]);
}

static void playerSetActive(char on) {
    if (p.active == on) return;
    p.active = on;
    if (on) {
        windowSetRelativeMouseMode(1);
        syncOrbitFromCamera(&p.camYaw, &p.camPitch);
        utils::info("player: mode on — WASD/SPACE move, mouse orbit, wheel zoom (C off, F fly)");
    } else {
        windowSetRelativeMouseMode(0);
        utils::info("player: mode off (C on, F fly)");
    }
}

static void playerSpawn(void) {
    HeightmapTerrain* ht = heightmapTerrainGetActive();
    f32 groundY           = ht ? heightmapTerrainSample(ht, p.spawn[0], p.spawn[2]) : p.spawn[1];
    p.pos[0]               = p.spawn[0];
    p.pos[1]               = groundY + CAPSULE_HALF;
    p.pos[2]               = p.spawn[2];
    p.vy                   = 0.0f;
    p.camDist              = DIST_DEFAULT;
    p.spawned               = 1;
    gltf::gltfPlaceAt(p.pos[0], p.pos[1] - CAPSULE_HALF, p.pos[2]);
    utils::info("player: spawned at (%.1f, %.1f, %.1f), ground %.1f m",
                p.pos[0], p.pos[1], p.pos[2], groundY);
}

void PlayerSystem::added() {
    playerSpawn();
    // Automated runs (screenshot / dolly / renderdoc) keep their scripted
    // camera: the player exists (model at spawn) but the mode stays off so
    // WASD and the orbit camera never fight ENGINE_CAMERA/DOLLY framing.
    // ENGINE_AUTO_RUN is the exception — it IS the camera-follow test.
    char automated = (getenv("ENGINE_SCREENSHOT") != nullptr) ||
                     (getenv("ENGINE_CAMERA_DOLLY") != nullptr) ||
                     (getenv("ENGINE_RENDERDOC_CAPTURE") != nullptr) ||
                     (getenv("ENGINE_NO_PLAYER") != nullptr);
    p.autoRun = autoRunEnabled();
    playerSetActive(!automated || p.autoRun);
}

void PlayerSystem::removed() {
    playerSetActive(0);
    p.spawned = 0;
}

void PlayerSystem::preUpdate() {
    // F started a fly this frame (the fly system runs first and already
    // captured the mouse): yield without touching the mouse mode.
    if (p.active && flyingCameraFlying()) {
        p.active = 0;
        return;
    }

    if (input.pressed == SDL_SCANCODE_C) {
        if (!p.active) {
            if (flyingCameraFlying()) {
                // Take over from a fly: park the player where the camera was
                // (the old engine's playerFollowFlyingCamera) — the first
                // update ground-snaps it.
                f32 pos[3];
                f32 f[3];
                renderer::rendererCameraGet(pos, f);
                p.pos[0] = pos[0];
                p.pos[1] = pos[1] - 3.0f;
                p.pos[2] = pos[2];
                p.vy     = 0.0f;
                flyingCameraStop();
            }
            playerSetActive(1);
        } else {
            playerSetActive(0);
        }
    }
}

// Camera-relative movement basis: forward = away from the camera (W), right =
// screen right (D). Derives from the orbit yaw only — the camera sits at
// pos + dist·(sin yaw·cos pitch, sin pitch, cos yaw·cos pitch), so the
// camera→player horizontal direction is −(sin yaw, 0, cos yaw).
static void movementInput(f32* outHx, f32* outHz) {
    char f = (input.keys[SDL_SCANCODE_W] ? 1 : 0) - (input.keys[SDL_SCANCODE_S] ? 1 : 0);
    char r = (input.keys[SDL_SCANCODE_D] ? 1 : 0) - (input.keys[SDL_SCANCODE_A] ? 1 : 0);
    // Auto-run: always run forward (W); a manual W/S press cancels it
    // (the old engine's rule — A/D strafing does not cancel).
    if (p.autoRun) {
        if (f) p.autoRun = 0;
        f = 1;
    }
    if (!f && !r) {
        *outHx = *outHz = 0.0f;
        return;
    }
    f32 sy = sinf(p.camYaw);
    f32 cy = cosf(p.camYaw);
    f32 fx = -sy, fz = -cy;  // forward (W)
    f32 rx =  cy, rz = -sy;  // right (D)
    f32 hx = fx * f + rx * r;
    f32 hz = fz * f + rz * r;
    f32 len = sqrtf(hx * hx + hz * hz);
    f32 speed = input.shift ? WALK_SPEED : RUN_SPEED;
    *outHx    = hx / len * speed;
    *outHz    = hz / len * speed;
}

static void playerUpdateCamera(void) {
    if (input.mouseDx != 0.0f || input.mouseDy != 0.0f) {
        p.camYaw -= input.mouseDx * CAM_SENS;
        p.camPitch += input.mouseDy * CAM_SENS;  // mouse up lowers the camera (looks up), like the old engine
        if (p.camPitch < PITCH_MIN) p.camPitch = PITCH_MIN;
        if (p.camPitch > PITCH_MAX) p.camPitch = PITCH_MAX;
    }
    if (input.scrollY != 0.0f) {
        p.camDist -= input.scrollY * 2.0f;
        if (p.camDist < DIST_MIN) p.camDist = DIST_MIN;
        if (p.camDist > DIST_MAX) p.camDist = DIST_MAX;
    }

    f32 sy  = sinf(p.camYaw);
    f32 cp  = cosf(p.camPitch);
    f32 sp  = sinf(p.camPitch);
    f32 eye[3] = {
        p.pos[0] + sy * cp * p.camDist,
        p.pos[1] + sp * p.camDist,
        p.pos[2] + cosf(p.camYaw) * cp * p.camDist,
    };
    f32 up[3] = {0.0f, 1.0f, 0.0f};
    renderer::rendererCameraLookAt(eye, p.pos, up);
}

void PlayerSystem::update() {
    if (!p.spawned) return;
    HeightmapTerrain* ht = heightmapTerrainGetActive();
    if (!ht) return;
    float dt = utils::timer.dt;

    // ── Desired horizontal velocity (old engine: instant, full air control) ──
    f32 hx = 0.0f, hz = 0.0f;
    if (p.active) movementInput(&hx, &hz);

    // ── Vertical (mirrors joltCharacterUpdate: grounded → jump impulse or
    //    flat, airborne → accumulate gravity) ─────────────────────────────
    f32 groundY  = heightmapTerrainSample(ht, p.pos[0], p.pos[2]);
    f32 feet     = p.pos[1] - CAPSULE_HALF;
    char onGround = feet <= groundY + GROUND_EPSILON;
    if (onGround) {
        p.vy = (p.active && input.keys[SDL_SCANCODE_SPACE]) ? JUMP_SPEED : 0.0f;
    } else {
        p.vy += GRAVITY * dt;
        if (p.vy < -MAX_FALL) p.vy = -MAX_FALL;
    }

    // ── Integrate ─────────────────────────────────────────────────────────
    f32 nx   = p.pos[0] + hx * dt;
    f32 ny   = p.pos[1] + p.vy * dt;
    f32 nz   = p.pos[2] + hz * dt;
    f32 newFeet = ny - CAPSULE_HALF;
    f32 oldFeet = p.pos[1] - CAPSULE_HALF;
    f32 groundAt = heightmapTerrainSample(ht, nx, nz);

    if (newFeet <= groundAt) {
        // Ground at/above the feet. Beyond a step, steeper than the climb
        // limit is a wall: keep the old XZ (the vertical move still applies).
        f32 rise = groundAt - oldFeet;
        if (rise > STEP_HEIGHT) {
            f32 dx    = nx - p.pos[0];
            f32 dz    = nz - p.pos[2];
            f32 horiz = sqrtf(dx * dx + dz * dz);
            if (horiz > 1e-4f && rise / horiz > MAX_SLOPE_TAN) {
                nx       = p.pos[0];
                nz       = p.pos[2];
                groundAt = heightmapTerrainSample(ht, nx, nz);
            }
        }
        p.pos[0] = nx;
        p.pos[1] = groundAt + CAPSULE_HALF;  // snap feet onto the surface
        p.pos[2] = nz;
        if (p.vy < 0.0f) p.vy = 0.0f;        // landing
    } else if (oldFeet > groundAt && oldFeet - groundAt <= STEP_HEIGHT && p.vy <= 0.0f) {
        // Walked off a small ledge: stick to the floor (Jolt's
        // mStickToFloorStepDown) instead of falling one frame at a time.
        p.pos[0] = nx;
        p.pos[1] = groundAt + CAPSULE_HALF;
        p.pos[2] = nz;
    } else {
        // Airborne.
        p.pos[0] = nx;
        p.pos[1] = ny;
        p.pos[2] = nz;
    }

    gltf::gltfPlaceAt(p.pos[0], p.pos[1] - CAPSULE_HALF, p.pos[2]);
    if (p.active) playerUpdateCamera();
}
}  // namespace engine
