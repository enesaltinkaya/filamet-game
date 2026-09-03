# Phase 6 recon: mirroring the Filament terrain pass into the Diligent backend

Round-1 recon for plans/azgaar-terrain.md phase 6. Everything below was verified
against the sources at baseline e547d56.

## 1. Files to create / modify

Create:

- `c-engine/renderer/diligent/HeightmapTerrainDiligent.h` — public API mirroring
  `c-engine/renderer/filament/HeightmapTerrainFilament.h` 1:1, renamed:
  `heightmapTerrainDiligentInit/Update/RegisterLook/ReleaseLook/SetDebugView/Stats/Destroy`.
- `c-engine/renderer/diligent/HeightmapTerrainDiligent.cpp` — the pass body
  (mirror map in §3).
- `c-engine/renderer/diligent/shaders/heightmap_terrain_vs.hlsl` +
  `heightmap_terrain_ps.hlsl` (names free; see §5 for the compile step).

Modify:

- `c-engine/ecs/system/heightmap/HeightmapTerrainRender.cpp` — dispatcher:
  every one of the six entry points is currently
  `if (renderer::rendererBackend() == renderer::Backend::Filament) { filament half }`
  (`registerLook` and `stats` even carry a `// phase 6: diligent half` comment
  placeholder). Add the Diligent branch to each; include the new header.
- `c-engine/renderer/diligent/DiligentRenderer.cpp`:
  - `draw()` (line 150): insert `heightmapTerrainRenderUpdate()` at the top
    (mirrors `FilamentRenderer.cpp:94`, called before anything else in draw).
  - after `worldDraw(context);` (line 184): the terrain draw hook
    (record the tile draws on `context`, must call `setWorldDrew(true)` when it
    drew — see §6 for why).
  - `destroy()` (line 271): call `heightmapTerrainRenderDestroy()` first
    (mirrors `FilamentRenderer.cpp:131` — terrain GPU state lives in the device
    that is about to be released; it currently starts with
    `guiOnBackendDestroy()`).
- `c-engine/CMakeLists.txt`:
  - add `renderer/diligent/HeightmapTerrainDiligent.cpp` to the
    `SKIP_PRECOMPILE_HEADERS` list (lines 178–182; the diligent TUs skip the
    shared pch because of heavy Diligent headers).
  - shader compile step (see §5).
- Game side (`c-game/game/Game.cpp`: registerLook ~209, setDebugView ~221,
  releaseLook ~399/412): **no changes** — they call the backend-agnostic
  dispatcher only.

## 2. Shared CPU state both halves read (unchanged, no porting needed)

- `c-engine/ecs/system/heightmap/HeightmapLattice.h`:
  `heightmapLatticeCornerCount()` (256²), `heightmapLatticeIndexCount()` (6·255²),
  `heightmapLatticeBuildIndices(u32*)`, `heightmapLatticeBuildCorners(const
  float* heights, originX, originZ, sizeMeters, HeightmapLatticeCorner*)`.
  Winding is CCW from above → PSO cull BACK with Vulkan's
  `FrontCounterClockwise = true` (same as GLTF_PBR_Renderer).
- `c-engine/ecs/system/heightmap/HeightmapTerrain.h`:
  `HeightmapTerrain* heightmapTerrainGetActive()` (has `initialized`,
  `windowSize`); `heightmapTerrainSnapshotTiles(ht, out, cap)` returning
  `HeightmapTileView{tileX, tileZ, readyStamp, originX, originZ, sizeMeters,
  heights[HEIGHTMAP_TEX²]}`; `heightmapWorldToTileCoord(ht, coord)`;
  `ht->windowSize * ht->windowSize` is the tile pool cap (≤ 25 today).
- Camera for nearest-ring ordering: `renderer::rendererCameraGet`
  (backend-agnostic, Renderer.cpp) — the Filament pass already uses it, so the
  Diligent pass uses the same call.

## 3. Mirror map: HeightmapTerrainFilament.cpp → Diligent

The Filament file (686 lines) is organized as: constants/statics → texture
helpers → initPass → look (register/release/apply) → tile cache (GpuTile pool,
deferred-destroy list) → updateImpl (deferred tick, cache-drop, ring sort,
budgeted uploads) → destroy → stats. Keep the exact same structure; the
state that changes type is:

