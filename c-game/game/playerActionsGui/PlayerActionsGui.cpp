#include "PlayerActionsGui.h"
#include "Utils.h"
#include "azgaar/AzgaarWorld.h"
#include "ecs/system/lua/LuaSystem.h"
#include "ecs/system/player/Player.h"
#include "loadingAzgaar/LoadingAzgaar.h"

#include "crmlui.h"

#include <cstdio>

namespace game {
static int teleportToCell(void* _);
static int teleportToOrigin(void* _);
static int playerActionsToggle(void* _);

PlayerActionsGui playerActionsGui;

PlayerActionsGui::PlayerActionsGui() : engine::System("playerActionsGui") {}

static void* document = nullptr;
static void* model    = nullptr;
static float cellId;
static char* statusText;
static char statusTextBuf[128];

static void setStatus(const char* text) {
    snprintf(statusTextBuf, sizeof(statusTextBuf), "%s", text);
    statusText = statusTextBuf;
    if (model) rmlUpdateDirtyAll(model);
}

void PlayerActionsGui::added() {
    engine::luaRegisterFunction("playerActionTeleportToCell", teleportToCell);
    engine::luaRegisterFunction("playerActionTeleportToOrigin", teleportToOrigin);
    engine::luaRegisterFunction("playerActionsToggle", playerActionsToggle);

    cellId     = 0.0f;
    statusText = statusTextBuf;
    snprintf(statusTextBuf, sizeof(statusTextBuf), "Enter Azgaar cell id");

    document = rmlNewDocument("gui/playerActions/playerActions.html");
    model    = rmlCreateModel("playerActions");
    rmlBindFloat(model, "cellId", &cellId);
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

// ── Lua callbacks (onclick handlers in playerActions.html) ───────────────

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

static int teleportToCell(void* _) {
    static_cast<void>(_);

    const AzgaarWorld* world = loadingAzgaarGetWorld();
    if (!world || world->cells.empty() || world->cellCount == 0u) {
        setStatus("Azgaar world is not loaded");
        return 0;
    }

    u32 id = static_cast<u32>(cellId + 0.5f);
    if (id >= world->cellCount) {
        snprintf(statusTextBuf, sizeof(statusTextBuf), "Invalid cell %u (max %u)", id, world->cellCount - 1u);
        statusText = statusTextBuf;
        rmlUpdateDirtyAll(model);
        return 0;
    }

    const AzgaarCell* cell = &world->cells[id];
    float wx = 0.0f;
    float wz = 0.0f;
    azgaarMapToWorld(world, cell->x, cell->y, &wx, &wz);

    float h = azgaarHeightToMeters(world, cell->height);
    if (!engine::playerTeleportTo(wx, h + 1.0f, wz)) {
        setStatus("Player is not ready");
        return 0;
    }

    snprintf(statusTextBuf, sizeof(statusTextBuf), "Teleported to cell %u", id);
    statusText = statusTextBuf;
    rmlUpdateDirtyAll(model);
    utils::info("playerActionsGui: teleported player to Azgaar cell %u (%.2f %.2f %.2f)", id, wx, h + 1.0f, wz);
    return 0;
}
}  // namespace game
