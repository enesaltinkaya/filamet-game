# DiligentFX SSAO — implementation plan

Add DiligentFX's `ScreenSpaceAmbientOcclusion` (prebuilt `libDiligentFX.a`) to the
Diligent render path: a world-space normal buffer in the offscreen chain, an SSAO
compute pass on the existing `PostFXContext`, and AO multiplied into the lit world
color before the TAA resolve hits the backbuffer. Reference:
`/home/enes/Projects/c/cpp-thirdparty/diligent/git/DiligentFX/PostProcess/ScreenSpaceAmbientOcclusion`
(README, interface, 1342-line src), Tutorial27 (integration shape), Hydrogent
(`HnPostProcessTask.cpp` — the full-engine wiring to mirror).

## Current state (verified in-tree)

Already ready — the integration surface is ~90% built:

- c-engine already runs TAA through DiligentFX `PostFXContext` + `TemporalAntiAliasing`
  — the exact Tutorial27 shape. `postFXContext->PrepareResources` every frame at
  render-scale size (TaaDiligent.cpp:843-849); `postFXContext->Execute(pa)` + depth
  SRVs + `taa->Execute` inside `taaWorldResolve` (TaaDiligent.cpp:1121-1145).
- The D32 world depth already carries `BIND_SHADER_RESOURCE` (TaaDiligent.cpp:726-731)
  and is sampled as an SRV by TAA reprojection today (TaaDiligent.cpp:1125) —
  D32-as-SRV on Vulkan is proven, SSAO reads the same SRV.
- The offscreen chain is `sceneColorTex` RGBA16F (692-700), `motionTex` RG16F
  (702-705), `depthTex[2]` D32 (726-731), sized by `createTargets` (682-741) at
  `swapchainSize * renderScale` (832-835); world pass binds 2 RTVs + DSV
  (DiligentRenderer.cpp:479-484).
- c-game links `libDiligentFX.a` once inside `--start-group`
  (c-engine/CMakeLists.txt:93-126 `DILIGENT_LIBS` PARENT_SCOPE → c-game/CMakeLists.txt:11,64);
  `nm` on the prebuilt archive confirms `Diligent::ScreenSpaceAmbientOcclusion::{ctor,
PrepareResources, Execute, GetAmbientOcclusionSRV}` are exported.
- An unconsumed `ssao` settings flag already flows end-to-end: `GraphicsSettings.ssao`
  (Renderer.h:19), loaded from `aoDisabled` (Renderer.cpp:234), persisted key
  `aoDisabled` (c-utils/settings/Settings.cpp:53), toggled from the debug overlay
  (`c-engine/gui/rmlui/guis/DebugGui.cpp:65,103`) and the graphics settings page
  (c-game/game/settingsGui/graphics/SettingsGraphicsGui.cpp:31,116,411-415) —
  but `DiligentRenderer::applyGraphicsSettings` (DiligentRenderer.cpp:801-804)
  only forwards TAA/CAS/renderScale. Nothing in the render path reads `g.ssao`.

The two gaps this plan closes:

1. **No normal buffer anywhere.** SSAO's second mandatory input is a world-space
   normal texture (Hydrogent feeds GBuffer `GBUFFER_TARGET_NORMAL`,
   HnPostProcessTask.cpp:294/829; format `RGBA16_FLOAT`, HnBeginFrameTask.cpp:66).
   We add it as a 3rd RTV on the existing 2-RTV world pass, with a normal output in
   all three world pixel shaders (glTF PBR "player", heightmap terrain, props).
2. **No AO application point.** The AO texture (R8, 1:1 with the offscreen color)
   must be multiplied into the lit color after TAA accumulation, before the
   downsample/CAS/blit to the backbuffer.

## Approach / decisions