| Filament | Diligent |
| --- | --- |
| `filament::Material` + `MaterialInstance` (filamat blob from `dataManagerRead("materials/heightmap_terrain.filamat")`) | one `IPipelineState` + one template `IShaderResourceBinding` (built in init; §4/§5) |
| `filament::Texture*` defaults ×6, biome/climate/fallback ×3 | `RefCntAutoPtr<ITexture>` ×8 (§7) |
| `filament::VertexBuffer` per tile (pos3 + tangent quat4, 28 B stride) | `RefCntAutoPtr<IBuffer>` per tile, (pos3, normal3) = 6 floats, 24 B stride. **No tangent attribute** — the PS rebuilds the TBN from the normal (terrain.mat's `buildTerrainTBN` pattern), so the VS is a pure transform |
| `filament::IndexBuffer` shared | `RefCntAutoPtr<IBuffer>` shared IBO (USAGE_IMMUTABLE, `UpdateBuffer` once) |
| `scene->addEntity(entity)` renderable | nothing to add — the draw hook (in DiligentRenderer.cpp or exposed as `heightmapTerrainDiligentDraw(ctx)` from the new file) iterates in-use tiles and records `DrawIndexedAttribs` per tile |
| `engine->destroy(...)` | `RefCntAutoPtr.Release()`; keep the 3-frame `kDeferredDestroyFrames` list because in-flight command buffers still reference the buffers |
| `utils::Entity` per tile | not needed (no ECS) |

Verbatim carries (do not re-derive): `kUploadsPerFrame = 3`,
`kDeferredDestroyFrames = 3`, `readyStamp` as the cache key
(`gpuTileHasView` compares tileX/tileZ/readyStamp), pool cap
`ht->windowSize * ht->windowSize`, the (Manhattan-ring, view-order) insertion
sort, "failed upload doesn't consume budget, retry next frame", the
`cachedHt != active → destroyAllTiles()` change detection, and the stats
machine (120-frame warmup `kStatWarmupFrames`, 1000-frame average
`kStatFrames`; `gpuBytes` = corners×6×sizeof(float) per resident tile +
idxCount×sizeof(u32) — 6 floats, not 7, per corner here).

Upload path (per tile, main thread, inside the draw frame):
`heightmapLatticeBuildCorners` into a static scratch → repack to
`float[65536][6]` on the heap (Diligent's `UpdateBuffer`/`CreateBuffer`
*copies* the source, so per-upload `new float[]` + `delete[]` after
`UpdateBuffer` is fine — the Filament zero-copy lifetime lesson does not
apply) → `device->CreateBuffer(USAGE_IMMUTABLE, BIND_VERTEX_BUFFER, size,
initData, &vbo)` (immutable: `USAGE_DYNAMIC` maps share the per-frame ring
with every other dynamic mapping and a one-shot write gets clobbered —
docs/lessons.md splat-terrain incident) → build the tile's SRB from the
template (clone or fresh `CreateShaderResourceBinding(pSRB, true)`).

Draw (per frame, after worldDraw): for each in-use tile:
`context->SetPipelineState(terrainPSO)` (or set once if one PSO),
`context->SetShaderResourceBinding(tileSRB)`,
`context->DrawIndexedAttribs({pIB = latticeIbo, IB_FORMAT_UINT,
IndexCount = heightmapLatticeIndexCount()})` — render targets/depth are
already bound by `draw()` (swapchain RTV + DSV, depth test/write must come
from the PSO's DepthStencilDesc: enable test + write,
`DepthCompare = COMPARE_FUNC_LESS`), then `setWorldDrew(true)`.

## 4. PSO / SRB / shader pattern (from GltfDiligent.cpp)

The repo's only hand-rolled-PSO reference is inside DiligentFX
(GLTF_PBR_Renderer creates PSOs; GltfDiligent.cpp wires it). Concrete
patterns to copy:

- **PSO create** (GltfDiligent.cpp `gltfInitDiligent`, lines ~61–75):
  `GraphicsPipelineDesc{ RTVFormats[0] = swapChain->GetDesc().ColorBufferFormat
  (RGBA8_UNORM_SRGB), DSVFormat = swapChain->GetDesc().DepthBufferFormat
  (D32_FLOAT), PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  FrontCounterClockwise = true, PackMatrixRowMajor = true,
  RasterizerDesc.CullMode = CULL_MODE_BACK,
  DepthStencilDesc{DepthEnable = true, DepthWriteEnable = true}}`.
  The swapchain was created with those formats (DiligentRenderer.cpp
  `init()`), so the terrain PSO must use the same RT/DSV formats.
- **cbuffer** (GltfDiligent.cpp line 68):
  `CreateUniformBuffer(device, size, name, &cb)` (DiligentFX Utilities).
  Per-frame remap via `MapHelper<PBRFrameAttribs> frame(ctx, cb, MAP_WRITE,
  MAP_FLAG_DISCARD)` (GltfDiligent.cpp `fillFrameAttribs`, lines ~250–300).
  The terrain pass gets its *own* frame-attrs buffer sized
  `sizeof(HLSL::PBRFrameAttribs) + sizeof(HLSL::PBRLightAttribs)` and fills
  the identical values: view/proj from `diligentFrameView()/diligentFrameProj()`
  (D3D-style LH view + projection — the matrix convention is already
  backend-local to DiligentRenderer), `PBRRendererShaderParameters`
  (AverageLogLum 0.25, MiddleGray 0.18, WhitePoint 3.0, IBLScale 1,
  LightCount 1, …), sun via `GLTF_PBR_Renderer::WritePBRLightShaderAttribs`
  (or a direct `PBRLightAttribs` fill — same fields; note the sun intensity
  scaling `* 3.0/110000.0` from physical lux to the PBR shader range).
- **PBR includes**: GltfDiligent.cpp lines 28–34 pulls
  `#include <Shaders/Common/public/BasicStructures.fxh>`,
  `<Shaders/PBR/public/PBR_Structures.fxh>`,
  `<Shaders/PBR/private/RenderPBR_Structures.fxh>` inside
  `namespace Diligent::HLSL` for the cbuffer structs. The terrain PS needs the
  *lighting functions* too: `Shaders/PBR/public/PBR_Shading.fxh`
  (1036 lines: `ApplyDirectionalLightGGX`, `GetLambertianIBL`,
  `GetSpecularIBL_GGX`, `ResolveLighting`, `PerturbNormal`, …) — the include
  root is `${diligent_git}/DiligentFX` (already in CMakeLists lines 105–107:
  `DiligentFX`, `DiligentFX/PBR/interface`, `DiligentFX/Utilities/interface`),
  so `#include <Shaders/PBR/public/PBR_Shading.fxh>` resolves in our HLSL.
- **IBL cubes** (GltfDiligent.cpp `makeConstantCube` + `gltfIblUpdateDiligent`):
  two 1×1×6 `RGBA8_UNORM` `USAGE_IMMUTABLE` cubes (irradiance + single-mip
  prefiltered env) from the ambient color/intensity,
  `k = max(0, intensity) * 1.2f * exp2f(-15.0f) * 0.318309886f` (1/π),
  per-component clamp to 1.0, then explicit
  `UNKNOWN → SHADER_RESOURCE` transitions (immutable textures start in
  transfer layout). The terrain pass re-creates its own pair from
  `diligentAmbientColor()/diligentAmbientIntensity()` — hook: the
  `DiligentBackend::setAmbient` override (DiligentRenderer.cpp ~line 330)
  already forwards to `gltfIblUpdateDiligent`; add a parallel call to the
  terrain half (or read the accessors at init/update — the accessors exist
  exactly for this, so an init-time + on-change rebuild suffices; the ambient
  only changes when the game sets weather/lighting).
- **SRB**: one template SRB (slot layout up to us; declare vertex-buffer
  slot, index-free — IBO is passed at draw — texture slots for
  grassAlbedo/grassNormal/cliffAlbedo/cliffNormal/snowAlbedo/sandAlbedo/
  biomeColor/climate/climateNearest + 2 IBL cubes + samplers + frame-attrs
  CBVR), `CreateShaderResourceBinding(pTemplateSRB, true)` per tile. The
  frame-attrs CBVR and IBL views are shared across tiles; the per-tile part
  is only the VBO.
- **destroy**: release tiles → IBO → look textures → fallback → PSO → SRB
  template → frame buffer; all `RefCntAutoPtr`, no engine object to call
  destroy on.

## 5. Shader compile: build-time vs runtime (deviation note)

The plan says "pre-compiled at build time, HLSL → SPIRV via the glslang
chain". Verified against the actual chain (c-engine/CMakeLists.txt lines
119–129):

