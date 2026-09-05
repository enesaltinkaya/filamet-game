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
static const float CAPSULE_HALF_HEIGHT = 0.45f; // half of the cylindrical part
static const float CAPSULE_RADIUS      = 0.25f; // capsule radius
static const float TP_LOOK_AT_HEIGHT   = 1.1f; // camera look-at above feet (old tpLookAtHeight)
static const float MAX_SLOPE_ANGLE     = 45.0f * (float)M_PI / 180.0f; // old joltCharacterCreate
static const float RUN_SPEED           = 4.0f; // m/s
static const float WALK_SPEED          = 2.0f; // shift-held
static const float JUMP_SPEED          = 4.0f; // vertical jump velocity
static const float SPRINT_MULT         = 40.0f; // alt-sprint (old MOVE_SPEED_SPRINT_MULT)

// ── Third-person orbit camera (old engine's ThirdPersonCamera.cpp) ───────────
// rad per pixel. Old engine: `yaw += dx * 0.15 * dt` where dt is the fixed
// 1/60 sim tick — inlined as 0.15/60 (utils::timer.dt is constant, so do not
// multiply by it again). dx accumulates across rendered frames and is zeroed
// by the consuming tick (see windowPollEvents), which is what keeps the look
// frame-rate independent.
static const float CAM_SENS  = 0.15f / 60.0f;
static const float PITCH_MIN = -20.0f * (float)M_PI / 180.0f;
static const float PITCH_MAX = 60.0f * (float)M_PI / 180.0f;
static const float DIST_MIN  = 1.5f;
static const float DIST_MAX  = 20.0f;
static const float DIST_DEFAULT = 10.0f;
static const float CAM_RADIUS = 0.5f; // obstacle-clamp sphere radius (old cameraRadius)

// ── Animation (old engine Player.cpp ANIM_* block; clips from the animation
// source asset — Game.cpp loads models/animations.zstd and starts eve_idle1) ──
static const char* ANIM_IDLE  = "eve_idle1";
static const char* ANIM_RUN   = "eve_run1";
static const char* ANIM_WALK  = "female_walk";
static const char* ANIM_JUMP  = "eve_jump";
static const char* ANIM_TPOSE = "eve_t";
static const f32 ANIM_SPEED_IDLE  = 1.0f;
static const f32 ANIM_SPEED_RUN   = 1.75f;
static const f32 ANIM_SPEED_WALK  = 1.5f;
static const f32 ANIM_SPEED_JUMP  = 1.0f;
static const f32 ANIM_SPEED_TPOSE = 1.0f;
static const f32 ANIM_BLEND = 0.2f;
static const f32 TURN_SPEED = 20.0f; // old engine's MOVE_SPEED_TURN

// ENGINE_TPOSE=1: always play the T-pose (old engine's inspect hook).
static char engineTpose(void) {
    static char v    = 0;
    static char init = 0;
    if (!init) {
        init = 1;
        const char* env = getenv("ENGINE_TPOSE");
        if (env && *env && atoi(env)) v = 1;
    }
    return v;
}

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

// Screenshot / dolly / renderdoc / no-player runs keep the player parked:
// the scripted camera owns the view, so a fly-end must not auto-activate the
// player over it (same gate as added(); ENGINE_AUTO_RUN is the exception —
// it IS the camera-follow test).
static char automatedRun(void) {
    return (getenv("ENGINE_SCREENSHOT") != nullptr) ||
           (getenv("ENGINE_CAMERA_DOLLY") != nullptr) ||
           (getenv("ENGINE_RENDERDOC_CAPTURE") != nullptr) ||
           (getenv("ENGINE_NO_PLAYER") != nullptr);
}

static struct {
    // Feet position, world metres. DOUBLE: Jolt is double internally and the
    // renderer anchors on the (double) camera eye — an f32 position at 39 km
    // sits on the 3.9 mm f32 grid and the character shimmers (lessons.md).
    double pos[3];
    // Feet at the previous fixed tick — for the tick-rate speed difference
    // (playerGetFootSpeed). A difference over the rendered frame dt aliases
    // against the fixed tick rate at >UPS fps (the unlimited-fps grass shake).
    double prevPos[3];
    double footSpeed;  // horizontal speed (m/s) as of the last fixed tick
    JoltCharacter* character = nullptr;
    char waitingForGround    = 0; // pinned at spawn until the body under it exists
    f32 camYaw   = 0.0f;
    f32 camPitch = 8.0f * (float)M_PI / 180.0f; // old tpCam.init pitch
    f32 camDist  = DIST_DEFAULT;
    f32 spawn[3] = {0.0f, 0.0f, 0.0f};
    f32 moveYaw    = 0.0f; // movement basis (old engine's moveYaw — stable during LMB drags)
    f32 faceTarget = 0.0f; // facing TARGET (old facingYaw — set instantly each frame)
    f32 modelYaw   = 0.0f; // smoothed model yaw (old transform->rot — slerps to faceTarget)