1. **Thin `SsaoDiligent` module on the existing `PostFXContext`** — no second
   context, no new frame machinery. `ScreenSpaceAmbientOcclusion::PrepareResources`
   reads the context's `FrameDesc`/feature flags (SSAO cpp:61-84) and its
   temporal-accumulation stage binds the context's reprojected-depth / previous-depth
   / closest-motion textures (SSAO cpp:1057-1059). Both TAA and SSAO consume the
   same shared `PostFXContext` that Tutorial27/Hydrogent do.
2. **Normal buffer = one more RTV on an already multi-target pass.**
   `normalTex` RGBA16F (RGB16F is not a Vulkan RT format; Hydrogent uses RGBA16F for
   the identical contract, HnBeginFrameTask.cpp:66), cleared to (0,0,0,0) — the
   sky/background then reads as an upward-degenerate normal and far-plane depth
   yields AO≈1, so the composite is a no-op there. Per-shader, the world-space
   normal is already in scope in all three world PS (see phase 1) — no new
   per-pass computation.
3. **AO is applied AFTER TAA accumulation, not into the TAA input.** SSAO owns its
   own temporal history (16-frame history-length textures, `SSAO_MAX_HISTORY_LENGTH`
   in the fxh:57, internal motion/disocclusion handling via the context's
   closest-motion texture). Stacking AO _before_ TAA would make TAA's
   variance-rejection fight AO disocclusion edges (ghosting + motion-reprojection
   smear). Placing the multiply after `taa->Execute` is the standard two-filter
   placement and a single 1:1 composite pass.
4. **SSAO `Execute` runs inside `taaWorldResolve`, AFTER `postFXContext->Execute`**
   (TaaDiligent.cpp:1128), not as a standalone pass between the `props` and
   `taa_resolve` debug groups: the context's reprojected/previous-depth and
   closest-motion textures are produced by `PostFXContext::Execute`
   (PostFXContext.cpp:287-336), and SSAO's temporal accumulation reads them
   (SSAO cpp:1057-1059). A standalone between-passes Execute would read frame N-1
   context data (first frame: uninitialized). The SSAO work gets its own
   `ScopedDebugGroup{ctx, "ssao"}` so it shows up by name in `scripts/rdc.py list`
   (nested inside the `taa_resolve` group in the capture tree — the group list is
   name-based, scripts/rdc.py:132-140).
