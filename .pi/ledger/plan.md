# plan

Strategy: port the old engine's RmlUi GUI system from /home/enes/Projects/c/game-001-cpp into
this project, keeping the existing C wrapper (crmlui, prebuilt at
/home/enes/Projects/c/cpp-thirdparty/rmlui, Vulkan renderer). The old system is
`c-engine/renderer/gui/rmlui/GuiManagerRmlUi.{h,cpp}` plus its guis (DebugGui, PassStatsGui,
ShowFpsGui, StatsGui) and game-level guis; it drives RmlUi documents via the crmlui C API.
Approach: add a `gui_rmlui` (or equivalent) layer in c-engine that wraps crmlui.h,
wire it into the renderer's frame (Vulkan device/context, after main render pass),
link `${thirdparty}/rmlui/wrapper/build-linux/libcrmlui.a` + the librmlui* libs the old
c-game CMakeLists already references, port the GuiManagerRmlUi System and at least the
engine guis, adapt to this engine's ECS/pak/input conventions, and register it in
c-engine/c-game CMake + Game.cpp. Decide during execution whether the new engine's
ImGui-based gui layer is kept side-by-side or replaced — the user explicitly wants the
wrapper path, so RmlUi must be the working GUI; do not delete the existing gui/ layer
until the rmlui path works (revert safety).

Verification: ./scripts/build.sh
Baseline commit: 5f511142fb847e6c622339460624a2cb95dd3a4f (clean)