    // Orbit-camera extras (old ThirdPersonCamera's smoothDist + skyPitchOffset)
    f32 tpSmoothDist   = -1.0f;
    f32 skyPitchOffset = 0.0f;
    char animMoving  = 0; // old engine's isMoving
    char animJumping = 0; // old engine's isJumping
    char animTposing = 0; // T-key emote active until movement resumes
    char active  = 0;
    char spawned = 0;
    char autoRun = 0;
    char dragging = 0; // LMB/RMB drag in flight — relative mouse mode is ours
    char prevFlying  = 0; // fly state last frame (off-edge hands control back)
    char canTakeover = 1; // 0 for automated runs — never auto-activate
} p = {};

void playerSetSpawn(f32 x, f32 y, f32 z) {
    p.spawn[0]   = x;
    p.spawn[1]   = y;
    p.spawn[2]   = z;
    p.spawned    = 0;
}

char playerMode(void) { return p.active; }

bool playerGetFootPos(double out[3]) {
    if (!p.spawned || !p.character) return false;
    out[0] = p.pos[0];
    out[1] = p.pos[1];
    out[2] = p.pos[2];
    return true;
}

// Advance the tick-rate speed difference with this tick's (already-updated)
// p.pos and record it as the next tick's baseline. Called at every exit of
// update(): the difference runs over the FIXED timer.dt (1/UPS), which is
// what keeps playerGetFootSpeed smooth at any rendered fps (a rendered-frame
// difference aliases against the tick rate at >UPS fps).
static void playerTickFootSpeed(void) {
    const double dx = p.pos[0] - p.prevPos[0];
    const double dz = p.pos[2] - p.prevPos[2];
    p.footSpeed = std::hypot(dx, dz) / (double)utils::timer.dt;
    p.prevPos[0] = p.pos[0];
    p.prevPos[1] = p.pos[1];
    p.prevPos[2] = p.pos[2];
}

double playerGetFootSpeed(void) {
    if (!p.spawned || !p.character) return 0.0;
    return p.footSpeed;
}

char playerTeleportTo(f32 x, f32 y, f32 z) {
    if (!p.spawned || !p.character) return 0;
    p.pos[0] = x;
    p.pos[1] = y;
    p.pos[2] = z;
    joltCharacterSetPositionD64(p.character, p.pos);
    p.autoRun = 0;  // old engine: a teleport cancels auto-run
    return 1;
}

// ── Player DB (persist position + orbit camera state across runs —
// port of the old engine's Player.cpp PlayerDb; the old engine split this
// into transformDb("player") + playerDb("player"), we use one blob) ──────
struct PlayerDb {
    f32 pos[3];
    f32 modelYaw; // the old engine persisted the facing — here it is the smoothed yaw
    f32 camYaw;
    f32 camPitch;
    f32 camDist;
    f32 moveYaw;  // appended last — old blobs lack it (fall back to modelYaw)
};

static void playerDbInit(void) {
    if (!utils::sqliteTableExists("player")) {
        utils::sqliteExecute(
            "CREATE TABLE IF NOT EXISTS player ("
            "name TEXT PRIMARY KEY, "
            "data BLOB);");
    }
}

static void playerDbSave(const char* name, PlayerDb* data) {
    void* stmt = utils::sqliteStatement("REPLACE INTO player (name, data) VALUES (?, ?);");
    utils::sqliteBindText(stmt, 1, name);
    utils::sqliteBindBlob(stmt, 2, data, sizeof(PlayerDb));
    utils::sqliteStep(stmt);
    utils::sqliteFinalize(stmt);
}

static bool playerDbLoad(const char* name, PlayerDb* data, int* outBlobSize) {
    void* stmt = utils::sqliteStatement("SELECT data, length(data) FROM player WHERE name = ?;");
    bool result = false;
    utils::sqliteBindText(stmt, 1, name);
    if (utils::sqliteStep(stmt)) {
        void* blob   = utils::sqliteGetBlob(stmt, 0);
        int blobSize = utils::sqliteGetInt(stmt, 1);
        *outBlobSize = blobSize;
        memcpy(data, blob, std::min(static_cast<size_t>(blobSize), sizeof(PlayerDb)));
        result = true;
    }
    utils::sqliteFinalize(stmt);
    return result;
}

