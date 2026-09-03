#include "loadingAzgaar/LoadingAzgaar.h"
#include "Utils.h"

namespace game {

// Shipped at c-game/data/pak_1/azgaar/ (space in the name is intentional).
static const char* const kMapPath = "azgaar/Chilerel 2026-08-11-15-35.map";

static AzgaarWorld s_world;
static AzgaarHeightmapSource s_source = {}; // lives as long as the world
static bool s_loaded = false;

bool loadingAzgaarLoad() {
    if (s_loaded) return true;
    if (!azgaarWorldLoad(&s_world, kMapPath)) {
        utils::warn("loadingAzgaar: load failed: %s", kMapPath);
        return false;
    }
    s_loaded = true;
    // Adapter over the parsed world. Builds the settlement plateau grid
    // inside, so the grid is live before any heightmap tile generates.
    azgaarHeightmapSourceInit(&s_source, &s_world, s_world.mapName);
    utils::info("loadingAzgaar: '%s' cells=%u biomes=%u rivers=%u settlements=%u %fx%fm (%.2f m/px)",
                s_world.mapName, s_world.cellCount, s_world.biomeCount, s_world.riverCount,
                s_world.settlementCount, s_world.widthPx * s_world.metersPerPixel,
                s_world.heightPx * s_world.metersPerPixel, s_world.metersPerPixel);
    return true;
}

const AzgaarWorld* loadingAzgaarGetWorld() {
    return s_loaded ? &s_world : nullptr;
}

AzgaarHeightmapSource* loadingAzgaarGetHeightmapSource() {
    return s_loaded ? &s_source : nullptr;
}

void loadingAzgaarReleaseWorld() {
    if (!s_loaded) return;
    azgaarWorldDestroy(&s_world);
    s_loaded = false;
}
}  // namespace game