5. **Defaults: GTAO algorithm + `FEATURE_FLAG_HALF_RESOLUTION`.** Checkerboard depth
   - core AO run at half size (SSAO cpp:255-291), history stays full-size, and
     `GetAmbientOcclusionSRV()` returns the full-frame-size `UpsampledOcclusion`
     (R8) (cpp:283-290, 463-466) — so compositing stays 1:1 at the existing
     render-scale size with zero extra resize logic. `FEATURE_FLAG_HALF_PRECISION_DEPTH`
     stays off (R32F intermediates, same as the TAA context's `FEATURE_FLAG_NONE`).
6. **Toggle follows the TAA idiom** (Hydrogent gates on `SSAOScale > 0`,
   HnPostProcessTask.cpp:651): `ssao->PrepareResources` every frame (size-gated
   no-op when stable, cpp:86-87; keeps the frame index advancing so re-enable
   auto-resets history via the `m_LastFrameIdx != m_CurrentFrameIdx + 1` check,
   cpp:802-806 — `m_LastFrameIdx` only advances in `Execute`), `Execute` + composite
   only when `g.ssao`.
7. **No new include directories.** `#include "PostProcess/ScreenSpaceAmbientOcclusion/interface/ScreenSpaceAmbientOcclusion.hpp"`
   resolves via the `${diligent_git}/DiligentFX` root already on the include path
   (c-engine/CMakeLists.txt:77) — same as TaaDiligent.cpp:22-23. The attribs struct
   lives in `Shaders/PostProcess/ScreenSpaceAmbientOcclusion/public/ScreenSpaceAmbientOcclusionStructures.fxh`
   (fxh:64-101), same root; include it inside `namespace Diligent::HLSL` with
   `Shaders/Common/public/ShaderDefinitions.fxh` first (fxh:4-8 `#error` otherwise) —
   the exact include block TaaDiligent.cpp:40-46 already uses for the TAA structs.
8. **Never call `ScreenSpaceAmbientOcclusion::UpdateUI`** (hpp:134) — it calls
   `ImGui::` (cpp:403-425) and the prebuilt lib was built against its own imgui;
   the game keeps its own imgui copy and drives SSAO from the existing rmlui
   settings page / debug overlay.

## Phase 1 — World-space normal buffer (offscreen chain + world PS outputs)

Files:

- `c-engine/renderer/diligent/TaaDiligent.{h,cpp}`
  - `createTargets` (682-741): create `normalTex` — RGBA16F,
    `BIND_RENDER_TARGET | BIND_SHADER_RESOURCE`, target size, `Name "taaWorldNormal"`,
    next to `motionTex`; release it in `destroyTargets` (668-680).
  - new accessors (TaaDiligent.h, next to `taaColorRTV/taaMotionRTV/taaDepthDSV`
    at 87-89): `taaNormalRTV()`, `taaNormalSRV()`, `taaPostFXContext()`,
    `taaDepthSRV(int idx)` (SRV of `depthTex[idx]`, the same view TAA passes at 1125) — these are what `SsaoDiligent` consumes; the depth SRV and context are
    file-static in TaaDiligent.cpp today.
- `c-engine/renderer/diligent/DiligentRenderer.cpp:479-499`
  - `worldRTVs` becomes `{worldRtv, taaMotionRTV(), taaNormalRTV()}`,
    `SetRenderTargets(3, ...)`; third clear to `(0,0,0,0)` (the motion clear at
    496-499 is the pattern). The legacy fallback path (505-521, TAA chain
    unavailable) keeps binding 1 RTV against the 3-RT PSOs — identical to today's
    2-RT-PSO-vs-1-RTV situation; behavior unchanged there.
- `c-engine/renderer/diligent/HeightmapTerrainDiligent.cpp:807-809`
  - lit PSO: `gp.NumRenderTargets = 3`, add `gp.RTVFormats[2] = TEX_FORMAT_RGBA16_FLOAT;`
  - shadow caster PSO untouched: `gp.NumRenderTargets = 0` (line 724) — the shadow
    VS/PS are separate sources and write no color.
- `c-engine/renderer/diligent/PropsRenderDiligent.cpp:487-489` — same lit-PSO bump;
  shadow PSO (`NumRenderTargets = 0`, line 586) untouched.
- `c-engine/gltf/GltfDiligent.cpp`
  - `rendererCI.NumRenderTargets = 2 → 3`, add `rendererCI.RTVFormats[2] =
TEX_FORMAT_RGBA16_FLOAT;` (486-488).
  - `GetPSMainSource` hook (504-526): in `src.OutputStruct`, add
    `float4 WorldNormal : SV_Target2;` gated on a flag the WORLD path sets and the
    shadow path does not — use `PBR_Renderer::PSO_FLAG_FIRST_USER_DEFINED`
    (PBR_Renderer.hpp:638; the lambda gates at C++ string level, so no new shader
    macro is needed in the prebuilt lib). In `src.Footer`, under the same gate,
    `PSOut.WorldNormal = float4(VSOut.Normal, 1.0);` — `VSOut.Normal` is the unit
    world-space normal (`GLTF_TransformVertex` inverse-transpose + normalize,
    VertexProcessing.fxh:22-38), in footer scope for every variant including
    UNSHADED/unlit (computed before the lighting branch, RenderPBR.psh:456-649).
    Leave the existing `#if UNSHADED` color branch (513-515) and the CustomData
    block untouched. The `VSOutput.Normal` member is emitted per-PSO only when the
    model has a NORMAL attribute (`PSO_FLAG_USE_VERTEX_NORMALS` is derived from the
    model at GLTF_PBR_Renderer.cpp:501-523 and AND-ed into the final flags at
    GLTF_PBR_Renderer.cpp:590; member emitted in PBR_Renderer.cpp:1827). Every
    model in pak_1 has normals today (verified: eve 1/1, deciduous 24/24,
    deciduous_far 24/24 primitives), but wrap the write in
    `#if USE_VERTEX_NORMALS ... #else PSOut.WorldNormal = float4(0.0, 0.0, 1.0);
#endif` (the macro is per-PSO, PBR_Renderer.cpp:1553) so a future normal-less
    model degrades to a flat normal instead of failing runtime HLSL compilation.
  - `worldDraw` (1078-1084): add `| GLTF_PBR_Renderer::PSO_FLAG_FIRST_USER_DEFINED`
    to `renderInfo.Flags`.
  - `gltfDiligentShadowDraw` (1107-1171): `renderInfo.Flags` stays
    `PSO_FLAG_DEFAULT` (1168) → shadow PSOs have no `SV_Target2`; but the shared
    `NumRenderTargets = 3` forces a third RTV at draw time: extend the throwaway
    dummy pair (1129-1151) with a cascade-sized RGBA16F dummy and bind
    `rtvs[3]` at 1153-1155 (same VUID-VkRenderingInfo-pNext-06079 rationale the
    existing dummies document, 1103-1106/1125-1128).
- `c-engine/renderer/diligent/shaders/heightmap_terrain_ps.hlsl`
  - `PSTerrainOut` (159-162): add `float4 Normal : SV_Target2;`
  - `main` (257): `Out.Normal = float4(geomNormal, 1.0);` — `geomNormal =
normalize(vs.Normal)` (267) is the border-aware world-space stencil normal the
    VS already outputs (heightmap_terrain_vs.hlsl:55,83).
- `c-engine/renderer/diligent/shaders/props_ps.hlsl`
  - `PSPropsOut` (80-83): add `float4 Normal : SV_Target2;`
  - `main` (101): `Out.Normal = float4(N, 1.0);` — `N` (131, front/back-flipped
    geometric normal) is world space: the VS rotates the mesh normal by the
    per-instance Y yaw (props_vs.hlsl:106-107,151) and does no scaling.
  - Shadow variant (`props_shadow_ps.hlsl`) untouched — `NumRenderTargets = 0`.
- Pak: the six `.hlsl` sources are copied to `c-game/data/pak_1/materials/` by the
  existing CMake custom command (c-engine/CMakeLists.txt:178-213) — no CMake change,
  but `./scripts/build.sh` MUST run after shader edits: runtime-compiled HLSL loads
  from the packed `pak_1.pak`, not the loose tree (docs/lessons.md 2026-09-07).

**Acceptance:** `./scripts/build.sh` clean; RenderDoc capture of a world frame —
the `player`/`terrain`/`props` groups show 3 RTVs; dumping RT2 shows unit-length
normals (terrain: smooth ground normals, props: mesh normals, player: character
normals); the `shadow` pass still renders (cascade depth unchanged, dummy RTVs
bound); `ENGINE_SCREENSHOT` with SSAO off is visually identical to the pre-phase
baseline (the extra RT costs pixels, not the image).

## Phase 2 — `SsaoDiligent` module

Files:

- **NEW** `c-engine/renderer/diligent/SsaoDiligent.h`
  - `ssaoInit(device)` / `ssaoDestroy()` — construct
    `std::unique_ptr<Diligent::ScreenSpaceAmbientOcclusion>` (ctor takes
    `CreateInfo{EnableAsyncCreation = false}`; the attribs struct + constant buffer
    are created inside the ctor, SSAO cpp:50-60).
  - `ssaoSettingsApply(bool enabled, float radius, float algorithm)` — store
    `g_ssaoOn` + the `HLSL::ScreenSpaceAmbientOcclusionAttribs` fields
    (`EffectRadius`, `Algorithm`; defaults per fxh:64-101: `EffectRadius 1.0`,
    `EffectFalloffRange 0.615`, `RadiusMultiplier 1.457`, `TemporalStabilityFactor
0.9`, `SpatialReconstructionRadius 4.0`, `Algorithm = SSAO_ALGORITHM_GTAO`).
  - `ssaoFrameBegin(ctx)` — `ssao->PrepareResources(device, ctx, taaPostFXContext(),
Diligent::ScreenSpaceAmbientOcclusion::FEATURE_FLAG_HALF_RESOLUTION)` every
    frame, enabled or not (size-gated no-op when stable).
  - `ssaoExecute(ctx)` — the per-frame `Execute` (see below); returns false while
    `POST_FX_EXECUTION_STATUS_PENDING` (PSOs still being created — the library
    writes a placeholder texture, hpp:129-131).
  - `ssaoAOSRV()` — `ssao->GetAmbientOcclusionSRV()` (full-frame R8) for the
    composite; `ssaoReady()`.
- **NEW** `c-engine/renderer/diligent/SsaoDiligent.cpp`
  - Includes: `PostProcess/ScreenSpaceAmbientOcclusion/interface/ScreenSpaceAmbientOcclusion.hpp`,
    then the TaaDiligent.cpp:40-46 include block with
    `Shaders/PostProcess/ScreenSpaceAmbientOcclusion/public/ScreenSpaceAmbientOcclusionStructures.fxh`
    in place of the TAA structures fxh (ShaderDefinitions.fxh first — fxh:4-8).
  - `ssaoExecute`: build `ScreenSpaceAmbientOcclusion::RenderAttributes`
    (hpp:85-107): `pDevice`, `pStateCache = nullptr`, `pDeviceContext`,
    `pPostFXContext = taaPostFXContext()`,
    `pDepthBufferSRV = taaDepthSRV(frameIdx & 1)` (the same SRV TAA uses),
    `pNormalBufferSRV = taaNormalSRV()`, `pSSAOAttribs = &attribs`.
    DEV_CHECKs (cpp:350-356) require all three of those non-null — hence the new
    TaaDiligent accessors from phase 1.
  - **Call site:** inside `taaWorldResolve`, immediately after
    `postFXContext->Execute(pa)` (TaaDiligent.cpp:1128), wrapped in
    `Diligent::ScopedDebugGroup{ctx, "ssao"}`, only when `ssaoReady() && ssaoOn`.
    This is after the world pass (depth/normal/motion all final for this frame)
    and before `taa->Execute` (1142) — the ordering constraint from decision 4:
    SSAO reads the context textures `postFXContext->Execute` just produced.
    On `PENDING`: skip the composite this frame (next frame's placeholder
    is overwritten by the real AO).
- `c-engine/CMakeLists.txt:135-146` — add `renderer/diligent/SsaoDiligent.cpp` to
  the `SKIP_PRECOMPILE_HEADERS` list (the TU is globbed in automatically, line 1;
  no new include dirs — decision 7).

**Acceptance:** `scripts/rdc.py list` on a capture shows an `ssao` group containing
the library's own `ScreenSpaceAmbientOcclusion` group with the 8 child compute
groups (`ComputeDownsampledDepth` → `ComputeBilateralUpsampling`, SSAO cpp:363,
834-1311); `scripts/rdc.py dump` of the `ssao` group's final output shows an
R8 occlusion map that is NOT all-1 (darkening in contact/crevice areas of the
props camera). AO off: no `ssao` group in the capture.

## Phase 3 — AO composite into the world color

Files:

- `c-engine/renderer/diligent/TaaDiligent.cpp`
  - new target `aoCompositeTex` — RGBA16F, TARGET size (1:1 with `sceneColorTex` /
    the TAA accumulated frame / `UpsampledOcclusion`), created in `createTargets`,
    released in `destroyTargets` (668-741).
  - new inline PS (alongside `kBlitPS`/`kDownsamplePS`/`kCasPS`, 157-382):
    `out = in.rgb * (1.0 - (1.0 - ao.r) * strength)` with `ao` a 1:1 point sample of
    the AO R8 texture — a small `AoCompositeAttribs` CB carries `strength`
    (intensity; default 1.0) so the settings page can tune it without a new PSO.
  - in `taaWorldResolve`, between the TAA block (1142-1145) and the renderScale
    downsample (1155-1163), after the `ENGINE_TAA_DEBUG_MV` swap (1150-1153) —
    the debug-MV path must stay AO-free: when `ssaoOn && ssaoAOSRV() != nullptr`,
    run the composite pass (`srcColorSRV` → `aoCompositeTex`, sampling the AO SRV)
    and set `srcColorSRV = aoCompositeTex SRV` before the downsample/CAS/blit.
    Because the composite is at TARGET size it composes with `renderScale` exactly:
    the box downsample (1159-1163) then averages the AO'd color, and the CAS/blit
    (1167-1173) see a single source as today. Works identically for TAA-on,
    TAA-off (raw `sceneColorTex` path), and CAS-off.
- `c-engine/renderer/diligent/DiligentRenderer.cpp:801-804`
  - `applyGraphicsSettings`: forward `s.ssao` (+ radius/algorithm/intensity from
    phase 4) into `ssaoSettingsApply(...)`.

**Acceptance:** with SSAO on, `ENGINE_SCREENSHOT` darkens concave contact areas
(tree trunks against trunks, under-canopy, terrain crevices) while the open sky
and flat ground stay untouched; toggling SSAO off returns to a pixel-identical
baseline (the composite is the only image change); at `renderScale = 0.5/1.5/2.0`
the AO follows the scene (no full-res bleed or mis-registration).

## Phase 4 — Settings wiring (flag + radius/algorithm)

Files:

- `c-engine/renderer/Renderer.h:11-26` — `GraphicsSettings`: keep `ssao` (line 19);
  add `float ssaoRadius = 1.0f;`, `int ssaoAlgorithm = 0;` (0=GTAO 1=HBAO 2=VBAO,
  the `ALGORITHM_TYPE` values, SSAO hpp:72-82), `float ssaoIntensity = 1.0f;`.
- `c-engine/renderer/Renderer.cpp`
  - `graphicsNormalize` (~195-210): clamp `ssaoRadius` (e.g. 0.1..10),
    `ssaoAlgorithm` 0..2, `ssaoIntensity` 0..2.
  - `rendererGraphicsLoad` (222-240): read the new keys
    (`s.ssaoRadius = (float)utils::settingsGetDouble("ssaoRadius");` etc.).
- `c-utils/settings/Settings.cpp:53` — add templates next to `aoDisabled`:
  `{"ssaoRadius", "double", 1.0}`, `{"ssaoAlgorithm", "int", 0.0}`,
  `{"ssaoIntensity", "double", 1.0}`. **Must** be added with the right types —
  settings.json type-validation rewrites the whole file on a type mismatch
  (docs/lessons.md 2026-09-04).
- `c-game/game/settingsGui/graphics/SettingsGraphicsGui.cpp`
  - `applyRenderer` (101-125): forward `g.ssaoRadius/ssaoAlgorithm/ssaoIntensity`.
  - load (215-216 area): read the three new keys.
  - new controls on the existing AO row (the `aoEnabled` toggle at 31/411-415,
    label sync at 337-338): a radius slider, an algorithm cycle (GTAO/HBAO/VBAO)
    and an intensity slider, each following the existing `toggleX`/`persistX`/
    `syncLabels`/`rmlBind` pattern (247,337); persist
    `ssaoRadius`/`ssaoAlgorithm`/`ssaoIntensity`; keep the legacy `aoDisabled`
    boolean in sync (413).
- `c-game/data/pak_1/gui/settings/graphics/graphics.html:92` — the AO row
  (`{{aoLabel}}`): add the radius/algorithm/intensity widgets + labels. (The
  in-game debug overlay toggle already works — `DebugGui.cpp:100-106` flips
  `g.ssao` through `rendererGraphicsApply`.)
- `c-engine/gui/rmlui/guis/DebugGui.cpp` — no change needed for the on/off toggle;
  optionally surface radius/algorithm there too (the `aoEnabled` binding at 167
  already exists).

**Acceptance:** both GUIs drive the effect live (debug-overlay `aoEnabled`
toggle; settings-page radius/algorithm/intensity); values persist across restarts
(`settings.json` round-trips, no full-file rewrite — diff the file after a
toggle); `aoDisabled` stays the persisted on/off key (legacy compat, 234).

## Phase 5 — Verification

All headless, from the project root after `./scripts/build.sh`:

1. **Build:** `./scripts/build.sh` — clean compile + pak regeneration (the
   shader-copy custom command runs; verify the edited PS landed in the pak:
   `unzip -p build/c-game/data/pak_1.pak materials/props_ps.hlsl | grep SV_Target2`
   — runtime HLSL reads the pak, not the loose tree, docs/lessons.md 2026-09-07).
2. **SSAO pass in the capture:**
   `ENGINE_RENDERDOC_CAPTURE=1 ENGINE_RENDERDOC_CAPTURE_FRAMES=300 ENGINE_LOG_TIMEOUT=120000 ./scripts/run.sh renderdoc`
   then `scripts/rdc.py list` — expect the new `ssao` group (with the
   `ScreenSpaceAmbientOcclusion` child groups from the library's
   `ScopedDebugGroup`s) between `props` and the TAA/blit work; `scripts/rdc.py dump ssao`
   shows the occlusion output (non-trivial R8 map); dump the world pass and confirm
   RT2 = unit normals for terrain/props/player.
3. **Before/after frames:** `ENGINE_SCREENSHOT=/tmp/ssao_off.jpg` then
   `aoDisabled=true` in `data/settings.json` (or the debug overlay) +
   `ENGINE_SCREENSHOT=/tmp/ssao_on.jpg` — same vantage; occluded areas darker with
   SSAO on, sky/flat ground unchanged. Repeat at `ENGINE_CAMERA=props` /
   `propsground` (densest occlusion) and at two `renderScale` values.
4. **Resize:** change window size mid-run (or two launches at different sizes) —
   `createTargets` + the context's `FrameDesc` recreate the chain; SSAO
   `PrepareResources` rebuilds its back buffers (cpp:86+) and the history resets
   via the frame-index continuity check (cpp:802-806).
5. **Lessons pitfall check** (docs/lessons.md):
   - D32-as-SRV on Vulkan: already proven by TAA (TaaDiligent.cpp:1125) — SSAO reads
     the same SRV; watch for new VUIDs in the capture log anyway.
   - Dynamic-buffer ring clobbering (2026-09-05): SSAO's constant buffer is a
     regular `USAGE_DEFAULT` buffer updated with `UpdateBuffer` (cpp:794-817),
     NOT a dynamic buffer — no ring pitfall; confirm no new `MAP_` calls from the
     module.
   - The shadow pass's VUID-VkRenderingInfo-pNext-06079 (cascade RTs wider than the
     2048 DSV): the new 3rd dummy RTV must be cascade-sized like its siblings
     (1129-1151), or the capture log gets the VUID back.
   - `--start-group` link line: `libDiligentFX.a` is already inside c-game's
     `--start-group` (c-game/CMakeLists.txt:11,64) and exports every SSAO symbol
     (nm-verified) — expect no link change; if the linker ever complains about a
     missing symbol, it will be from a NEW dependency of the SSAO objects, and the
     fix is adding that archive to `DILIGENT_LIBS`, not reordering.
   - Static cbuffer on SRB silently ignored (2026-09-06): the composite pass's
     `AoCompositeAttribs` follows the same rule as the existing CAS blit — set the
     cbuffer on the PRS/PSO, not per-frame on a static SRB slot.