static void playerDbSaveState(void) {
    PlayerDb data = {
        .pos       = {(f32)p.pos[0], (f32)p.pos[1], (f32)p.pos[2]},
        .modelYaw  = p.modelYaw,
        .camYaw    = p.camYaw,
        .camPitch  = p.camPitch,
        .camDist   = p.camDist,
        .moveYaw   = p.moveYaw,
    };
    playerDbSave("player", &data);
}

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
        p.dragging = 0;
        syncOrbitFromCamera(&p.camYaw, &p.camPitch);
        utils::info("player: mode on — WASD/SPACE move, LMB/RMB drag orbit, wheel zoom (C off, F fly)");
    } else {
        // Release the capture if a drag was in flight when the mode ended
        if (p.dragging) {
            p.dragging = 0;
            windowSetRelativeMouseMode(0);
        }
        utils::info("player: mode off (C on, F fly)");
    }
}

// Cursor show/hide on drag transitions (the old engine's behaviour —
// windowSystemHideCursor on press / ShowCursor on release): the cursor is
// shown normally while idle; pressing LMB or RMB enters relative mouse mode
// for the orbit drag, releasing restores the normal cursor. No button held,
// no camera rotation.
static void playerUpdateMouseMode(void) {
    char drag = input.mouseLeft || input.mouseRight;
    if (drag && !p.dragging) {
        p.dragging = 1;
        windowSetRelativeMouseMode(1);
        // drop the warp-to-center delta, like FlyingCamera::setFlying
        float dx, dy;
        SDL_GetRelativeMouseState(&dx, &dy);
        input.mouseDx = 0.0f;
        input.mouseDy = 0.0f;
    } else if (!drag && p.dragging) {
        p.dragging = 0;
        windowSetRelativeMouseMode(0);
    }
}

// Port of the old engine's playerFollowFlyingCamera: while the fly camera is
// active the player sticks to it — parked just ahead of and below the eye so
// the model stays in view, the Jolt body is teleported along (no rubber-band
// when control comes back), and the orbit angles track the fly camera so the
// handover is seamless. Runs after the fly system's update in the same frame.
static void playerFollowFlyingCamera(void) {
    f32 pos[3];
    f32 f[3];
    renderer::rendererCameraGet(pos, f);
    p.pos[0] = pos[0] + f[0] * 2.0f;
    p.pos[1] = pos[1] - 3.0f;
    p.pos[2] = pos[2] + f[2] * 2.0f;

    if (p.character) joltCharacterSetPositionD64(p.character, p.pos);

    // Keep the orbit angles in sync with the engine camera (old engine:
    // cameraYaw / cameraPitch / moveYaw / facingYaw ← cam; pitch clamped to
    // the orbit range). syncOrbitFromCamera already flips the pitch sign —
    // the orbit convention has pitch > 0 = camera above the target.
    syncOrbitFromCamera(&p.camYaw, &p.camPitch);
    if (p.camPitch < PITCH_MIN) p.camPitch = PITCH_MIN;
    if (p.camPitch > PITCH_MAX) p.camPitch = PITCH_MAX;
    p.moveYaw    = p.camYaw;
    p.faceTarget = p.moveYaw;
}

