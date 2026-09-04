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
static const float CAPSULE_CENTER      = 0.70f; // feet → centre (0.45 + 0.25), camera target
static const float MAX_SLOPE_ANGLE     = 45.0f * (float)M_PI / 180.0f; // old joltCharacterCreate
static const float RUN_SPEED           = 4.0f; // m/s
static const float WALK_SPEED          = 2.0f; // shift-held
static const float JUMP_SPEED          = 4.0f; // vertical jump velocity

// ── Third-person orbit camera (old engine's orbit-mode ranges) ──────────────
static const float CAM_SENS       = 0.002f; // rad/px (same as the flying camera)
static const float PITCH_MIN      = -20.0f * (float)M_PI / 180.0f;
static const float PITCH_MAX      = 60.0f * (float)M_PI / 180.0f;
static const float DIST_MIN       = 1.5f;
static const float DIST_MAX       = 20.0f;
static const float DIST_DEFAULT   = 10.0f;

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
static const f32 ANIM_BLEND       = 0.2f;
static const f32 TURN_SPEED       = 20.0f; // old engine's MOVE_SPEED_TURN

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

static struct {
    f32 pos[3];  // feet position, world metres (Jolt character position)
    JoltCharacter* character = nullptr;
    char waitingForGround    = 0; // pinned at spawn until the body under it exists
    f32 camYaw   = 0.0f;
    f32 camPitch = 20.0f * (float)M_PI / 180.0f;
    f32 camDist  = DIST_DEFAULT;
    f32 spawn[3] = {0.0f, 0.0f, 0.0f};
    f32 moveYaw   = 0.0f;  // movement basis (old engine's moveYaw — stable during LMB drags)
    f32 facingYaw = 0.0f;  // model yaw toward the movement direction
    char animMoving  = 0; // old engine's isMoving
    char animJumping = 0; // old engine's isJumping
    char animTposing = 0; // T-key emote active until movement resumes
    char active  = 0;
    char spawned = 0;
    char autoRun = 0;
    char dragging = 0; // LMB/RMB drag in flight — relative mouse mode is ours
} p = {};

void playerSetSpawn(f32 x, f32 y, f32 z) {
    p.spawn[0]   = x;
    p.spawn[1]   = y;
    p.spawn[2]   = z;
    p.spawned    = 0;
}

char playerMode(void) { return p.active; }

char playerTeleportTo(f32 x, f32 y, f32 z) {
    if (!p.spawned || !p.character) return 0;
    p.pos[0] = x;
    p.pos[1] = y;
    p.pos[2] = z;
    joltCharacterSetPosition(p.character, p.pos);
    p.autoRun = 0;  // old engine: a teleport cancels auto-run
    return 1;
}

// ── Player DB (persist position + orbit camera state across runs —
// port of the old engine's Player.cpp PlayerDb; the old engine split this
// into transformDb("player") + playerDb("player"), we use one blob) ──────
struct PlayerDb {
    f32 pos[3];
    f32 facingYaw;
    f32 camYaw;
    f32 camPitch;
    f32 camDist;
    f32 moveYaw;  // appended last — old blobs lack it (fall back to facingYaw)
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
        .pos       = {p.pos[0], p.pos[1], p.pos[2]},
        .facingYaw = p.facingYaw,
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

static void playerSpawn(void) {
    HeightmapTerrain* ht = heightmapTerrainGetActive();
    f32 groundY           = ht ? heightmapTerrainSample(ht, p.spawn[0], p.spawn[2]) : p.spawn[1];
    p.pos[0]               = p.spawn[0];
    p.pos[1]               = groundY;
    p.pos[2]               = p.spawn[2];
    p.camDist              = DIST_DEFAULT;
    p.moveYaw               = 0.0f;  // orbit convention: W runs away from the camera
    p.facingYaw            = 0.0f;
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
        p.facingYaw = saved.facingYaw;
        p.moveYaw   = (savedSize >= (int)sizeof(PlayerDb)) ? saved.moveYaw
                                                           : saved.facingYaw + (float)M_PI;
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
        f32 dy = savedEye[1] - (p.pos[1] + CAPSULE_CENTER);
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
        p.character = joltCharacterCreate(CAPSULE_HALF_HEIGHT, CAPSULE_RADIUS, p.pos, MAX_SLOPE_ANGLE);
        if (!p.character) utils::warn("player: Jolt character creation failed");
    }
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
    char automated = (getenv("ENGINE_SCREENSHOT") != nullptr) ||
                     (getenv("ENGINE_CAMERA_DOLLY") != nullptr) ||
                     (getenv("ENGINE_RENDERDOC_CAPTURE") != nullptr) ||
                     (getenv("ENGINE_NO_PLAYER") != nullptr);
    p.autoRun = autoRunEnabled();
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
    // F started a fly this frame (the fly system runs first and already
    // captured the mouse): yield without touching the mouse mode.
    if (p.active && flyingCameraFlying()) {
        p.active = 0;
        p.dragging = 0;  // fly owns the capture now
        return;
    }