- **glslang alone does not parse HLSL** — its front end is GLSL only. The
  linked statics (`libglslang.a`, `MachineIndependent`, `SPIRV`,
  SPIRV-Tools/Cross) can only compile GLSL source to SPIRV. So a
  build-time tool that compiles *HLSL* must link
  `libDiligent-ShaderTools.a` (+ `HLSL2GLSLConverterLib`, both already in
  DILIGENT_LIBS) — i.e. a small build-only exe that calls
  `diligent::CreateCompiler()...HLSL2SPIRV` (the same statics
  `IDevice::CreateShader` uses internally).
- **The repo already compiles HLSL at runtime, on every diligent boot**:
  DiligentFX's PBR_Renderer.cpp (thirdparty source, prebuilt into
  libDiligentFX.a) creates its PSOs via `m_Device.CreateShader(ShaderCI)`
  with `pShaderSourceStreamFactory =
  &DiligentFXShaderSourceStreamFactory::GetInstance()` (embedded
  .vsh/.psh/.fxh stream factory, declared in
  `DiligentFX/Utilities/interface/DiligentFXShaderSourceStreamFactory.hpp` —
  include dir already in CMake). So a one-time runtime compile of the
  terrain VS/PS at pass init (~tens of ms, same mechanism) is the
  established in-process precedent with zero new build machinery.

