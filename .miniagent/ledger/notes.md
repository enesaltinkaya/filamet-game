# notes

## brainstorm

## Core difficulty

Bloom itself is the *simplest* DiligentFX post-FX effect to integrate — it needs exactly
one external input (the HDR color SRV), no normal/depth buffer, no new offscreen-chain
texture, and its output is the final HDR color directly (a drop-in `srcColorSRV` swap),
so the whole "add a 3rd RTV + world-PS normal output" Phase 1 of the SSAO plan is empty
for bloom. The real difficulty is the *placement* in `taaWorldResolve`: picking the
correct slot relative to the existing SSAO-composite (TaaDiligent.cpp:1367-1371) and the
render-scale downsample (1377-1381) so bloom reads the resolved target-size HDR frame and
stays the last HDR pass before the sRGB encode-on-store blit/CAS — plus confirming the
3-channel `R11G11B10_FLOAT` bloom output flows through the existing `Texture2D<float4>`
blit/CAS PS (HLSL 3→4 channel extension, alpha=1.0).

## Reductions / key lemmas

1. **Color→color drop-in, not a mask multiply.** The upsampled pass
   (`Bloom_ComputeUpsampledTexture.fx:52`) returns `SourceColor + ColorSum` into the
   full-frame output; `Bloom::GetBloomTextureSRV()` (Bloom.cpp:452-455) returns that
   texture. So integration is `srcColorSRV = bloom->GetBloomTextureSRV()` — a pointer swap,
   NOT a new composite shader. Contrast: SSAO's R8 AO mask had no color and needed
   `aoCompositeApply` (a 1:1 multiply pass into `aoCompositeTex`). This deletes the
   SSAO-plan Phase 3 (AO composite) entirely.
2. **No new textures, no PSO/RTV change.** Bloom's only external input is the HDR color
   SRV (already the TAA accumulated frame / `sceneColorTex`); every internal texture
   (downsample/upsample pyramids + output) is created by `Bloom::PrepareResources`
   (Bloom.cpp:103-141). No offscreen-chain edit, no `NumRenderTargets` bump, no HLSL edit,
   no pak re-pack.
3. **Target-size invariance (free renderScale support).** `PrepareResources` sizes from
   the `PostFXContext::FrameDesc` (Bloom.cpp:84-85, no `FEATURE_FLAG_TEMPORAL_UPSCALING` →
   uses `FrameDesc.Width/Height` = target size = `swapchainSize * renderScale`). So the
   bloom output is 1:1 with the TAA frame at any `renderScale`, and the existing
   downsample/CAS/blit handle the backbuffer exactly as today — no resize logic.
4. **Placeholder = valid color → no ready-gate.** While PSOs create, `Execute` returns
   `PENDING` and runs `ComputePlaceholderTexture` (Bloom.cpp:398-405, a copy of the input
   color into the output), so `GetBloomTextureSRV()` is always a valid `srcColorSRV`
   replacement. No `bloomRan`/ready gate is needed for the swap (contrast: SSAO's AO mask
   was undefined during PENDING and required the `ssaoRan` gate at TaaDiligent.cpp:1367).
5. **The `bloom` on/off flag already flows end-to-end** — `GraphicsSettings.bloom`
   (Renderer.h:23, default `true`), loaded from `bloomDisabled` (Renderer.cpp:244),
   persisted (Settings.cpp:63, default 0 = bloom ON), toggled in the GUI
   (SettingsGraphicsGui.cpp:483-490). Only `applyGraphicsSettings`
   (DiligentRenderer.cpp:807-810) does not forward it yet. Identical shape to how the
   `ssao` flag worked before the SSAO plan — the settings wiring is half-built.
6. **`libDiligentFX.a` already exports every needed symbol** (nm-verified on the
   build-linux archive): `Bloom::{Bloom,~Bloom,PrepareResources,Execute,GetBloomTextureSRV,
   UpdateUI}`. No new archive to link; the one ABI trap is `Bloom::UpdateUI` (Bloom.cpp:
   457-478, calls `ImGui::`) against the lib's own imgui — never call it, drive bloom from
   the rmlui settings page.

## Candidate approaches

