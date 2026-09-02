# Plan: implement plans/diligent-migration.md (Filament → Diligent Engine)

## Strategy

Follow the migration plan's phases in order on a new branch: (1) prep — branch,
inspect `models/terrain/oghuzlands.glb` for KTX2/embedded-texture questions, and
capture `ENGINE_SCREENSHOT` baselines of the current Filament build. No GLB re-export:
`-kn -kv -ke` are gltfpack KEEP flags (not compression) — dropping them merges the
100 named nodes into one anonymous node and strips the splat TEXCOORD_0/TANGENT;
the checked-in GLB loads as-is in Diligent's GLTFLoader (see notes.md `## final`); (2) core Diligent
renderer — Vulkan IRenderDevice/swapchain/one world render pass replacing
`Renderer.cpp` + pch + CMake linkage; (3) model pipeline via DiligentFX
`GLTFLoader` + `GLTF_PBR_Renderer` (replaces gltfio, keeps `gltfEntitiesNamed`
and bounding box); (4) custom HLSL terrain splat shader compiled at build time
with prebuilt glslang (replaces matc/`.filamat`); (5) ImGui via
`imgui_impl_vulkan.cpp` in a second UI render pass (never link
`libDiligent-Imgui.a`); (6) cutover — strip all filament libs/defines from both
CMakeLists, verify win32 mingw cross links, run the validation matrix.

Key constraints to enforce throughout: keep ECS/paks/input/game state untouched;
route KTX2/UASTC through the prebuilt `ktx` transcoder (Diligent's TextureLoader
can't decode KTX2); keep `ENGINE_SCREENSHOT` semantics (frame-3 one-shot JPEG) and
RenderDoc working; compare screenshots against the Phase 0 baselines
(structure-identical, PBR/tonemap deltas acceptable but flagged in notes.md).
Readback uses `CopyTexture` + CPU-access buffer (`ReadPixelsFromBuffer` does not
exist in this Diligent version) and validation runs pass `ENGINE_LOG_TIMEOUT>=300`
(it is milliseconds, see notes.md).

## Intended approach

Workers proceed phase by phase; each phase's acceptance criteria from
`plans/diligent-migration.md` are the gate before the next. Baseline screenshots
land in `/tmp/shot_diligent_baseline_*.jpg`. Every verifier round re-runs the
pinned build command (it configures/builds CMake + runs the pak pipeline);
screenshot checks via `ENGINE_SCREENSHOT` on top as each phase lands. Findings
(KTX2 answers, entity-name table shape, light/tonemap tuning values, link order)
go to notes.md as they're discovered.

Verification: ./scripts/build.sh
