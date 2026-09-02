# Migration plan: Filament → Diligent Engine

## Goal

Replace the Filament renderer in c-engine with Diligent Engine (DiligentFX), keeping
all non-render code (ECS, paks/datamanager, input, GUI widgets, game state) untouched.
Deliverables: game runs on Diligent for linux (native) and win32 (mingw cross),
`ENGINE_SCREENSHOT` and RenderDoc flows still work, visual output acceptable
(terrain splat + glTF model + ImGui menu).

## Current state

Filament touchpoints (everything else is engine-agnostic):

| File                                                             | Filament use                                                                                                                                            |
| ---------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `c-engine/renderer/Renderer.cpp/.h`                              | Engine/swapchain/renderer/scene/views/camera lifecycle, resize, screenshot readPixels, clear color                                                      |
| `c-engine/pch.h`                                                 | filament public headers                                                                                                                                 |
| `c-engine/gltf/Gltf.cpp/.h`                                      | gltfio: AssetLoader, ubershader MaterialProvider, ResourceLoader (stb/ktx2 texture providers), Animator, bounding box, entity names, ubershader archive |
| `c-engine/terrain/Terrain.cpp/.h` + `terrain.mat`                | filamat splat shader (matc-compiled to `.filamat`), MaterialInstance, 2D-array BC7 texture upload (KTX2/UASTC → BC7 via basisu)                         |
| `c-engine/gui/GuiManager.cpp`                                    | filagui `ImGuiHelper` (ImGui rendered into a 2nd Filament view)                                                                                         |
| `c-engine/ecs/.../FlyingCamera.cpp`                              | filament::math vec math, camera lookAt                                                                                                                  |
| `scripts/build-terrain.py`                                       | gltfpack `-kn -kv -ke` (meshopt string compression → gltfio decoder), manifest `material` field points at `terrain.filamat`                             |
| `c-game/game/Game.cpp`                                           | LightManager sun, IndirectLight ambient, Aabb bounds                                                                                                    |
| `c-game/game/mainMenu/MainMenuGui.cpp`, `credits/CreditsGui.cpp` | filament Texture upload (PNG), texture refs                                                                                                             |
| `c-engine/CMakeLists.txt`, `c-game/CMakeLists.txt`               | ~25 filament static libs, matc build step, FILAMENT\_\* defines                                                                                         |

Diligent side is already prepared in `/home/enes/Projects/c/cpp-thirdparty/diligent`:

- `build.sh` (linux + mingw-win cross, Vulkan-only, static-only overlay, mingw-lib shims).
- Prebuilt static libs in `git/build-linux` and `git/build-win`: DiligentCore,
  GraphicsEngine(Vk), glslang, SPIRV-Cross, SPIRV-Tools, volk, xxhash, DiligentTools
  (AssetLoader, TextureLoader, RenderStatePackager, Diligent-Imgui), **libDiligentFX.a**
  (GLTFLoader + GLTF_PBR_Renderer + GBuffer + ToneMapping + Shadows), libRadient.a.
- Supporting libs already prebuilt elsewhere: `cgltf`, `ktx` (KTX2/UASTC/basisu
  transcoding), `imgui`, `stb`, `png`, `cglm`, `sdl`.

## Approach / decisions

1. **In-place replacement on a branch** (no dual-backend flag): filament code is
   confined to the files above, the branch keeps the filament version in git history,
   and a backend flag would double the CMake + renderer complexity for no permanent
   value. Rebase/merge when the branch is green.
2. **Use DiligentFX `GLTF_PBR_Renderer` for the model pipeline** — replaces gltfio
   (loading, PBR materials, IBL, animation) without writing our own.
3. **Keep the two-view structure** (3D view + 2D UI overlay view) by owning two render
   passes on the same swapchain, matching today's translucent-overlay-on-top behavior.
4. **imgui**: use the game's thirdparty imgui with its `imgui_impl_vulkan.cpp`
   backend, compiled into c-engine. **Do not link `libDiligent-Imgui.a`** — it bundles
   its own imgui v1.85 (see risks).
