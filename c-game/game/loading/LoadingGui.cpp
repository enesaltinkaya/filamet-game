#include "LoadingGui.h"
#include "Game.h"
#include "gui/rmlui/GuiManagerRmlUi.h"
#include "Utils.h"

#include "crmlui.h"

#include <cstdlib>

namespace game {
    LoadingGui loadingGui;

    LoadingGui::LoadingGui() : engine::System("loading") {}

    static void* document    = nullptr;
    static char loadStarted  = 0;
    static char worldReady   = 0;
    static double shownNanos = 0.0;

    // Minimum time the loading screen stays up (the old engine used 1.0 s so
    // a fast load never flashed). The world load here is synchronous (~0.3 s),
    // so the screen is up for the load's duration plus whatever is left of the
    // minimum. ENGINE_LOADING_MIN_MS overrides (milliseconds; 0 disables the
    // floor and the world appears as soon as the load returns).
    static double minDisplayNanos(void) {
        static const double ns = [] {
            const char* e = getenv("ENGINE_LOADING_MIN_MS");
            double ms = e ? atof(e) : 400.0;
            if (ms < 0.0) ms = 0.0;
            return ms * 1e6;
        }();
        return ns;
    }

    void loadingGuiShowNow() {
        loadStarted = 0;
        worldReady  = 0;
        shownNanos  = utils::nanos();
        if (!document) {
            document        = rmlNewDocument("gui/loading/loading.html");
            rmlLoadDocument(document);
            rmlShowDocument(document);
        }
    }

    void LoadingGui::added() {
        // Normally the document is already up (shown synchronously on the
        // ENTER WORLD click, before this deferred add lands). Safety net for
        // a direct add: show it here.
        loadingGuiShowNow();
    }

    void LoadingGui::removed() {
        if (document) {
            rmlUnloadDocument(document);
            document = nullptr;
        }
    }

    void LoadingGui::update() {
        // added() ran last frame's postUpdate and the document has rendered at
        // least once, so the blocking world load happens now — the user sees
        // the loading screen, not a frozen main menu.
        if (!loadStarted) {
            loadStarted = 1;
            gameSystem.loadWorld();
            worldReady = 1;
        }
        if (worldReady && (utils::nanos() - shownNanos) >= minDisplayNanos()) {
            gameSystem.finishWorldLoad();
        }
    }
}  // namespace game