1. **Thin `BloomDiligent` module on the existing `PostFXContext`, pointer-swap into
   `taaWorldResolve`.** Mirror `SsaoDiligent` exactly (`bloomInit/bloomDestroy/
   bloomSettingsApply/bloomFrameBegin/bloomExecute/bloomSRV`), call `bloom->Execute` after
   the AO composite (1371), swap `srcColorSRV = bloomSRV()`. *Risk:* the 3-channel
   `R11G11B10_FLOAT` output through the `float4` blit/CAS PS — verify by screenshot, low
   effort, no code risk. *Effort: S* (one small module + three one-line call sites).
2. **Standalone "bloom" pass in `DiligentRenderer.cpp` draw loop + full-res composite
   PS.** Run the effect between `props` and `taa_resolve` debug groups, then a new
   additive composite to merge. *Risk:* unnecessary — the pointer-swap already yields the
   result; adds a shader + a target + a pass, and fights the render-scale downsample
   ordering. *Effort: M, not recommended.*
3. **Integrate the `Bloom` object directly into `TaaDiligent.cpp`** (no new file).
   *Risk:* muddies the TAA module's single responsibility; the `SsaoDiligent` thin-module
   precedent exists and is cleaner and independently verifiable. *Effort: S, not
   recommended.*

## Recommended approach

**Approach 1.** It is the established pattern (SSAO), the minimal correct change (pointer
swap; no new textures/PSO/HLSL), and it places bloom as the *last HDR pass before the sRGB
encode-on-store* — matching the Tutorial27/Radient order (light → AO → TAA → bloom →
tonemap; Tutorial27_PostProcessing.cpp:314-320, RadientTesseraPostProcessPipeline.cpp:
540-557 both run bloom after TAA, before the final tonemap pass). Insertion point: inside
`taaWorldResolve`, after the AO composite block (TaaDiligent.cpp:1367-1371) and before the
render-scale downsample (1377) — bloom then reads the AO'd resolved target-size HDR frame
and its output (also target-size) flows through the same downsample/CAS/blit. What must be
true for it to work: (a) `GetBloomTextureSRV()`'s `R11G11B10_FLOAT` output binds to the
`Texture2D<float4>` blit/CAS `g_Source` via standard 3→4 channel extension (verify by
screenshot + capture); (b) `bloom->Execute` runs after `postFXContext->Execute`
(1329) — it reads `GetTransitionAlpha` and `IsPSOsReady` from the context; (c) `g.bloom`
is forwarded from `applyGraphicsSettings` into `bloomSettingsApply`.

Note the transition fade: `Bloom::UpdateConstantBuffer` (Bloom.cpp:272-286) multiplies
`PostFXContext::GetTransitionAlpha` into `BloomAttribs.AlphaInterpolation`, so the effect
ramps in over a short duration after (re)enable. `ENGINE_SCREENSHOT` (fired a few frames
after startup) with bloom ON will show *partial* bloom by design — take the "on" capture a
frame past the ramp, and the "off" baseline by setting `bloomDisabled=true`.

## Proposed tasks

1. **Create `c-engine/renderer/diligent/BloomDiligent.{h,cpp}`** mirroring `SsaoDiligent`
   (`SsaoDiligent.h`/`.cpp` as the template): `bloomInit()` (ctor `Bloom(device,
   Bloom::CreateInfo{false})`), `bloomDestroy()`, `bloomSettingsApply(bool enabled)`
   (store `bloomOn` + a static `HLSL::BloomAttribs` with defaults Intensity 0.15 /
   Threshold 1.0 / SoftTreshold 0.125 / Radius 0.75 — `BloomStructures.fxh:14-24`),
   `bloomOn()`, `bloomSRV()` (→ `GetBloomTextureSRV()`), `bloomFrameBegin(ctx)` (→
   `PrepareResources(device, ctx, taaPostFXContext(), FEATURE_FLAG_NONE)` every frame,
   like `ssaoFrameBegin` at SsaoDiligent.cpp:67-73). Include block: `Bloom.hpp` then
   `Shaders/Common/public/ShaderDefinitions.fxh` + `Shaders/PostProcess/Bloom/public/
   BloomStructures.fxh` inside `namespace Diligent::HLSL` (the fxh `#error` demands
   ShaderDefinitions first — same block shape as SsaoDiligent.cpp:13-18). Add
   `renderer/diligent/BloomDiligent.cpp` to the `SKIP_PRECOMPILE_HEADERS` list
   (c-engine/CMakeLists.txt:135-146).
