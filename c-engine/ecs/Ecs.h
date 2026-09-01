#pragma once

#include "Defines.h"
#include <entt/entt.hpp>
#include <vector>

namespace engine {

class System {
public:
    explicit System(const char* systemName) : name(systemName) {}
    virtual ~System() = default;

    virtual void added() {}
    virtual void removed() {}
    virtual void preUpdate() {}
    virtual void update() {}
    virtual void postUpdate() {}

    const char* name;
    i32 priority = 0;
    double cpuElapsedLastFrame = 0.0;
    double cpuElapsed = 0.0;
};

struct Ecs {
    entt::registry registry;
    std::vector<System*> systems;
};

extern struct Ecs ecs;

void ecsInit(System* gameSystem);
void ecsDestroy(void);
void ecsPreUpdate(void);
void ecsUpdate(void);
void ecsPostUpdate(void);

void systemAdd(int order, System* system);  // insert by priority, calls system->added()
void systemRemove(System* system);          // calls system->removed(), erases
}  // namespace engine