static void playerSpawn(void) {
    HeightmapTerrain* ht = heightmapTerrainGetActive();
    f32 groundY           = ht ? heightmapTerrainSample(ht, p.spawn[0], p.spawn[2]) : p.spawn[1];
    p.pos[0]               = p.spawn[0];
    p.pos[1]               = groundY;
    p.pos[2]               = p.spawn[2];
    p.camDist              = DIST_DEFAULT;
    p.moveYaw               = 0.0f;  // orbit convention: W runs away from the camera
    p.faceTarget            = 0.0f;
    p.modelYaw              = 0.0f;
    p.tpSmoothDist          = -1.0f;
    p.skyPitchOffset        = 0.0f;
    p.animMoving           = 0;
    p.animJumping          = 0;
    p.animTposing          = 0;
    p.spawned               = 1;
    p.waitingForGround      = 1;  // released by the first update once the body under the spawn exists

    // Load the last saved player + camera state (old engine's scene-load
    // transformDbLoad + playerDbLoad) — overwrites spawn position and the
    // orbit camera angles. The waitingForGround gate drops the character
    // onto the heightmap under the loaded position.
    playerDbInit();
    PlayerDb saved = {};
    int savedSize = 0;
    if (playerDbLoad("player", &saved, &savedSize)) {
        p.pos[0]   = saved.pos[0];
        p.pos[1]   = saved.pos[1];
        p.pos[2]   = saved.pos[2];
        p.faceTarget = saved.modelYaw;
        p.modelYaw   = saved.modelYaw;
        p.moveYaw   = (savedSize >= (int)sizeof(PlayerDb)) ? saved.moveYaw
                                                           : saved.modelYaw + (float)M_PI;
        p.camYaw    = saved.camYaw;
        p.camPitch  = saved.camPitch;
        p.camDist   = saved.camDist;
        utils::info("player: loaded saved state pos (%.1f, %.1f, %.1f) cam (%.0f°, %.0f°, %.1f m)",
                    p.pos[0], p.pos[1], p.pos[2], p.camYaw * 180.0f / (float)M_PI,
                    p.camPitch * 180.0f / (float)M_PI, p.camDist);
    }

    // The camera table is the source for the last camera view (fly or orbit).
    // If a saved view exists, derive the initial orbit from it: the orbit
    // starts at the saved eye, so third-person mode picks up exactly where the
    // last session's camera was (the player DB orbit above is the fallback
    // for first runs without a camera row).
    f32 savedEye[3];
    f32 savedCamYaw, savedCamPitch;
    if (flyingCameraSavedView(savedEye, &savedCamYaw, &savedCamPitch)) {
        f32 dx = savedEye[0] - p.pos[0];
        f32 dy = savedEye[1] - (p.pos[1] + TP_LOOK_AT_HEIGHT);
        f32 dz = savedEye[2] - p.pos[2];
        f32 d  = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > 0.05f) {
            p.camDist  = d;
            if (p.camDist < DIST_MIN) p.camDist = DIST_MIN;
            if (p.camDist > DIST_MAX) p.camDist = DIST_MAX;
            p.camPitch = asinf(dy / d);
            if (p.camPitch < PITCH_MIN) p.camPitch = PITCH_MIN;
            if (p.camPitch > PITCH_MAX) p.camPitch = PITCH_MAX;
            p.camYaw   = atan2f(dx, dz);
            utils::info("player: orbit restored from saved camera eye (%.1f, %.1f, %.1f) — dist %.1f m, pitch %.0f°",
                        savedEye[0], savedEye[1], savedEye[2], p.camDist,
                        p.camPitch * 180.0f / (float)M_PI);
        }
    }

    if (!p.character) {
        // The create API is f32 (one-shot: the spawn comes from f32 world data
        // anyway); from here on the position stays double.
        f32 cpos[3] = {(f32)p.pos[0], (f32)p.pos[1], (f32)p.pos[2]};
        p.character = joltCharacterCreate(CAPSULE_HALF_HEIGHT, CAPSULE_RADIUS, cpos, MAX_SLOPE_ANGLE);
        if (!p.character) utils::warn("player: Jolt character creation failed");
    }
    // Baseline for the tick-rate speed difference — after ALL p.pos writes
    // (the saved-state load above may have moved it).
    p.prevPos[0] = p.pos[0]; p.prevPos[1] = p.pos[1]; p.prevPos[2] = p.pos[2];
    p.footSpeed   = 0.0;
    gltf::gltfPlaceAt(p.pos[0], p.pos[1], p.pos[2]);
    utils::info("player: spawned at (%.1f, %.1f, %.1f), ground %.1f m",
                p.pos[0], p.pos[1], p.pos[2], groundY);
}

void PlayerSystem::added() {
    playerSpawn();
    // Automated runs (screenshot / dolly / renderdoc) keep their scripted
    // camera: the player exists (model at spawn) but the mode stays off so
    // WASD and the orbit camera never fight ENGINE_CAMERA/DOLLY framing.
    // ENGINE_AUTO_RUN is the exception — it IS the camera-follow test.
    char automated = automatedRun();
    p.autoRun = autoRunEnabled();
    p.canTakeover = !automated;
    playerSetActive(!automated || p.autoRun);
}

void PlayerSystem::removed() {
    playerSetActive(0);
    // ecsDestroy snapshots the system list, so removed() may run twice;
    // only destroy the character once.
    if (p.character) {
        joltCharacterDestroy(p.character);
        p.character = nullptr;
    }
    p.spawned = 0;
}

void PlayerSystem::preUpdate() {
    // Fly-end edge: disabling the fly handed control back to the player (old
    // engine behaviour — player mode is simply no longer suppressed). The
    // follow has been parking the player under the camera all along and the
    // orbit angles are already synced; re-engage the ground gate so
    // unstreamed terrain under the landing spot can't swallow the character.
    const char flying = flyingCameraFlying();
    if (p.prevFlying && !flying && !p.active && p.canTakeover) {
        p.waitingForGround = 1;
        p.tpSmoothDist     = -1.0f;
        p.skyPitchOffset   = 0.0f;
        playerSetActive(1);
    }
    p.prevFlying = flying;

    // F started a fly this frame (the fly system runs first and already
    // captured the mouse): yield without touching the mouse mode.
    if (p.active && flying) {
        p.active = 0;
        p.dragging = 0;  // fly owns the capture now
        return;
    }

    if (input.pressed == SDL_SCANCODE_C) {
        if (!p.active) {
            if (flying) {
                // Explicit takeover from a fly: the follow already parked the
                // player under the camera and synced the orbit — just stop
                // the fly (the first physics update drops the player onto
                // the ground).
                flyingCameraStop();
            }
            playerSetActive(1);
        } else {
            playerSetActive(0);
        }
    }

    // Middle mouse click: toggle auto-run (old engine: rising edge of the
    // middle button in the player movement input — runs W forward, any W/S
    // cancels it, and the periodic state save is suppressed while on).
    if (p.active && input.mousePressed == 2) {
        p.autoRun = !p.autoRun;
        utils::info("player: auto-run %s", p.autoRun ? "on" : "off");
    }

    if (p.active) playerUpdateMouseMode();
}