2. **Wire the lifecycle + frame-begin hooks.** `bloomInit()` next to `ssaoInit()`
   (DiligentRenderer.cpp:350), `bloomDestroy()` next to `ssaoDestroy()` (DiligentRenderer.
   cpp:738), `bloomFrameBegin(ctx)` next to `ssaoFrameBegin(ctx)` (TaaDiligent.cpp:1042).
   Verify: `./scripts/build.sh` clean; the module constructs/tears down with the renderer.
3. **Execute + swap in `taaWorldResolve`.** After the AO composite (TaaDiligent.cpp:1371)
   and before the downsample (1377): `if (bloomOn()) { Diligent::ScopedDebugGroup g(ctx,
   "bloom"); Bloom::RenderAttributes ra; ra.pDevice=device; ra.pDeviceContext=ctx;
   ra.pPostFXContext=taaPostFXContext(); ra.pColorBufferSRV=srcColorSRV; ra.pBloomAttribs=
   &bloomAttribs; bloom->Execute(ra); if (ITextureView* b=bloomSRV()) srcColorSRV=b; }`
   (the `bloom` unique_ptr is module-static, exposed via `bloomExecute`/`bloomSRV` like
   the SSAO private-static pattern). Verify: `scripts/rdc.py list` shows a `bloom` group
   with `ComputePrefilteredTexture` / `ComputeDownsampledTexture` / `ComputeUpsampledTexture`
   children (Bloom.cpp:298,320,349); `scripts/rdc.py dump bloom` shows the full-frame
   output; `ENGINE_SCREENSHOT` with `bloomDisabled=true` is pixel-identical to baseline,
   with bloom ON bright areas (sky, sunlit surfaces) show halos; repeat at `renderScale`
   0.5 / 1.5 / 2.0 to confirm the bloom follows the scene (no full-res bleed).
4. **Forward the flag.** `applyGraphicsSettings` (DiligentRenderer.cpp:807-810): add
   `bloomSettingsApply(s.bloom);` next to the existing `ssaoSettingsApply(...)`. Verify:
   the in-game `toggleBloom` (SettingsGraphicsGui.cpp:483) and the `bloomDisabled` key
   switch the effect live and persist across restarts (diff `settings.json` after a
   toggle — no full-file rewrite; the key already exists at Settings.cpp:63).

Optional follow-on (mirrors SSAO Phase 4, defer): add `bloomIntensity/bloomThreshold/
bloomSoftThreshold/bloomRadius` to `GraphicsSettings` + `bloomDisabled`-adjacent keys +
GUI sliders, clamped to the `UpdateUI` ranges (Bloom.cpp:461-475: Intensity 0..1, Radius
0.3..0.85, Threshold 0..10, SoftTreshold 0..1).

## final

## Findings

Deliverable shipped: `plans/bloom-diligentfx.md` (24.7 KB), mirroring
`plans/ssao-diligentfx.md`. Every factual claim in the plan was re-verified
against the live tree this round (not just the earlier brainstorm); all held.

- **Bloom module contract (verified in Bloom.hpp/Bloom.cpp):** ctor takes
  `CreateInfo{EnableAsyncCreation}`; `PrepareResources(device, ctx,
  PostFXContext*, FEATURE_FLAGS)` sizes from `FrameDesc.Width/Height` (no
  `FEATURE_FLAG_TEMPORAL_UPSCALING` on our context -> target size), all internal
  textures `R11G11B10_FLOAT`; `Execute`'s only mandatory input is
  `pColorBufferSRV` (DEV_CHECKED) + `pBloomAttribs`; `GetBloomTextureSRV()`
  returns the output = `SourceColor + ColorSum` (upsample shader `uInstID==0`
  branch). `FEATURE_FLAGS` only has `FEATURE_FLAG_NONE`. `UpdateUI` is the
  ImGui ABI trap. `HLSL::BloomAttribs` defaults: Intensity 0.15 / Threshold 1.0
  / SoftTreshold 0.125 (source misspelling) / Radius 0.75. `nm` on the
  build-linux `libDiligentFX.a` exports every `Bloom` symbol.
