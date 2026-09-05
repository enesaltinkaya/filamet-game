#pragma once

#include "ecs/Ecs.h"

namespace game {
// The graphics settings sub-page (gui/settings/graphics/graphics.html) — the
// old engine's page ported as-is: FSR3 upscaler + TAA, RCAS sharpening,
// resolution scale, shadows quality, contact shadows, reflections (SSR),
// ambient occlusion, global illumination, bloom, lens effects (grain /
// chromatic aberration / vignette), depth of field (+ quality) and fog,
// BACK returns to the main settings page.
//
// GUI-only port: this engine has none of those post passes yet, so every
// control moves local placeholder state only (labels flip, sliders track,
// nothing touches the renderer or the settings file). Engine wiring and
// persistence come later — the old engine's handlers read live state from
// the vulkan*Pass getters / renderer and wrote it back through the setters
// + utils::settings; the TODO(graphics-wire) comments mark those spots.
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
}  // namespace game