// Movement basis: forward = away from the camera (W), right = screen right
// (D). Derived from moveYaw, NOT camYaw — an LMB drag orbits the camera and
// must not change the player's direction (old engine: moveYaw is only synced
// to cameraYaw while RMB is held). moveYaw is in the camera convention
// (camera→player = −(sin, 0, cos) at that yaw), so the same math applies.
static void movementInput(f32* outHx, f32* outHz) {
    char f = (input.keys[SDL_SCANCODE_W] ? 1 : 0) - (input.keys[SDL_SCANCODE_S] ? 1 : 0);
    char r = (input.keys[SDL_SCANCODE_D] ? 1 : 0) - (input.keys[SDL_SCANCODE_A] ? 1 : 0);
    // Auto-run: always run forward (W); a manual W/S press cancels it and
    // keeps the pressed direction (the old engine's rule — A/D strafing does
    // not cancel).
    if (p.autoRun) {
        if (f) p.autoRun = 0;
        else f = 1;
    }
    if (!f && !r) {
        *outHx = *outHz = 0.0f;
        return;
    }
    f32 sy = sinf(p.moveYaw);
    f32 cy = cosf(p.moveYaw);
    f32 fx = -sy, fz = -cy;  // forward (W)
    f32 rx =  cy, rz = -sy;  // right (D)
    f32 hx = fx * f + rx * r;
    f32 hz = fz * f + rz * r;
    f32 len = sqrtf(hx * hx + hz * hz);
    f32 speed = input.shift ? WALK_SPEED : RUN_SPEED;
    if (input.alt) speed *= SPRINT_MULT;
    *outHx    = hx / len * speed;
    *outHz    = hz / len * speed;
}

