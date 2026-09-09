#include "PlayerActionsGui.h"
#include "Utils.h"
#include "ecs/system/lua/LuaSystem.h"
#include "ecs/system/player/Player.h"

#include "crmlui.h"

#include <cstdio>

namespace game {
static int teleportToOrigin(void* _);
static int playerActionsToggle(void* _);

PlayerActionsGui playerActionsGui;

PlayerActionsGui::PlayerActionsGui() : engine::System("playerActionsGui") {}

static void* document = nullptr;
static void* model    = nullptr;
static char* statusText;
static char statusTextBuf[128];

static void setStatus(const char* text) {
    snprintf(statusTextBuf, sizeof(statusTextBuf), "%s", text);
    statusText = statusTextBuf;
    if (model) rmlUpdateDirtyAll(model);
}

void PlayerActionsGui::added() {
    engine::luaRegisterFunction("playerActionTeleportToOrigin", teleportToOrigin);
    engine::luaRegisterFunction("playerActionsToggle", playerActionsToggle);

    statusText = statusTextBuf;
    snprintf(statusTextBuf, sizeof(statusTextBuf), "");

    document = rmlNewDocument("gui/playerActions/playerActions.html");
    model    = rmlCreateModel("playerActions");
    rmlBindCharPointer(model, "statusText", &statusText);

    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void PlayerActionsGui::removed() {
    if (document) {
        rmlUnloadDocument(document);
        document = nullptr;
    }
    if (model) {
        rmlUnloadModel(model);
        model = nullptr;
    }
}

void PlayerActionsGui::update() {}

static int playerActionsToggle(void* _) {
    static_cast<void>(_);
    void* body = rmlGetElementById(document, "playerActionsBody");
    if (!body) return 0;
    if (rmlElementHasClass(body, "collapsed")) {
        rmlRemoveElementClass(body, "collapsed");
    } else {
        rmlSetElementClass(body, "collapsed");
    }
    return 0;
}

static int teleportToOrigin(void* _) {
    static_cast<void>(_);
    if (!engine::playerTeleportTo(0.0f, 0.0f, 0.0f)) {
        setStatus("Player is not ready");
        return 0;
    }
    setStatus("Teleported to 0, 0, 0");
    utils::info("playerActionsGui: teleported player to origin (0.00 0.00 0.00)");
    return 0;
}
}
