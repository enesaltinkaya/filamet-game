#include "PlayerGui.h"
#include "Utils.h"
#include "ecs/system/player/Player.h"

#include "crmlui.h"

namespace game {

PlayerGui playerGui;

PlayerGui::PlayerGui() : engine::System("playerGui") {}

static void* document = nullptr;
static void* model    = nullptr;
static float posX, posY, posZ;

void PlayerGui::added() {
    document = rmlNewDocument("gui/player/player.html");
    model    = rmlCreateModel("player");

    rmlBindFloat(model, "posX", &posX);
    rmlBindFloat(model, "posY", &posY);
    rmlBindFloat(model, "posZ", &posZ);

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

    lastShown = now;
    rmlUpdateDirtyAll(model);
}

}