5. Terrain splat shader stays **custom** (DiligentFX PBR doesn't do tile splatting):
   rewrite `terrain.mat` in HLSL, pre-compiled at build time (replace the matc step).

## Phase 0 — Prep (half day)

- Branch from main.
- Inspect `models/terrain/oghuzlands.glb`: are textures embedded PNG or external
  KTX2? (Diligent's TextureLoader decodes KTX1/PNG/JPG/DDS — **not KTX2**.) If the
  glb references external ktx2, route those through the thirdparty `ktx` transcoder
  before handing to GLTFLoader, or convert the glb textures at build time.
- **Asset pipeline (`scripts/build-terrain.py`)**: the GLB is exported with
  `gltfpack -kn -kv -ke` (meshoptimizer string-table compression for node names /
  extras / entity names — decodable by gltfio, **not** by cgltf-backed
  Diligent GLTFLoader). The glb must be re-exported without those flags (keep
  `-vpf -vn 16 -vtf` vertex quantization, keep no-`-cc`). Uncompressed node names
  then sit in plain JSON — which is what `gltfEntitiesNamed` needs. Verify by
  loading the re-exported glb in GLTFLoader/Radient before building anything else.
  Everything else in the pipeline (blender export, terrain-chunker, UV orientation
  probe, UDIM→KTX2→BC7 baking, manifest) is engine-agnostic and stays as-is.
- Note the exact list of Diligent libs + include dirs needed for linux and win
  (`build-*/DiligentCore/...`, `DiligentFX`, `DiligentTools/AssetLoader`,
  `DiligentTools/TextureLoader`, `.../RenderStatePackager`) — link order matters;
  one `--start-group` on the exe, same as today.
- Baseline screenshots of the current build for later comparison:
  `ENGINE_SCREENSHOT=/tmp/shot_diligent_baseline_{menu,world,topdown,close}.jpg`
  using `ENGINE_CAMERA=topdown|close` variants via `scripts/run.sh`.

## Phase 1 — Core Diligent renderer (1–2 days)

Files: `c-engine/renderer/Renderer.cpp/.h`, `pch.h`, `c-engine/CMakeLists.txt`,
`c-game/CMakeLists.txt`.

- Create Diligent `IRenderDevice`/`IDeviceContext` (Vulkan API; Diligent loads the
  system vulkan loader via volk — VulkanSDK is present at `/var/home/enes/Sdks/...`).
- Swapchain from the SDL3 window (SDL3 stays our windowing/input layer; Diligent
  only needs the native handle to attach its surface to): on Linux Diligent needs
  the XCB window handle (SDL3 `SDL_PROP_WINDOW_XCB_WINDOW_POINTER`; Window.cpp
  already extracts the X11 window for filament — add the xcb variant). Win32: HWND.
  Note: swapchain path is X11/XCB-based (same as today's Filament build); if the
  window were ever created under Wayland there'd be no XCB handle — force with
  `SDL_VIDEO_BACKEND=x11` if that ever comes up.
- One render pass for the world: clear color `{0.02,0.04,0.09,1.0}`, color +
  depth-stencil attachments, `ResizeSwapChain` handling in `rendererDraw` (today's
  window-resize block).
- Camera: keep the same 60° vertical fov / 0.1–20000 projection; store projection
  in a `uniforms`/`PBRRendererShaderParameters`-style struct (shared with phases 2/3).
- Screenshot: `pCtx->ReadPixelsFromBuffer` on the color texture → JPEG via stb —
  keep `ENGINE_SCREENSHOT` semantics (capture after frame 3, one-shot, `engineStop`).
- `ENGINE_RENDERDOC`/LD_PRELOAD: no code change (still Vulkan); verify a capture works.
- pch.h: swap filament headers for Diligent core headers.

**Acceptance:** build + run shows a window with the correct clear color, resizes
correctly, `ENGINE_SCREENSHOT` writes the clear-color JPEG on both platforms.

## Phase 2 — GLTF model + PBR via DiligentFX (1–2 days)

Files: `c-engine/gltf/Gltf.cpp/.h`, `c-game/game/Game.cpp`.

- Load `oghuzlands.glb` bytes (from the pak, as today) into `GLTFLoader`
  (DiligentTools/AssetLoader) with the resource cache; wire textures through
  Diligent's `ITextureLoader` (PNG/JPG embedded is fine; see Phase 0 KTX2 check).
- Create `GLTF_PBR_Renderer` bound to our swapchain RT/DSV formats; `Render()` the
  scene each frame with camera (view/proj) + light params.
- Sun (directional, from `Game.cpp` direction/color/intensity) + ambient: express
  ambient via the PBR renderer's IBL slot with a constant environment
  (`{0.32,0.35,0.38}` at the current intensity ratio).
- Animation: `GLTFLoader::UpdateAnimation(0, 0, t, transforms)` each frame
  (replaces `Animator::applyAnimation` + `updateBoneMatrices`).
- Bounding box: use the loader's bounding-box option to re-derive the `Aabb`
  consumed by `gltfFrameCamera` / `Game.cpp` camera modes.
- Names: `gltfEntitiesNamed(prefix,...)` must keep working — walk the loaded model's
  node hierarchy, build a name→entity table (or a simple parallel array) so game code
  can still select meshes by name.
- Destroy path in `gltfDestroy` mirrors today's (loader/renderer resource release).

**Acceptance:** `./scripts/run.sh` enters world; terrain glb renders with PBR
lighting; animation runs; screenshot (world/topdown/close modes) visually matches
the filament baseline (PBR/tonemap differences are acceptable but flagged).

## Phase 3 — Terrain splat shader (1–3 days)

Files: `c-engine/terrain/Terrain.cpp/.h`, `terrain.mat` (→ new HLSL source),
`c-engine/CMakeLists.txt` (replace matc step), `scripts/build-terrain.py`
(only: gltfpack flags in `buildModel()` and the manifest `material` field),
`scripts/ktx2bc7.c` + `build-terrain.py` texture stages (unchanged — still emit
KTX2/BC7 the same way).

- Write the splat shader as HLSL: same inputs as `terrain.mat` (5 sampler2dArrays:
  splatTiles/styleAlbedo/styleNormal/defaultAlbedo/defaultNormal; int[100]
  tileLayer0..2; styleRemap; sand/snow height+fade params; sun + ambient lighting to
  match Phase 2's model lighting so terrain and model read as one scene).
- Compile it at **build time** (custom CMake command) with the prebuilt glslang
  (HLSL→SPIR-V) from `diligent/git/build-linux/.../ThirdParty/glslang` — replaces
  `matc` + `terrain.filamat` in `c-game/data/pak_1/materials/`; update the
  manifest's `"material"` field in `build-terrain.py` to the new artifact name.
  The UV handling must keep the `v=one-minus/u=same` behavior that
  `probeUvOrientation()` asserts. (Alternatively
  runtime-compile once via Diligent's ShaderToolchain; build-time is preferred:
  no runtime deps, same UX as today's matc step.)
- PSOs: 1–2 pipeline states (opaque terrain; wireframe for debug if wanted).
- Geometry: draw the splat shader over the **same vertex buffers** GLTFLoader
  uploaded for the model (positions/normals/uv), with a custom
  `IShaderResourceBinding` (splat arrays + params). Terrain init still reads the
  same `oghuzlands.json`; the KTX2→BC7 2D-array assembly in `Terrain.cpp` ports
  almost as-is (upload via Diligent `ITextureLoader`/`CreateTexture` from BC7 blocks).
- Keep `terrainApplyToAsset` semantics: terrain replaces/augments the model's
  material for the terrain meshes (same as today via MaterialInstance).

**Acceptance:** topdown screenshot shows correct splat tiles, sand/snow fades,
style blending — visually equivalent to the filament baseline shot.

## Phase 4 — GUI (1–2 days)

Files: `c-engine/gui/GuiManager.cpp/.h`, `c-game/game/mainMenu/*`.

- Replace `filagui::ImGuiHelper` with `imgui_impl_vulkan.cpp` (from thirdparty
  imgui, compiled into c-engine):
  - init on the swapchain device/queue + UI render pass;
  - per frame: `NewFrame` → font scale (keep `guiScale` behavior) → `feedInput`
    (unchanged) → guis draw → `Render` → submit the UI draw data into the UI pass
    (translucent, blended over the 3D pass — same as today's 2nd Filament view).
- UI render pass: same extent, no depth writes, blend enabled, drawn after the
  world pass; skip entirely when `guiIsActive()` is false (keep the fast path).
- `MainMenuGui`/`CreditsGui` texture upload: PNG via stb → Diligent texture
  (replaces `filament::Texture::Builder` + PixelBufferDescriptor); font loading
  from the pak is unchanged (pure ImGui).
- Watch for the Diligent-Imgui pitfall (risks #1).

**Acceptance:** main menu renders with correct fonts/scale at 720p and 4K, buttons
work, menu→world transition hides the UI pass, ESC back-to-menu still works.

## Phase 5 — Cutover & cleanup (half–1 day)

- `c-engine/CMakeLists.txt` / `c-game/CMakeLists.txt`: remove all filament include
  dirs, defines (FILAMENT*SUPPORTS*\*, GLTFIO_DRACO_SUPPORTED), FILAMENT_LIBS,
  filagui, matc step (replaced in Phase 3); add Diligent include dirs + libs for
  both platforms (path layout differs: `build-linux` vs `build-win` + `mingw-lib`).
- Win32 cross: link the mingw-built Diligent libs; verify no new system DLL
  requirements (Vulkan loader DLL expected on win — acceptable, same as before).
- Rename window title in `Engine.cpp`/`rendererInit` off "filament-game"
  (optional, cosmetic).
- `scripts/build.sh`: update if it references filament artifacts.

## Phase 6 — Validation matrix

All of these, on linux native and win32 cross:

- [ ] `./scripts/run.sh` boots to main menu (clear color + ImGui).
- [ ] ENTER WORLD → terrain + model render, animation, fly camera.
- [ ] `ENGINE_SCREENSHOT` + `ENGINE_CAMERA=topdown|close` shots compared against
      Phase 0 baseline (color/lighting deltas acceptable, structure identical).
- [ ] `ENGINE_LOG_TIMEOUT` automated run exits cleanly (no leaks/crashes in teardown
      — verify `rendererDestroy` order: UI pass → world pass → swapchain → device).
- [ ] RenderDoc capture (LD_PRELOAD) opens a frame.
- [ ] Window resize mid-game: viewport + projection + ImGui scale all track.

## Risks / gotchas

1. **Two imgui copies**: `libDiligent-Imgui.a` bundles imgui v1.85; the game uses its
   own (newer) thirdparty imgui. Linking both = symbol clashes. Don't link
   Diligent-Imgui; if an FX object drags in imgui symbols (DebugView etc.),
   rebuild `DiligentTools/Imgui` pointing `DILIGENT_DEAR_IMGUI_PATH` at the game's
   imgui instead of shipping two copies.
2. **KTX2**: Diligent's TextureLoader doesn't decode KTX2/UASTC. glb model textures
   and/or terrain arrays must go through the prebuilt `ktx` lib (basisu) like today.
   Phase 0 determines exactly where this bites.
3. **GLB string compression**: `build-terrain.py` currently runs gltfpack with
   `-kn -kv -ke` (meshoptimizer string-table format). Diligent's GLTFLoader (cgltf)
   can't decode it — re-export without those flags, keep vertex quantization, keep
   no-`-cc` (no KHR_meshopt_compression: neither gltfio nor the Diligent loader
   would decode the buffer stream).
4. **Look mismatch**: DiligentFX PBR + tone mapping won't be pixel-identical to
   Filament's uber-shader. Expect to tune light intensity/tonemap for terrain +
   model consistency; treat the Phase 0 baseline as reference, not target.
5. **Diligent boilerplate**: render passes, PSO creation, resource bindings,
   frame resource management are more verbose than Filament — budget the Phase 1
   learning time honestly; the sample code in
   `diligent/git/DiligentSamples/SampleBase` (and DiligentFX PBR samples) is the
   reference to crib from.
6. **Entity mapping**: Filament was entity-centric (scene.addEntity per gltf
   entity); Diligent is buffer-centric. The ECS entity ids must be re-mapped onto
   loaded GLTF meshes (needed for `gltfEntitiesNamed` and terrain apply).
7. **Win32 static-only build quirks** are already solved (`static-only.cmake`,
   `mingw-lib` shims) — just reuse the documented flags from
   `diligent/build.sh`, don't re-derive them.