// Port of the old engine's ThirdPersonCamera.cpp: spherical orbit around
// feet + TP_LOOK_AT_HEIGHT, 5-ray obstacle clamp (sphere approximation),
// smooth distance recovery after a clamp, and the sky-look tilt at max
// pitch.
static void playerUpdateCamera(char moving) {
    // Orbit only while a camera button is held (old engine: deltas were
    // accumulated only during an ongoing drag).
    if (p.dragging && (input.mouseDx != 0.0f || input.mouseDy != 0.0f)) {
        p.camYaw   -= input.mouseDx * CAM_SENS;
        p.camPitch += input.mouseDy * CAM_SENS;
        if (p.camPitch < PITCH_MIN) p.camPitch = PITCH_MIN;
        if (p.camPitch > PITCH_MAX) p.camPitch = PITCH_MAX;
    }
    if (input.scrollY != 0.0f) {
        p.camDist -= input.scrollY * 2.0f;
        if (p.camDist < DIST_MIN) p.camDist = DIST_MIN;
        if (p.camDist > DIST_MAX) p.camDist = DIST_MAX;
    }

    const f32 sy   = sinf(p.camYaw);
    const f32 cy   = cosf(p.camYaw);
    const f32 cp   = cosf(p.camPitch);
    const f32 sp   = sinf(p.camPitch);
    const f32 dist = p.camDist;

    // Eye in DOUBLE (absolute world metres): the renderer derives the world
    // anchor from it, and the model placement below must use the same value,
    // so the camera is computed BEFORE the model is placed.
    f32 playerPos[3] = {(f32)p.pos[0], (f32)p.pos[1] + TP_LOOK_AT_HEIGHT, (f32)p.pos[2]};
    f32 offset[3]    = {sy * cp * dist, sp * dist, cy * cp * dist};
    f32 desired[3]   = {playerPos[0] + offset[0], playerPos[1] + offset[1], playerPos[2] + offset[2]};
    const f32 fullOrbitDist = dist;

    // ── Obstacle raycasts (sphere approximation, old ThirdPersonCamera) ──
    {
        f32 mainDir[3] = {offset[0], offset[1], offset[2]};
        f32 mainLen    = sqrtf(mainDir[0] * mainDir[0] + mainDir[1] * mainDir[1] + mainDir[2] * mainDir[2]);
        if (mainLen > 0.001f) {
            f32 inv = 1.0f / mainLen;
            mainDir[0] *= inv;
            mainDir[1] *= inv;
            mainDir[2] *= inv;

            f32 right[3], upv[3];
            f32 ref[3];
            if (fabsf(mainDir[1]) < 0.99f) ref[0] = 0.0f, ref[1] = 1.0f, ref[2] = 0.0f;
            else                           ref[0] = 1.0f, ref[1] = 0.0f, ref[2] = 0.0f;
            right[0] = mainDir[1] * ref[2] - mainDir[2] * ref[1];
            right[1] = mainDir[2] * ref[0] - mainDir[0] * ref[2];
            right[2] = mainDir[0] * ref[1] - mainDir[1] * ref[0];
            f32 rl = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
            if (rl > 1e-6f) {
                right[0] /= rl; right[1] /= rl; right[2] /= rl;
            } else {
                right[0] = 1.0f; right[1] = 0.0f; right[2] = 0.0f;
            }
            upv[0] = right[1] * mainDir[2] - right[2] * mainDir[1];
            upv[1] = right[2] * mainDir[0] - right[0] * mainDir[2];
            upv[2] = right[0] * mainDir[1] - right[1] * mainDir[0];

            const f32 offs[5][3] = {
                {0.0f, 0.0f, 0.0f},
                {right[0] * CAM_RADIUS, right[1] * CAM_RADIUS, right[2] * CAM_RADIUS},
                {-right[0] * CAM_RADIUS, -right[1] * CAM_RADIUS, -right[2] * CAM_RADIUS},
                {upv[0] * CAM_RADIUS, upv[1] * CAM_RADIUS, upv[2] * CAM_RADIUS},
                {-upv[0] * CAM_RADIUS, -upv[1] * CAM_RADIUS, -upv[2] * CAM_RADIUS},
            };

            f32 closestDist = mainLen;
            for (int i = 0; i < 5; i++) {
                f32 target[3] = {desired[0] + offs[i][0], desired[1] + offs[i][1], desired[2] + offs[i][2]};
                f32 rayDir[3] = {target[0] - playerPos[0], target[1] - playerPos[1], target[2] - playerPos[2]};
                f32 rayLen    = sqrtf(rayDir[0] * rayDir[0] + rayDir[1] * rayDir[1] + rayDir[2] * rayDir[2]);
                if (rayLen <= 0.001f) continue;
                f32 invl = 1.0f / rayLen;
                rayDir[0] *= invl;
                rayDir[1] *= invl;
                rayDir[2] *= invl;
                f32 hit[3];
                if (joltCastRay(playerPos, rayDir, rayLen, hit)) {
                    f32 hitDist   = sqrtf((hit[0] - playerPos[0]) * (hit[0] - playerPos[0]) +
                                          (hit[1] - playerPos[1]) * (hit[1] - playerPos[1]) +
                                          (hit[2] - playerPos[2]) * (hit[2] - playerPos[2]));
                    f32 projected = hitDist - CAM_RADIUS;
                    if (projected < closestDist) closestDist = projected;
                }
            }
            f32 safeDist = closestDist;
            if (safeDist < 0.05f) safeDist = 0.05f;
            if (safeDist < mainLen) {
                desired[0] = playerPos[0] + mainDir[0] * safeDist;
                desired[1] = playerPos[1] + mainDir[1] * safeDist;
                desired[2] = playerPos[2] + mainDir[2] * safeDist;
            }
        }
    }

    // ── Smooth obstacle recovery (old smoothDist) ──
    {
        f32 fullDist    = dist;
        f32 clampedDist = sqrtf((desired[0] - playerPos[0]) * (desired[0] - playerPos[0]) +
                                (desired[1] - playerPos[1]) * (desired[1] - playerPos[1]) +
                                (desired[2] - playerPos[2]) * (desired[2] - playerPos[2]));
        const char wasClamped = (clampedDist < fullDist - 0.01f);
        if (p.tpSmoothDist < 0.0f) p.tpSmoothDist = clampedDist;
        if (wasClamped) {
            p.tpSmoothDist = clampedDist;
        } else {
            f32 t = (f32)std::min(1.0f, 12.0f * utils::timer.dt);
            p.tpSmoothDist += (clampedDist - p.tpSmoothDist) * t;
        }
        if (fabsf(p.tpSmoothDist - clampedDist) > 0.001f) {
            f32 dx = desired[0] - playerPos[0];
            f32 dy = desired[1] - playerPos[1];
            f32 dz = desired[2] - playerPos[2];
            f32 dl = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dl > 1e-6f) {
                f32 invd = p.tpSmoothDist / dl;
                desired[0] = playerPos[0] + dx * invd;
                desired[1] = playerPos[1] + dy * invd;
                desired[2] = playerPos[2] + dz * invd;
            }
        }
    }

    // ── Sky-look (old skyPitchOffset) ──
    {
        f32 dx = desired[0] - playerPos[0];
        f32 dy = desired[1] - playerPos[1];
        f32 dz = desired[2] - playerPos[2];
        f32 actualCamDist = sqrtf(dx * dx + dy * dy + dz * dz);
        const char cameraClipped = (actualCamDist < fullOrbitDist - 0.2f);
        const char pitchAtMax    = (p.camPitch >= PITCH_MAX - 0.01f);
        const char pushingUp     = (input.mouseDy > 0.0f);
        if (!moving && cameraClipped && pitchAtMax && pushingUp && p.dragging) {
            p.skyPitchOffset += input.mouseDy * CAM_SENS;
            p.skyPitchOffset = std::max(0.0f, std::min(p.skyPitchOffset, 0.44f * (float)M_PI));
        } else {
            if (moving || !cameraClipped || p.camPitch < PITCH_MAX - 0.05f) {
                f32 decay = (f32)std::min(1.0f, 5.0f * utils::timer.dt);
                p.skyPitchOffset *= (1.0f - decay);
                if (p.skyPitchOffset < 0.001f) p.skyPitchOffset = 0.0f;
            }
        }
    }

    // Consume the accumulated look delta: windowPollEvents keeps adding to it
    // across rendered frames until the tick zeroes it (frame-rate independence —
    // see the note in windowPollEvents). No delta is lost at any render fps.
    if (p.dragging) {
        input.mouseDx = 0.0f;
        input.mouseDy = 0.0f;
    }

    const double eye[3] = {desired[0], desired[1], desired[2]};
    double target[3]    = {playerPos[0], playerPos[1], playerPos[2]};
    if (p.skyPitchOffset > 0.001f) {
        f32 camDist = sqrtf((desired[0] - playerPos[0]) * (desired[0] - playerPos[0]) +
                            (desired[1] - playerPos[1]) * (desired[1] - playerPos[1]) +
                            (desired[2] - playerPos[2]) * (desired[2] - playerPos[2]));
        target[1] += tanf(p.skyPitchOffset) * camDist;
    }
    const double up[3] = {0.0, 1.0, 0.0};
    renderer::rendererCameraLookAt(eye, target, up);
}