Recommendation: start with runtime `device->CreateShader` (ShaderCI:
`SourceLanguage = SHADER_SOURCE_LANGUAGE_HLSL`,
`pShaderSourceStreamFactory = &DiligentFXShaderSourceStreamFactory::
GetInstance()`, our .hlsl files' contents as inline `Source` with the
DiligentFX fxh `#include`s, entry `main`, `SHADER_TYPE_VERTEX` /
`SHADER_TYPE_PIXEL`) and keep the build-time SPIRV step as a follow-up
optimization only if the init-time cost shows up in the logs. Document this
deviation from the plan in the final round notes.

## 6. Backend selection + call-site inventory

- Selection: `c-engine/renderer/Renderer.cpp` `selectBackend()` —
  `ENGINE_RENDERER` env (`filament`|`diligent`, case-insensitive via
  strequals) overrides the persisted `settingsGetInt("rendererBackend")`
  (1 = diligent, default filament). `rendererBackend()` (public,
  renderer/Renderer.h) is what the dispatcher switches on.
- Dispatcher call sites (all in `HeightmapTerrainRender.cpp`, main thread):
  `registerLook` (game world load, Game.cpp:209), `releaseLook`
  (Game.cpp:399/412), `setDebugView` (Game.cpp:221), `update`
  (render-thread-ish: called from the backend `draw()` —
  FilamentRenderer.cpp:94 at the very top), `stats` (game/system logging),
  `destroy` (backend teardown — FilamentRenderer.cpp:131, first thing).
- `setWorldDrew` semantics (DiligentRenderer.h line 24–27): the UI pass LOADs
  over the world only when the world pass drew; today only the glTF hook
  sets it. A world with terrain but no .glb would otherwise CLEAR the UI
  pass → the terrain draw hook must call `setWorldDrew(true)` when it
  recorded any tile draw.

## 7. Texture load path (Diligent)

