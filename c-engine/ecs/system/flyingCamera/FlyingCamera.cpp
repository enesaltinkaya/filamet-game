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
    double x, y, z;  // double: the camera is the world anchor (see Player.cpp)

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
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

// Camera DB (persist the free camera's location + rotation across runs —
// same pattern as the player DB; the orbit camera is already persisted via
// the player system, this covers the fly/free view).
struct CameraDb {
    float pos[3];
    float yaw;
    float pitch;
};

static void cameraDbInit(void) {
    if (!utils::sqliteTableExists("camera")) {
        utils::sqliteExecute(
            "CREATE TABLE IF NOT EXISTS camera ("
            "name TEXT PRIMARY KEY, "
            "data BLOB);");
    }
}

static void cameraDbSave(const char* name, CameraDb* data) {
    void* stmt = utils::sqliteStatement("REPLACE INTO camera (name, data) VALUES (?, ?);");
    utils::sqliteBindText(stmt, 1, name);
    utils::sqliteBindBlob(stmt, 2, data, sizeof(CameraDb));
    utils::sqliteStep(stmt);
    utils::sqliteFinalize(stmt);
}

static bool cameraDbLoad(const char* name, CameraDb* data) {
    void* stmt = utils::sqliteStatement("SELECT data, length(data) FROM camera WHERE name = ?;");
    bool result = false;
    utils::sqliteBindText(stmt, 1, name);
    if (utils::sqliteStep(stmt)) {
        void* blob   = utils::sqliteGetBlob(stmt, 0);
        int blobSize = utils::sqliteGetInt(stmt, 1);
        memcpy(data, blob, std::min(static_cast<size_t>(blobSize), sizeof(CameraDb)));
        result = true;
    }
    utils::sqliteFinalize(stmt);
    return result;
}

// camera looks along local -Z
static Vec3 forwardDir(void) {
    float cp = std::cos(pitch);
    return {-cp * std::sin(yaw), std::sin(pitch), -cp * std::cos(yaw)};
}

bool flyingCameraSavedView(f32* outPos, f32* outYaw, f32* outPitch) {
    CameraDb data = {};
    // A missing table would terminate in sqliteStatement — guard it (the
    // fly system's added() creates the table, the player may read it first).
    if (!utils::sqliteTableExists("camera")) return false;
    if (!cameraDbLoad("camera", &data)) return false;
    outPos[0]   = data.pos[0];
    outPos[1]   = data.pos[1];
    outPos[2]   = data.pos[2];
    *outYaw     = data.yaw;
    *outPitch   = data.pitch;
    return true;
}

void flyingCameraSaveView(const f32 pos[3], f32 yaw, f32 pitch) {
    CameraDb data = {
        .pos   = {pos[0], pos[1], pos[2]},
        .yaw   = yaw,
        .pitch = pitch,
    };
    cameraDbSave("camera", &data);
}

static Vec3 rightDir(void) {
    return {std::cos(yaw), 0.0f, -std::sin(yaw)};
}

static void applyCamera(void) {
    // keep world up; pitch is clamped below 88 degrees so (0,1,0) stays valid
    Vec3 fwd = forwardDir();
    double eye[3] = {pos.x, pos.y, pos.z};
    double center[3] = {pos.x + fwd.x * 1000.0, pos.y + fwd.y * 1000.0, pos.z + fwd.z * 1000.0};
    const double up[3] = {0.0, 1.0, 0.0};
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

    // Restore the last saved free-camera view; R (reset) then returns to
    // this same state rather than the scripted startup camera.
    cameraDbInit();
    CameraDb saved = {};
    if (cameraDbLoad("camera", &saved)) {
        pos   = {saved.pos[0], saved.pos[1], saved.pos[2]};
        yaw   = saved.yaw;
        pitch = saved.pitch;
        originalPos   = pos;
        originalYaw   = yaw;
        originalPitch = pitch;
        applyCamera();
        utils::info("flying camera: loaded saved state pos (%.1f, %.1f, %.1f) yaw %.0f\u00b0 pitch %.0f\u00b0",
                    pos.x, pos.y, pos.z, yaw * 180.0f / (float)M_PI, pitch * 180.0f / (float)M_PI);
    }
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
    // Consume the accumulated look delta (windowPollEvents adds across
    // rendered frames; this tick zeroes it — see the note there).
    input.mouseDx = 0.0f;
    input.mouseDy = 0.0f;

    float speedMult = input.shift ? 10.0f : (input.ctrl ? 0.25f : 1.0f);
    double step     = (double)(flySpeed * speedMult) * utils::timer.dt;

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

    // Periodic save like the player's (automated runs never fly, so a
    // scripted camera — dolly, framing — is never persisted).
    static double lastSave = 0.0;
    const double now = utils::millies();
    if (now > lastSave + 1000.0) {
        lastSave = now;
        CameraDb data = {
            .pos   = {(f32)pos.x, (f32)pos.y, (f32)pos.z},
            .yaw   = yaw,
            .pitch = pitch,
        };
        cameraDbSave("camera", &data);
    }
}

FlyingCameraSystem flyingCameraSystem;

bool flyingCameraFlying(void) { return flying != 0; }

void flyingCameraStop(void) { setFlying(0); }
}  // namespace engine