- **Integration surface (verified):** `taWorldResolve` (TaaDiligent.cpp:1310)
  has the exact slots the plan cites — SSAO composite 1367-1371, renderScale
  downsample 1375-1381, CAS/blit 1385-1394. The blit/CAS/downsample PS all
  sample `Texture2D<float4> g_Source` and the present is opaque, so the
  `R11G11B10_FLOAT` output's defined-1.0 alpha is inert (Risk 1 is low). The
  `bloom` flag already flows end-to-end EXCEPT `applyGraphicsSettings`
  (DiligentRenderer.cpp:806-809) not forwarding `s.bloom` — that one line is
  the only real settings change. `device` is `extern` in DiligentRenderer.h:19;
  `taaPostFXContext()` is TaaDiligent.h:93 (the accessor SsaoDiligent uses).
- **`SsaoDiligent.{h,cpp}` is the proven, in-tree template** to copy for
  `BloomDiligent` (module-statics, includes, `SKIP_PRECOMPILE_HEADERS` entry at
  c-engine/CMakeLists.txt:141). Bloom is strictly simpler: no new texture/RTV/
  PSO/HLSL, no normal buffer — the whole SSAO "Phase 1" and AO-composite
  "Phase 3" are empty.
- **Reference order (verified in Tutorial27 + Radient):** light -> SSAO -> TAA
  -> bloom -> tonemap/encode. Tutorial27_PostProcessing.cpp 314-317/774/895;
  RadientTesseraPostProcessPipeline.cpp 431->467->503->540->344 (bloom after
  TAA/DoF, `pCurrentColorSRV = GetBloomTextureSRV()` into the final tonemap;
  `MakeBloomAttribs` at RadientPostFXParameters.cpp:95). So the plan's app point
  (after the SSAO composite, before downsample/CAS/blit) matches both references.
- **No new link target / no pak re-pack:** `libDiligentFX.a` is already in the
  `--start-group` and exports all `Bloom` symbols; bloom reuses the prebuilt
  lib's `.fx` shaders (loaded by the library), so no `pak_1.pak` regeneration
  is needed (unlike SSAO's shader edits).

## Remaining steps

- **Phase 1 (S):** create `c-engine/renderer/diligent/BloomDiligent.{h,cpp}`
  mirroring `SsaoDiligent`; add `BloomDiligent.cpp` to `SKIP_PRECOMPILE_HEADERS`
  (c-engine/CMakeLists.txt:135-146). `./scripts/build.sh` clean.
- **Phase 2 (S):** `bloomInit()` next to `ssaoInit()` (DiligentRenderer.cpp:350),
  `bloomDestroy()` next to `ssaoDestroy()` (:738), `bloomFrameBegin(ctx)` next
  to `ssaoFrameBegin(ctx)` (TaaDiligent.cpp:1042).
- **Phase 3 (S):** in `taWorldResolve` after the AO composite (1367-1371), before
  the downsample (1375): `if (!debugMv && bloomOn()) if (bloomExecute(ctx,
  srcColorSRV)) if (ITextureView* b = bloomSRV()) srcColorSRV = b;` wrapped in
  `ScopedDebugGroup{ctx,"bloom"}`. Verify via `scripts/rdc.py list` (expect a
  `bloom` group) + `ENGINE_SCREENSHOT` on/off baseline.
- **Phase 4 (S):** add `bloomSettingsApply(s.bloom);` to `applyGraphicsSettings`
  (DiligentRenderer.cpp:806-809). Optional follow-on: the 4 sliders
  (intensity/threshold/softThreshold/radius) + `Settings.cpp` templates with the
  exact types + GUI.
- **Verification:** `./scripts/build.sh`; RenderDoc capture `scripts/rdc.py
  list`/`dump bloom`; `ENGINE_SCREENSHOT` on/off + `renderScale` 0.5/1.5/2.0
  sweep; note the transition-ramp caveat (take the "on" shot past the ramp).
