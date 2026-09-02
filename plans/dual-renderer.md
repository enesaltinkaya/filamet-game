# Dual renderer: Filament + Diligent side by side

Follow-up to `diligent-migration.md`: instead of replacing Filament, the engine keeps
**two render paths** — `filament` (default) and `diligent` — selected at startup by a
setting (env var `ENGINE_RENDERER=filament|diligent` overrides the persisted
`rendererBackend` setting: 0 = filament, 1 = diligent). Everything else (ECS, paks,
input, game state, ImGui widgets) stays backend-agnostic.

## Layout

```
c-engine/renderer/
  Renderer.h            public, backend-agnostic API (no filament/diligent types leak out)
  Renderer.cpp          backend selection (env > settings), window, screenshot/renderdoc
                        bookkeeping, dispatch to the active backend
  RenderBackend.h       internal: small interface both backends implement
  filament/FilamentRenderer.{h,cpp}   old Renderer.cpp body (globals stay here)
  diligent/DiligentRenderer.{h,cpp}   device/context/swapchain, frame loop, resize,
                                      screenshot readback, sun/ambient storage
c-engine/gltf/
  Gltf.{h,cpp}          dispatch + shared pak read; ids are u64 handles
  GltfFilament.cpp      gltfio (unchanged behavior)
  GltfDiligent.cpp      Diligent GLTF::Model + GLTF_PBR_Renderer, names, bbox, anim,
                       world draw hook called by the diligent backend between pass
                       begin/end
c-engine/terrain/
  Terrain.{h,cpp}       manifest parse + KTX2→BC7 decode (backend-agnostic), dispatch
  TerrainFilament.cpp   .filamat material + texture arrays (unchanged behavior)
  TerrainDiligent.cpp   embedded HLSL splat shader (runtime HLSL→SPIRV via Diligent
                       ShaderTools/glslang — replaces matc for this backend), BC7
                       texture arrays, PSO + SRB, per-chunk draw
c-engine/gui/
  GuiManager.cpp        fonts/input/scale (unchanged) + backend hooks:
                       frame(cb), texture create/destroy, UI pass draw
  GuiFilament.cpp       filagui ImGuiHelper
  GuiDiligent.cpp       imgui_impl_vulkan on the Diligent Vk device/queue
```

## Notes / decisions

- The terrain GLB (oghuzlands.glb) has **no textures and plain node names** — the
  migration-plan worry about meshopt string tables / KTX2 in the glb does not apply.
  `build-terrain.py` needs no changes.
- Terrain material for diligent is an HLSL string compiled once at terrain init
  (runtime compile via the engine; the plan's acceptable alternative to a build step).
  The filament path keeps `matc` + `terrain.filamat`.
- When every mesh of the loaded model is a `terrain_chunk_*` (current data: 100/100),
  the diligent path draws the whole model with the splat PSO. If a model has non-terrain
  meshes, GLTF_PBR_Renderer renders the whole model instead (per-mesh mixing is not
  supported by `GLTF_PBR_Renderer::Render`).
- Both backends link into one binary (namespaced, no symbol clashes: imgui is a single
  copy, Diligent-Imgui is NOT linked, volk pointers don't collide with bluevk).
- `rendererBackend` setting template added to c-utils settings (0/1).

## Status

- [x] Baseline screenshots captured (filament, menu + topdown world)
- [x] Backend abstraction + filament path refactored behind it
- [x] Diligent core: device/swapchain/clear/resize/screenshot
- [x] Diligent gltf: GLTF::Model from pak (ReadWholeFileCallback), PBR renderer,
      sun/ambient, names/bbox/animation
- [x] Diligent terrain: HLSL splat shader + BC7 arrays
- [x] Diligent gui: imgui_impl_vulkan + pak PNG textures
- [x] Validation: both backends boot to menu, enter world, screenshot, teardown
