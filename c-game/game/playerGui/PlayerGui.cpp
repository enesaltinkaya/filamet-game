#include "PlayerGui.h"
#include "Utils.h"
#include "azgaar/AzgaarWorld.h"
#include "ecs/system/player/Player.h"
#include "loadingAzgaar/LoadingAzgaar.h"

#include "crmlui.h"

#include <cstdio>

namespace game {

PlayerGui playerGui;

PlayerGui::PlayerGui() : engine::System("playerGui") {}

static void* document = nullptr;
static void* model    = nullptr;
static float posX, posY, posZ;
static char* cellText;
static char cellTextBuf[32];

// World xz -> map pixels (the inverse of azgaarMapToWorld: the map is
// centred on the world origin).
static void worldToMap(const AzgaarWorld* world, float wx, float wz, float* outMapX, float* outMapY) {
    *outMapX = (-wx) / static_cast<float>(world->metersPerPixel) + static_cast<float>(world->widthPx) * 0.5f;
    *outMapY = (-wz) / static_cast<float>(world->metersPerPixel) + static_cast<float>(world->heightPx) * 0.5f;
}

void PlayerGui::added() {
    document = rmlNewDocument("gui/player/player.html");
    model    = rmlCreateModel("player");
    cellText = cellTextBuf;

    rmlBindFloat(model, "posX", &posX);
    rmlBindFloat(model, "posY", &posY);
    rmlBindFloat(model, "posZ", &posZ);
    rmlBindCharPointer(model, "cellText", &cellText);

    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void PlayerGui::removed() {
    if (document) {
        rmlUnloadDocument(document);
        document = nullptr;
    }
    if (model) {
        rmlUnloadModel(model);
        model = nullptr;
    }
}

void PlayerGui::update() {
    static double lastShown;
    double now = utils::millies();
    if (now <= lastShown + 50) return;  // 50ms, like the old gui

    double footPos[3];
    if (!engine::playerGetFootPos(footPos)) return;  // no player in the world

    posX = (float)footPos[0];
    posY = (float)footPos[1];
    posZ = (float)footPos[2];

    snprintf(cellTextBuf, sizeof(cellTextBuf), "n/a");

    const AzgaarWorld* world = loadingAzgaarGetWorld();
    if (world && world->metersPerPixel > 0.0) {
        float mapX = 0.0f;
        float mapY = 0.0f;
        u32 cellIndex = (u32)-1;
        worldToMap(world, posX, posZ, &mapX, &mapY);
        azgaarWorldSampleHeightCell(world, mapX, mapY, &cellIndex);
        if (cellIndex != (u32)-1) {
            snprintf(cellTextBuf, sizeof(cellTextBuf), "%u", cellIndex);
        }
    }

    lastShown = now;
    rmlUpdateDirtyAll(model);
}

}  // namespace game