6. **Perf sanity:** the normal RT adds 8 B/px write + (SSAO half-res) read at
   target size — measure with `ENGINE_DEBUG_GPUTIME=1` (DiligentRenderer.cpp:435)
   static + moving-camera before/after; budget is the existing
   ≤ 1.5 ms/frame post-world envelope used in the terrain plan.

## Risks / gotchas

1. **D32-as-SRV on Vulkan** — sampling the D32 depth as a shader resource.
   Mitigated: TAA reprojection already does exactly this every frame
   (TaaDiligent.cpp:1125, `depthTex[curr]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)`),
   and SSAO's downsample/prefilter techniques consume the same SRV type
   (`g_TextureDepth`, SSAO_ComputeDownsampledDepth.fx). No new driver surface;
   if the capture log shows a depth-SRV VUID it would already show for TAA today.
2. **TAA × SSAO interaction** — SSAO has no external jitter input (the fxh attribs
   have no jitter field); its temporal stability is internal (history textures +
   `SSAO_MAX_HISTORY_LENGTH 16` + the context's closest-motion disocclusion,
   SSAO cpp:1057-1059). Consequences: (a) apply AO _after_ TAA (decision 3) so the
   two history filters don't compete; (b) when TAA is on, SSAO's depth/normal
   inputs are jittered per frame — its own accumulation handles that the same way
   TAA's does; expect a brief settle after enabling SSAO, not steady-state noise;
   (c) `PostFXContext::GetTransitionAlpha` fades the AO in over a short ramp after
   (re)enable (cpp:799, 809-816) — screenshots taken immediately after toggling on
   will show partial AO by design.