- **KTX2 → BC7**: `utils::imageLoadKtx(path, KTX_FORMAT_BC7_RGBA)`
  (c-utils/image/Image.cpp:83) — the process's single BasisU copy
  (`thirdparty ktx`, UASTC transcode via `ktxTexture2_TranscodeBasis`).
  Returns `utils::Image{data (BC7 bytes, all mips packed), size, width,
  height, channels=4, mips, mipSizes, vkFormat}`. **Pitfall: `mipSizes`
  holds per-mip *offsets* (`ktxTexture_GetImageOffset`), not sizes** — and
  nothing in the repo consumes them today. For `TextureData` init, compute
  each mip's size/stride yourself: BC7 RGBA mip i = `ceil(w/2^i)·ceil(h/2^i)
  · 8` bytes (w,h ≥ 1).
- Pak paths (pak_1, same as the Filament `kDefaultTextures` table in
  HeightmapTerrainFilament.cpp lines 55–62):
  `images/terrain/{grass_default,cliff_side_default,snow_default,
  sand_default}/{albedo,normal}.ktx2` (snow/sand ship albedo only).
  Load with `dataManagerRead` (the ktx loader reads the pak via
  `imageLoadKtx` → `dataManagerRead` internally — just pass the pak path).
- **Formats**: albedo → `TEX_FORMAT_BC7_UNORM_SRGB` (restores the sRGB
  decode of the Filament `SRGB_ALPHA_BPTC_UNORM` slot), normal + snow/sand?
  → `TEX_FORMAT_BC7_UNORM` (normals linear; snow/sand albedos SRGB).
  All `USAGE_IMMUTABLE`, `BIND_SHADER_RESOURCE`, `MipLevels = image.mips`
  (11), init data via `TextureSubResData` per mip (data pointer
  `image.data + offset`, stride = row size of that mip), then the
  `UNKNOWN → SHADER_RESOURCE` transition pair from `gltfIblUpdateDiligent`.
- **RGBA8 look textures** (from the `HeightmapTerrainLook` packed pixels,
  same as Filament's `createRgba8`): `biomeColor` →
  `RGBA8_UNORM_SRGB`, `climate` → `RGBA8_UNORM` (R/G/B byte-encoded scalars
  are linear by design), both 1 mip, `USAGE_IMMUTABLE` (they are replaced,
  never updated in place — release/recreate per registerLike call; the
  Filament side destroys + rebuilds in releaseLook/registerLook, keep that).
  A-channel biome id needs a NEAREST sampler bound to the *same* climate
  texture (`climateNearest` slot in the .mat), so the SRB carries two
  sampler slots for one texture.
- **Fallback**: 1×1 white `RGBA8_UNORM` for all eight slots before a world
  look is registered (Filament has `fallbackTex`; do the same).
- **Samplers** (docs/lessons.md 2026-09 — no plain LINEAR tiling samplers):
  tiling = `SamplerDesc{MinFilter = FILTER_MIN_LINEAR_MIPMAP_LINEAR,
  MagFilter = FILTER_MAG_LINEAR, WrapU/WrapV = WRAP_MODE_REPEAT,
  MaxAnisotropy = 16}` for the six default terrain textures;
  linear-clamp for biome/climate; nearest-clamp for climateNearest.
  Create via `device->CreateSampler(SamplerDesc)` (the imgui gui pass uses
  raw vkCreateSampler — do not copy that; the terrain path is pure
  Diligent API).

## 8. VBO layout + input layout (the one genuinely new struct)

- Vertex: `float3 pos @ 0`, `float3 normal @ 12`, stride 24, no per-vertex
  attributes beyond those. `InputLayout` with two attribute layouts
  (VTF/TF slots 0 and 1) + the buffer layout; VS is
  `worldPos = mul(float4(pos,1), viewProj)` + pass normal/worldPos to the PS.
  (The DiligentBackend's view/proj are D3D-LH; the VS gets viewProj from the
  shared frame-attrs cbuffer, so no separate uniform is needed.)
- PS = the full `terrain.mat` port: constants from the .mat
  (AZGAAR_GRASS_TILE 2048/7000, CLIFF_DETAIL_TILE 32,
  SPLAT_NORMAL_STRENGTH 2.0, MICRO_NOISE_STRENGTH 0.45,
  AZGAAR_CLIMATE_TEMP_BIAS 64, Glacier id 11, roughness 0.9, grazing
  `smoothstep(0.05,0.30, geomNdotV)`, land mask `smoothstep(0,0.2,y)`,
  sea level y=0), params =
  `{mapBounds: float4, climateParams: float4, maxLandHeight: float,
  debugView: float}` (+ the 9 texture slots incl. climateNearest).
  Output through the same tone-map constants as the glTF PBR frame
  (AverageLogLum/MiddleGray/WhitePoint) so terrain + model read as one scene.
  Debug views 0/1/2 (off / 256 m height ramp / raw biome texture) must match
  the .mat's `debugView` block (terrain.mat lines ~381–400) for the
  phase-6 acceptance screenshots.

## 9. Verification surface

- Build: `./scripts/build.sh` (note: run.sh-style scripts need TERM set).
- Screenshot: `ENGINE_RENDERER=diligent ENGINE_SCREENSHOT=/tmp/... ./build/
  c-game/c-game` — ramp debug view first (seam check), then land look, then
  a streaming/moving run. Compare against `docs/azgaar-terrain/
  {terrain-land-look,terrain-seam-ramp,terrain-default}.jpg` (phase 5
  references).
