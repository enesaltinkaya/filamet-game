#pragma once
#include "ecs/Ecs.h"

namespace engine {
// Jolt world lifecycle (port of the old engine's PhysicsSystem —
// game-001-cpp c-engine/ecs/system/physics). Owns the single process-wide
// Jolt physics world: joltInit on added, a fixed 1/50 s world step each
// update, joltDestroy on removed.
//
// The world is not thread-safe: all Jolt calls happen on the main thread.
// Consumers that create/destroy bodies outside this system check
// physicsSystemJoltActive() so they never touch a destroyed world.
//
// The step rate (0.02 s) is the old engine's: it steps dynamic rigid bodies
// only — the character controller (CharacterVirtual) steps itself with the
// real frame dt in its own update.
class PhysicsSystem final : public System {
public:
    PhysicsSystem();
    void added() override;
    void removed() override;
    void update() override;
};

extern PhysicsSystem physicsSystem;

/// True while the Jolt world is alive (between the physics system's
/// added() and removed()).
char physicsSystemJoltActive(void);

/// Register the terrain's pre-baked Jolt sidecar (scripts/blender-terrain.py:
/// the .jolt JBVH v2 file next to the terrain model in the pak). The game
/// sets this from loadWorld() — the physics system is (re)added deferred a
/// moment later, so the sidecar is remembered and its static bodies restored
/// in added() once the world is up. If the world is already active the load
/// happens immediately. Re-setting replaces the previous bodies (world
/// re-entry).
void physicsTerrainSidecarSet(const char* pakPath);
}