3. **Normal-RT bandwidth at render scale** — `normalTex` is RGBA16F at
   `swapchainSize * renderScale` (up to 2×): ~18 MB at 1080p×1, ~72 MB at 4K×1,
   one write per world pixel + SSAO half-res reads. That is the cost of a true
   G-Buffer normal (Hydrogent pays the same, HnBeginFrameTask.cpp:66); mitigations
   in order: keep SSAO at `FEATURE_FLAG_HALF_RESOLUTION` (default here), drop the
   normal RT to RG16F+depth-reconstruction if ever needed (not a Vulkan RT format
   for RGB — would need a format change, not a flag), or lower renderScale.
4. **`--start-group` link line** — c-game links all Diligent archives inside one
   `--start-group` (c-game/CMakeLists.txt:11,64) and `libDiligentFX.a` already
   contains the SSAO symbols (nm-verified: `ScreenSpaceAmbientOcclusion::{ctor,
PrepareResources, Execute, GetAmbientOcclusionSRV}`). No new archive is
   expected; the one ABI trap is `UpdateUI`'s `ImGui::` calls (cpp:403-425) against
   the lib's own imgui build — do not call it (decision 8).
5. **One `NumRenderTargets` for all PBR PSOs** — the GLTF_PBR_Renderer CreateInfo
   is a single value (GltfDiligent.cpp:486); bumping it to 3 forces the SHADOW
   path to bind a 3rd RTV (the dummies at 1129-1155), and its PSOs must not write
   `SV_Target2` (they don't — the gate flag is world-path-only). Forgetting either
   half breaks the shadow pass (VUID-06079 for an undersized bind, or an
   out-of-range RT write).
6. **Context-texture ordering** — SSAO's temporal accumulation reads
   `GetReprojectedDepth()/GetPreviousDepth()/GetClosestMotionVectors()`
   (SSAO cpp:1057-1059), produced by `PostFXContext::Execute` (PostFXContext.cpp:287-336).
   Placing the SSAO Execute before that call (e.g. as a standalone pass between
   `props` and `taa_resolve`) silently reads the previous frame's context data —
   AO disocclusion would be off by one frame (first frame: uninitialized). The
   call site in `taaWorldResolve` after TaaDiligent.cpp:1128 is not cosmetic.

## Out of scope

Screen-space reflections / GI (separate DiligentFX modules, separate plan), the
VBAO/HBAO quality-tuning pass (the algorithm switch ships in phase 4, tuning later),
AO into the lighting equation (the README's diffuse-term formula — needs a
diffuse/specular split the resolve path doesn't have; a color multiply is the
practical application), normal-buffer use by other effects, and any Filament-era
path (backend removed 2026-09-05).
