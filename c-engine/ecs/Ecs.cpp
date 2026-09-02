#include "Ecs.h"
#include "Utils.h"

namespace engine {
struct Ecs ecs;

void ecsInit(System* gameSystem) {
    utils::info("ecs: initializing");
    systemAdd(0, gameSystem);
}

void ecsDestroy(void) {
    utils::info("ecs: destroying");
    for (System* system : ecs.systems) {
        system->removed();
    }
    ecs.systems.clear();
}

void systemAdd(int order, System* system) {
    system->priority = order;

    size_t index = 0;
    while (index < ecs.systems.size() && ecs.systems[index]->priority <= order) {
        index++;
    }
    ecs.systems.insert(ecs.systems.begin() + index, system);

    utils::info("ecs: system added (%s, priority %d)", system->name, order);
    system->added();
}

void systemRemove(System* system) {
    for (size_t i = 0; i < ecs.systems.size(); i++) {
        if (ecs.systems[i] == system) {
            ecs.systems.erase(ecs.systems.begin() + i);
            break;
        }
    }
    utils::info("ecs: system removed (%s)", system->name);
    system->removed();
}

void ecsPreUpdate(void) {
    for (System* system : ecs.systems) {
        double start = utils::elapsedBegin();
        system->preUpdate();
        system->cpuElapsed = utils::elapsedEnd(start);
        system->cpuElapsedLastFrame = system->cpuElapsed;
    }
}

void ecsUpdate(void) {
    for (System* system : ecs.systems) {
        double start = utils::elapsedBegin();
        system->update();
        system->cpuElapsed = utils::elapsedEnd(start);
        system->cpuElapsedLastFrame = system->cpuElapsed;
    }
}

void ecsPostUpdate(void) {
    for (System* system : ecs.systems) {
        double start = utils::elapsedBegin();
        system->postUpdate();
        system->cpuElapsed = utils::elapsedEnd(start);
        system->cpuElapsedLastFrame = system->cpuElapsed;
    }
}

// ── Deferred (next-frame) system changes ─────────────────────────────────────
// A system may request an add/remove from within its own callback (e.g. a GUI
// button that transitions state). Applying that inline would mutate ecs.systems
// while the phase loop is iterating it, so we queue it and apply at the top of
// the next frame, outside any loop.
static std::vector<System*> deferredAdds;
static std::vector<int>     deferredAddOrder;
static std::vector<System*> deferredRemoves;

static bool deferredAddPending(System* s) {
    for (System* x : deferredAdds) if (x == s) return true;
    return false;
}

void ecsSystemAddDeferred(int order, System* system) {
    // cancel a pending remove for the same system, then queue the add
    for (size_t i = 0; i < deferredRemoves.size(); i++) {
        if (deferredRemoves[i] == system) {
            deferredRemoves.erase(deferredRemoves.begin() + i);
            break;
        }
    }
    if (!deferredAddPending(system)) {
        deferredAdds.push_back(system);
        deferredAddOrder.push_back(order);
    }
}

void ecsSystemRemoveDeferred(System* system) {
    // cancel a pending add for the same system, then queue the remove
    for (size_t i = 0; i < deferredAdds.size(); i++) {
        if (deferredAdds[i] == system) {
            deferredAdds.erase(deferredAdds.begin() + i);
            deferredAddOrder.erase(deferredAddOrder.begin() + i);
            break;
        }
    }
    for (System* x : deferredRemoves) if (x == system) return;
    deferredRemoves.push_back(system);
}

void ecsApplyDeferred(void) {
    for (System* s : deferredRemoves) systemRemove(s);
    deferredRemoves.clear();
    for (size_t i = 0; i < deferredAdds.size(); i++) {
        systemAdd(deferredAddOrder[i], deferredAdds[i]);
    }
    deferredAdds.clear();
    deferredAddOrder.clear();
}
}  // namespace engine
