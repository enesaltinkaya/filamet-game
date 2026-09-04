#pragma once
#include "ecs/Ecs.h"

namespace engine {
// Jolt world lifecycle (port of the old engine's PhysicsSystem —
// game-001-cpp c-engine/ecs/system/physics). Owns the single process-wide
// Jolt physics world: joltInit on added, a fixed 1/50 s world step each
// update, joltDestroy on removed.
//
// The world is not thread-safe: all Jolt calls happen on the main thread.
// Consumers that create/destroy bodies outside this system (the heightmap
// terrain's streaming heightfields) check physicsSystemJoltActive() so they
// never touch a destroyed world.
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
}  // namespace engine