// ── Animation state machine (port of the old engine's Player.cpp ANIM block) ──
// One active clip + a 0.2 s crossfade into it (gltfPlayAnimationBlended).
// Jump takes priority over ground clips; the T key holds the T-pose until
// the character moves again.
static void playerUpdateAnimation(char moving, char onGround) {
    if (engineTpose()) {
        gltf::gltfPlayAnimationBlended(ANIM_TPOSE, ANIM_SPEED_TPOSE, true, ANIM_BLEND);
        return;
    }

    if (!onGround && !p.animJumping) {
        gltf::gltfPlayAnimationBlended(ANIM_JUMP, ANIM_SPEED_JUMP, false, 0.25f);
        p.animJumping = 1;
        return;
    }
    if (onGround && p.animJumping) {
        p.animJumping = 0;
        if (moving) {
            gltf::gltfPlayAnimationBlended(input.shift ? ANIM_WALK : ANIM_RUN,
                                          input.shift ? ANIM_SPEED_WALK : ANIM_SPEED_RUN, true, 0.2f);
        } else {
            gltf::gltfPlayAnimationBlended(ANIM_IDLE, ANIM_SPEED_IDLE, true, 0.2f);
        }
    }
    if (p.animJumping) return;

    if (input.pressed == SDL_SCANCODE_T && !moving && !p.animTposing) {
        gltf::gltfPlayAnimationBlended(ANIM_TPOSE, ANIM_SPEED_TPOSE, true, 0.3f);
        p.animTposing = 1;
    }
    if (moving && p.animTposing) p.animTposing = 0;

    if (moving && !p.animMoving) {
        gltf::gltfPlayAnimationBlended(input.shift ? ANIM_WALK : ANIM_RUN,
                                      input.shift ? ANIM_SPEED_WALK : ANIM_SPEED_RUN, true, ANIM_BLEND);
        p.animMoving = 1;
    } else if (!moving && p.animMoving) {
        gltf::gltfPlayAnimationBlended(ANIM_IDLE, ANIM_SPEED_IDLE, true, ANIM_BLEND);
        p.animMoving = 0;
    }
}

