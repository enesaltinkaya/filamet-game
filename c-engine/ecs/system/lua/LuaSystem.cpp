#include "ecs/system/lua/LuaSystem.h"
#include "Engine.h"
#include "Utils.h"
#include "ecs/system/sound/SoundSystem.h"

// Lua 5.4's public headers carry no `extern "C"` guard, so they must be pulled
// in under C linkage or their symbols get C++-mangled and the C-built liblua.a
// (unmangled) fails to resolve them at link time.
extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace engine {
static i32 luacHoverSound(void*);
static i32 luacClickSound(void*);
static i32 luacExit(void*);

static lua_State* luaState = nullptr;
LuaSystem luaSystem;

LuaSystem::LuaSystem() : System("lua") {}

void LuaSystem::added() {
    luaState = luaL_newstate();
    luaL_openlibs(luaState);

    luaRegisterFunction("luacHoverSound", luacHoverSound);
    luaRegisterFunction("luacClickSound", luacClickSound);
    luaRegisterFunction("luacExit", luacExit);
}

void LuaSystem::removed() {
    // Deliberately does NOT close the state here: ecsDestroy runs the systems
    // in priority order, so this would destroy the Lua state before the
    // higher-priority rmlui systems' removed() → rmlDestroy() → Rml::Shutdown
    // → ~Context → ~LuaEventListener (lua_getglobal on a closed state =
    // segfault). Engine.cpp calls luaDestroy() after ecsDestroy(), like the
    // old engine did.
}

void luaDestroy(void) {
    if (luaState) {
        lua_close(luaState);
        luaState = nullptr;
    }
}

i32 luacHoverSound(void*) {
    soundPlayHover();
    return 0;
}

i32 luacClickSound(void*) {
    soundPlayClick();
    return 0;
}

i32 luacExit(void*) {
    engineStop();
    return 0;
}

void luaRegisterFunction(const char* name, LuaFunction luaFunction) {
    lua_register(luaState, name, reinterpret_cast<lua_CFunction>(luaFunction));
}

void* luaGetState(void) {
    return luaState;
}

void luaLoadFile(const char* path) {
    utils::String buffer = utils::dataManagerRead(path);
    if (luaL_loadstring(luaState, buffer.data) || lua_pcall(luaState, 0, 0, 0)) {
        utils::error("failed to load lua: %s", path);
    }
    // buffer is a stack String owned by the data manager; nothing to free here
}

void luaCallFunction(const char* functionName) {
    lua_getglobal(luaState, functionName);
    if (lua_pcall(luaState, 0, 0, 0)) {
        utils::error("failed to call: %s", functionName);
        utils::error("%s", lua_tostring(luaState, -1));
    }
}
}  // namespace engine
