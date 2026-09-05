#include "PassStatsGui.h"

#include "Utils.h"
#include "gui/rmlui/GuiManagerRmlUi.h"

#include "crmlui.h"

#include <cstdio>

namespace engine {

// Port of the old engine's PassStatsGui. The old document (pak_0_engine/gui/
// passstats/passstats.html) is unchanged, but this engine has no per-pass GPU
// profiling (no VulkanProfile / vulkanGetPassProfiles in the Diligent
// backend), so the pass list stays empty (passCount 0) and the list/detail
// transforms emit empty strings — the document renders its header with no
// rows instead of the old engine's per-pass GPU/CPU times.
PassStatsGui passStatsGui;

PassStatsGui::PassStatsGui() : System("passStatsGui") {}

static void onClickPassRow(void* element, const char* eventType, const char* id, EventParameter* parameter);

static void* document    = nullptr;
static void* model       = nullptr;
static void* bodyElement = nullptr;
static double lastUpdate;

static int selectedPassIndex = -1;
static size_t passCount      = 0;

static char selectedPassName[64] = {0};
static double selectedPassGpuElapsed;
static double selectedPassCpuElapsed;

static void passListInfo(int index, int type, char* out);
static void selectedPassInfo(int index, int type, char* out);

static void* findPassRowAncestor(void* element) {
    void* current = element;
    while (current) {
        if (rmlElementHasClass(current, "pass-row")) {
            return current;
        }
        current = rmlElementGetParentNode(current);
    }
    return nullptr;
}

static int collectPassRows(void** outElements, int maxElements) {
    void* allDivs[256];
    int total = rmlQuerySelectorAll(bodyElement, "div", allDivs, 256);
    int count = 0;
    for (int i = 0; i < total && count < maxElements; i++) {
        if (rmlElementHasClass(allDivs[i], "pass-row")) {
            outElements[count++] = allDivs[i];
        }
    }
    return count;
}

static int getPassRowIndex(void* passRow) {
    void* elements[64];
    int count = collectPassRows(elements, 64);
    for (int i = 0; i < count; i++) {
        if (elements[i] == passRow) {
            return i;
        }
    }
    return -1;
}

void PassStatsGui::added() {
    document = rmlNewDocument("gui/passstats/passstats.html");
    model    = rmlCreateModel("passstats");

    rmlBind(model, "selectedPassIndex", &selectedPassIndex);
    rmlBind(model, "passCount", &passCount);

    rmlRegisterTransformFunc(model, "passListInfo", passListInfo);
    rmlRegisterTransformFunc(model, "selectedPassInfo", selectedPassInfo);

    rmlBindArray(model, "passes", &passCount);

    rmlLoadDocument(document);
    rmlShowDocumentWithoutFocus(document);

    bodyElement = rmlGetElementById(document, "passstats");

    rmlBindEventListener(document, "click", onClickPassRow);
}

void onClickPassRow(void* element, const char* eventType, const char* id, EventParameter* parameter) {
    (void)eventType;
    (void)id;
    (void)parameter;

    void* passRow = findPassRowAncestor(element);
    if (!passRow) {
        return;  // click was not on a pass row
    }

    int index = getPassRowIndex(passRow);
    if (index < 0 || (size_t)index >= passCount) {
        return;  // invalid index
    }

    if (selectedPassIndex != index) {
        int prevSelected = selectedPassIndex;
        selectedPassIndex = index;

        void* elements[64];
        int count = collectPassRows(elements, 64);
        for (int i = 0; i < count; i++) {
            if (i == selectedPassIndex)
                rmlSetElementClass(elements[i], "selected");
            else if (i == prevSelected)
                rmlRemoveElementClass(elements[i], "selected");
        }

        // No pass profiles in this engine — nothing to cache.
        selectedPassName[0] = '\0';
        selectedPassGpuElapsed = 0.0;
        selectedPassCpuElapsed = 0.0;

        rmlUpdateDirtyAll(model);
    }
}

void PassStatsGui::removed() {
    selectedPassIndex = -1;
    if (document) {
        rmlUnloadDocument(document);
        document    = nullptr;
        bodyElement = nullptr;
    }
    if (model) {
        rmlUnloadModel(model);
        model = nullptr;
    }
}

void PassStatsGui::update() {
    double now = utils::nanos();
    if (now > lastUpdate + BILLION / 2.) {  // twice per second
        // No per-pass profiling in the Diligent engine — the list stays empty.
        passCount = 0;

        lastUpdate = now;
        rmlUpdateDirtyAll(model);
    }
}

void passListInfo(int index, int type, char* out) {
    (void)index;
    (void)type;
    out[0] = '\0';  // no pass profiles in this engine
}

void selectedPassInfo(int index, int type, char* out) {
    (void)index;
    (void)type;
    out[0] = '\0';  // no pass profiles in this engine
}

void passStatsGuiToggle(void) {
    if (document) {
        guiManagerRemoveGuiNextFrame(&passStatsGui);
    } else {
        guiManagerAddGuiNextFrame(&passStatsGui);
    }
}

}  // namespace engine