void PlayerSystem::update() {
    if (!p.spawned || !p.character) return;

    // Fly mode: the player sticks to the camera (old engine's
    // playerFollowFlyingCamera) — park under the eye, keep the Jolt body and
    // orbit angles in sync, and return before the ground gate and physics:
    // the character is teleported, not stepped, so gravity never fights the
    // follow and WASD/mouse input is left to the fly camera.
    if (flyingCameraFlying()) {
        playerFollowFlyingCamera();
        gltf::gltfPlaceAtFacing(p.pos[0], p.pos[1], p.pos[2], p.modelYaw);
        playerTickFootSpeed();  // the follow teleports p.pos — speed the jump
        return;
    }

    // Hold the character at its spawn position until the streaming heightfield
    // body under it exists (the old engine's waitingForGround gate — without
    // it the character would fall through the terrain before the collision
    // data is ready). No active heightmap means a non-heightmap world: the
    // gate clears immediately.
    if (p.waitingForGround) {
        HeightmapTerrain* ht = heightmapTerrainGetActive();
        if (!ht || heightmapTerrainHasBodyAt(ht, p.pos[0], p.pos[2])) {
            p.waitingForGround = 0;
            utils::info("player: ground body ready, releasing character");
        } else {
            // No ground yet: stay pinned at spawn, no physics step.
            if (p.active) playerUpdateCamera(0);
            playerTickFootSpeed();  // pinned: the difference reads 0
            return;
        }
    }

    // Desired velocity (old engine: instant, full air control).
    f32 desiredVel[3] = {0.0f, 0.0f, 0.0f};
    if (p.active) movementInput(&desiredVel[0], &desiredVel[2]);

    // Jump: grounded + SPACE → vertical velocity. The wrapper keeps the
    // desired Y while grounded, cancels it otherwise, and accumulates
    // gravity in the air itself.
    if (p.active && input.keys[SDL_SCANCODE_SPACE] &&
        joltCharacterGetGroundState(p.character) == JOLT_GROUND_STATE_ON_GROUND) {
        desiredVel[1] = JUMP_SPEED;
    }

    // Step the character controller with the full desired velocity — the old
    // engine applied it instantly every frame (no smoothing, no coasting).
    // Vertical is instant too (jump).
    joltCharacterUpdate(p.character, desiredVel, utils::timer.dt);
    double charPos[3];
    joltCharacterGetPositionD64(p.character, charPos);
    p.pos[0] = charPos[0];
    p.pos[1] = charPos[1];
    p.pos[2] = charPos[2];
    playerTickFootSpeed();

    if (getenv("ENGINE_JITTER_PROBE")) {
        static int jn = 0;
        if ((jn++ % 5) == 0) {
            f32 eye[3], fwd[3];
            renderer::rendererCameraGet(eye, fwd);
            utils::info("jit: pos %.6f %.6f %.6f eye %.6f %.6f %.6f", p.pos[0], p.pos[1], p.pos[2],
                        eye[0], eye[1], eye[2]);
        }
    }

    const char moving   = desiredVel[0] != 0.0f || desiredVel[2] != 0.0f;
    const char onGround = joltCharacterGetGroundState(p.character) == JOLT_GROUND_STATE_ON_GROUND;

    // Facing target = the movement direction ONLY (old engine: the model's
    // rotation slerped to the movement direction while moving, and held its
    // pose while idle). RMB does NOT rotate the model — in the old engine the
    // RMB block's `facingYaw = moveYaw` only updated the stored basis (aim /
    // next-input default), it never drove the model's visible rotation.
    if (moving) {
        p.faceTarget = atan2f(desiredVel[0], desiredVel[2]);
    }
    // RMB held: movement basis snaps to the camera's orbit yaw every frame
    // (old playerMovement: moveYaw = cameraYaw), so A/D strafes relative to
    // the camera. LMB drag rotates the camera only — the player's direction
    // and facing stay as is.
    if (p.active && input.mouseRight) {
        p.moveYaw = p.camYaw;
    }
    {
        const f32 diff = atan2f(sinf(p.faceTarget - p.modelYaw), cosf(p.faceTarget - p.modelYaw));
        p.modelYaw     += diff * (f32)std::min(1.0f, TURN_SPEED * utils::timer.dt);
    }
    // Camera BEFORE placement: playerUpdateCamera re-derives the world anchor
    // from the orbit, and gltfPlaceAtFacing re-expresses the feet relative to
    // exactly that anchor — the two must see the same value this frame.
    if (p.active) playerUpdateCamera(moving);
    gltf::gltfPlaceAtFacing(p.pos[0], p.pos[1], p.pos[2], p.modelYaw);
    playerUpdateAnimation(moving, onGround);
}

void PlayerSystem::postUpdate() {
    if (!p.spawned) return;

    static double lastSave = 0.0;
    const double now = utils::millies();

    // The old engine suppressed these periodic saves while ENGINE_AUTO_RUN
    // was running (the test character runs around and must not clobber the
    // parked state).
    if (p.autoRun) return;

    if (now > lastSave + 1000.0) {
        lastSave = now;
        playerDbSaveState();
        // The camera table holds the last camera view in either mode — while
        // the orbit drives the renderer camera, persist its actual eye
        // position + yaw/pitch (fly convention: pitch > 0 = looking up). Gated
        // on p.active: automated runs (screenshot / dolly) keep their scripted
        // camera and must not clobber the saved view.
        if (p.active) {
            f32 pos[3];
            f32 f[3];
            renderer::rendererCameraGet(pos, f);
            f32 yaw   = atan2f(-f[0], -f[2]);
            f32 pitch = asinf(f[1]);
            flyingCameraSaveView(pos, yaw, pitch);
        }
    }
}
}  // namespace engine