    if (input.pressed == SDL_SCANCODE_C) {
        if (!p.active) {
            if (flyingCameraFlying()) {
                // Take over from a fly: park the player where the camera was
                // (the old engine's playerFollowFlyingCamera) — the first
                // physics update drops it onto the ground.
                f32 pos[3];
                f32 f[3];
                renderer::rendererCameraGet(pos, f);
                p.pos[0] = pos[0];
                p.pos[1] = pos[1] - 3.0f;
                p.pos[2] = pos[2];
                if (p.character) joltCharacterSetPosition(p.character, p.pos);
                flyingCameraStop();
            }
            playerSetActive(1);
        } else {
            playerSetActive(0);
        }
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
    *outHx    = hx / len * speed;
    *outHz    = hz / len * speed;
}

static void playerUpdateCamera(void) {
    // Orbit only while a camera button is held (old engine: deltas were
    // accumulated only during an ongoing drag).
    if (p.dragging && (input.mouseDx != 0.0f || input.mouseDy != 0.0f)) {
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
    // The camera orbits the capsule centre (feet + 0.70 m), not the feet.
    f32 cx   = p.pos[0];
    f32 cy   = p.pos[1] + CAPSULE_CENTER;
    f32 cz   = p.pos[2];
    f32 eye[3] = {
        cx + sy * cp * p.camDist,
        cy + sp * p.camDist,
        cz + cosf(p.camYaw) * cp * p.camDist,
    };
    f32 up[3] = {0.0f, 1.0f, 0.0f};
    f32 target[3] = {cx, cy, cz};
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
            if (p.active) playerUpdateCamera();
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

    // Step the character controller, read the position back (feet).
    joltCharacterUpdate(p.character, desiredVel, utils::timer.dt);
    f32 charPos[3];
    joltCharacterGetPosition(p.character, charPos);
    p.pos[0] = charPos[0];
    p.pos[1] = charPos[1];
    p.pos[2] = charPos[2];

    const char moving   = desiredVel[0] != 0.0f || desiredVel[2] != 0.0f;
    const char onGround = joltCharacterGetGroundState(p.character) == JOLT_GROUND_STATE_ON_GROUND;

    // Face the model toward the movement direction (old engine's animFacingYaw
    // smoothed by MOVE_SPEED_TURN). The model keeps its last facing at rest.
    if (moving) {
        const f32 target = atan2f(desiredVel[0], desiredVel[2]);
        const f32 diff   = atan2f(sinf(target - p.facingYaw), cosf(target - p.facingYaw));
        p.facingYaw     += diff * (f32)std::min(1.0f, TURN_SPEED * utils::timer.dt);
    }

    // RMB held: the player's direction snaps to the camera's orbit forward
    // (old engine: while the right button is held, moveYaw/facingYaw =
    // cameraYaw — the model faces away from the camera). moveYaw is in the
    // orbit convention (camera→player = −(sin, 0, cos) at that yaw); the
    // model faces forward yaw (sin, 0, cos), so that is camYaw + π.
    // LMB drag rotates the camera only — the player's direction stays as is.
    if (p.active && input.mouseRight) {
        p.moveYaw   = p.camYaw;
        p.facingYaw = atan2f(-sinf(p.camYaw), -cosf(p.camYaw));
    }
    gltf::gltfPlaceAtFacing(p.pos[0], p.pos[1], p.pos[2], p.facingYaw);
    playerUpdateAnimation(moving, onGround);
    if (p.active) playerUpdateCamera();
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
