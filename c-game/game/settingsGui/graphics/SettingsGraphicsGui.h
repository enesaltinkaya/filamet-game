#pragma once

#include "ecs/Ecs.h"

namespace game {
// The graphics settings sub-page (gui/settings/graphics/graphics.html):
// TAA, resolution scale, shadows quality,
// reflections (SSR), ambient occlusion, global illumination, bloom, lens
// effects (grain / chromatic aberration / vignette), depth of field
// (+ quality) and fog. BACK returns to the main settings page. (The old
// engine's FSR3 upscaler selector is gone — the manual resolution scale is
// the only resolution control now.)
//
// Wired: controls seed from data/settings.json in added() (the same keys
// rendererGraphicsLoad maps to renderer::GraphicsSettings at startup) and
// every change applies to the live renderer (rendererGraphicsApply) and
// persists (utils::settings* + settingsWrite). The diligent backend does not
// consume every field yet (no post passes ported) — it ignores what it has
// no equivalent for, so some toggles are persist-only until their pass
// lands; see RenderBackend::applyGraphicsSettings.
class SettingsGraphicsGui : public engine::System {
public:
    SettingsGraphicsGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern SettingsGraphicsGui settingsGraphicsGui;

// 1 while the document is alive (between added() and removed()).
char settingsGraphicsGuiIsShowing(void);
}
