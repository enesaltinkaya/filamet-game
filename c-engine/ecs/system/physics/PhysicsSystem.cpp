#include "PhysicsSystem.h"
#include "Utils.h"

namespace engine {
PhysicsSystem physicsSystem;

static char joltActive = 0;

char physicsSystemJoltActive(void) {
    return joltActive;
}

PhysicsSystem::PhysicsSystem() : System("physics") {}

void PhysicsSystem::added() {
    joltInit();
    joltActive = 1;
    utils::info("physics: Jolt world up");
}

void PhysicsSystem::removed() {
    // ecsDestroy snapshots the system list, so removed() may run twice;
    // joltDestroy must not run twice (it does not null-check).
    if (!joltActive) return;
    joltActive = 0;
    joltDestroy();
    utils::info("physics: Jolt world down");
}

void PhysicsSystem::update() {
    joltUpdate(0.02f);
}
}  // namespace engine
