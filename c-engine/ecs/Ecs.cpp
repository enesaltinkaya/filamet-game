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
}  // namespace engine
