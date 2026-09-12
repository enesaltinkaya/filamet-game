# notes

## brainstorm

### Session context (verified facts, not guesses)

- `SsaoDiligent.{h,cpp}` is the template: static `ScreenSpaceAmbientOcclusion` + attribs, `PrepareResources` per frame on the shared `postFXContext`, `Execute` with depth/normal SRVs, called from `taaWorldResolve` (TaaDiligent.cpp ~1338 and ~1374 for the `!taaOn` branch), composite via `aoCompositeApply` after TAA.
- SSR interface (`ScreenSpaceReflection.hpp`): RenderAttributes = color, depth, **normal, material**, motion SRVs + attribs; output `GetSSRRadianceSRV()` = RGBA (rgb radiance, a confidence).
- Contracts verified compatible with our engine:
  - Normals must be **world-space** (SSR_ComputeIntersection.fx `LoadNormalWS` → mul by `g_Camera.mView` in-shader). Our GLTF footer writes `VSOut.Normal` (world). ✓
  - Depth: engine clears 1.0 (standard Z), `PostFXContext` created with `FEATURE_FLAG_NONE` (TaaDiligent.cpp:1033) → SSR reads the same flags (ScreenSpaceReflection.cpp:73) and compiles `SSR_OPTION_INVERTED_DEPTH=0` — the supported non-reversed path, same as the proven SSAO config. ✓
  - Motion: SSR multiplies by `F3NDC_XYZ_TO_UVD_SCALE.xy` — same RG16F NDC convention as our motionTex. ✓
  - Material: stencil pass declares `Texture2D<float4> g_TextureMaterialParameters` with `RoughnessChannel` (0..3) and `IsRoughnessPerceptual` attribs → binding the RGBA16F normal texture as material SRV is contract-legal (`RoughnessChannel=3`, `IsRoughnessPerceptual=TRUE`).
  - Roughness availability: the PS footer override in `gltfInitDiligent()` (GltfDiligent.cpp ~581-615) runs at end of main of the stock RenderPBR.psh, where `Shading.BaseLayer.Srf.PerceptualRoughness` is in scope — a one-line **local** footer change writes roughness into `WorldNormal.a`. No third-party shader edits needed.
  - SSAO safety: every SSAO shader reads the normal buffer as float3/`.xyz` only → alpha reuse is invisible to SSAO.
- Reference composite (Hydrogent/shaders/HnPostProcess.psh:147-171): `Color.rgb += (GetSpecularIBL_GGX(SSR) - SpecularIBL) * SSR.w * SSRScale` — it **replaces** the G-buffer IBL specular. We have no SpecularIBL G-buffer; our color already contains the near-flat constant-cube IBL specular.
- Settings plumbing shape to mirror: `applyGraphicsSettings` (DiligentRenderer.cpp:787-791) forwards `s.ssao*` to `ssaoSettingsApply`; init/destroy beside ssao at :351/:717.

## Core difficulty

Wiring itself is de-risked by the SSAO integration; the real difficulty is feeding SSR its two inputs we don't currently produce (per-pixel perceptual roughness, and a material buffer at all) and compositing without the reference's SpecularIBL G-buffer — each silent contract mismatch (normal space, depth convention, motion encoding, roughness channel) yields black/garbage reflections that are expensive to debug blind.

## Reductions / key lemmas

1. **Contract audit (done, above): every SSR input matches what the engine already produces or can produce with local one-line changes.** No third-party source edits; no new G-buffer target required.
2. **Monotonic bring-up invariant:** current `WorldNormal.a = 1.0` everywhere ⇒ perceptual roughness 1.0 ⇒ `IsReflectionSample` false everywhere ⇒ empty stencil mask ⇒ SSR renders nothing. So "wire first, roughness second" is safe: step 1 cannot visually regress, and the reflection mask only grows deliberately.
3. **Composite reduction:** reference subtracts per-pixel IBL specular; our IBL is a flat 1×1 constant cube, so that term is nearly constant → v1 composite `color += ssr.rgb * fresnelApprox * confidence * strength` turns a missing-G-buffer problem into a one-shader tuning knob. Cheap mitigation for miss-regions (sky): lerp toward the ambient/env constant where confidence ≈ 0.
4. **Execution-order constraint:** SSR consumes PostFXContext-produced reprojected depth + closest motion, so like SSAO it must run after `postFXContext->Execute` in BOTH the `taaOn` and `!taaOn` branches; radiance input is pre-TAA `sceneColorTex`; composite after the TAA resolve beside `aoCompositeApply` (target-sized 1:1, so downsample/CAS see it exactly as they see AO'd color).

## Candidate approaches

- **A. Plan-as-written + amendments:** `SsrDiligent` module mirroring `SsaoDiligent`, roughness packed into normal alpha (RoughnessChannel=3), custom composite beside aoCompositeApply. Risk: additive composite double-counts specular energy (over-bright glossy pixels) → tune fresnel/strength; sky-miss shows black unless fallback added. Effort: medium (~SSAO-shaped module + ~100-line composite PSO + 4-point settings plumbing).
- **B. Hydrogent-faithful separate material RT:** 4th world RT (roughness+metallic). Risk: +1 RT bandwidth and SetRenderTargets/PSO/both world-shader-footer churn for the same visual result today (metallic only matters later). Effort: medium-high.
- **C. Sky-fallback-first:** A but composite lerps to env color where confidence≈0. Not a separate track — fold into A's composite; the *decision* (fallback color) is the fork.
- **D. Hand-rolled SSR:** rejected — reimplements tested Hi-Z tracing/reconstruction DiligentFX already ships and whose sibling is already integrated.

## Recommended approach

A, with C's fallback folded into the composite. Everything required is verified present: exported `libDiligentFX` symbols, shared `PostFXContext` in the FEATURE_FLAG_NONE-consistent configuration, RGBA16F normal buffer with a free alpha channel, local footer override in scope of `Shading.BaseLayer.Srf.PerceptualRoughness`, and a proven SSAO-shaped template. Must be true: (a) the footer compiles `PerceptualRoughness` access for every PSO variant (UNSHADED/unlit branches need the same #if guard shape as the existing footer), (b) terrain footer writes a constant roughness — 1.0 (no rays) initially, small value only if terrain reflections are wanted, (c) the parked-scene view used for verification actually contains a low-roughness GLTF surface, else reflections won't be visible in screenshots.

## Proposed tasks

1. **SSR module skeleton + conservative wiring.** `SsrDiligent.{h,cpp}` mirroring `SsaoDiligent` (init/destroy/settingsApply/frameBegin/execute; attribs: RoughnessThreshold 0.2, MostDetailedMip 0, IsRoughnessPerceptual TRUE, RoughnessChannel 3, FEATURE_FLAG_NONE). Slot `ssrFrameBegin` beside `ssaoFrameBegin`, `ssrExecute` under `ScopedDebugGroup "ssr"` in both `taaWorldResolve` branches, `pMaterialBufferSRV = taaNormalSRV()`. Verify: build clean; settings off → frame identical to baseline; on → no crash (alpha=1.0 ⇒ empty stencil ⇒ no visual change expected).
2. **Roughness into normal alpha (GLTF footer).** One-line footer change to `float4(VSOut.Normal, Shading.BaseLayer.Srf.PerceptualRoughness)` with the UNSHADED guard; terrain keeps 1.0. Verify: SSAO screenshots unchanged (xyz-only consumers); roughness present via RenderDoc `world` pass dump of RT2.a if needed.
3. **Composite pass.** `ssrCompositeApply` PSO mirroring `aoCompositeApply` (target-sized RGBA16F, 1:1 UV): `color += ssr.rgb * fresnel * ssr.a * strength` with env-color fallback where confidence ≈ 0; call after TAA beside the AO composite, gated `ssrOn() && ssrRan` and skipped on debug-MV blit. Verify: parked-scene screenshot shows reflections on glossy surfaces; RenderDoc `ssr` group visible in `scripts/rdc.py list`.
4. **Settings plumbing + tuning.** `GraphicsSettings.ssr` (+strength/persistence), DebugGui + SettingsGraphicsGui toggles, `applyGraphicsSettings` forwarding; tune RoughnessThreshold/strength/fallback against the parked scene; clean up /tmp/RenderDoc captures.

Effort: ~1.5-2 SSAO-sized sessions. Baseline commit e7208cfbf160bedf34d0b95a76121c8d4606ed38.

## round 1

### Done (task 2)

- `SsrDiligent.{h,cpp}` created in `c-engine/renderer/diligent/`, 1:1 mirror of `SsaoDiligent`:
  `ssrInit/ssrDestroy/ssrSettingsApply(enabled,strength)/ssrOn/ssrReady/ssrFrameBegin/ssrExecute/ssrRadianceSRV/ssrStrength`.
  Attribs: `RoughnessThreshold=0.2`, `MostDetailedMip=0`, `IsRoughnessPerceptual=TRUE`, `RoughnessChannel=3`,
  rest from fxh DEFAULT_VALUEs; `FEATURE_FLAG_NONE` in PrepareResources; `EnableAsyncCreation=false`;
  `pMaterialBufferSRV = pNormalBufferSRV = taaNormalSRV()` (RGBA16F, alpha = perceptual roughness).
  `ssrExecute(ctx, colorSRV, depthSRV, motionSRV)` — color/depth/motion passed in by the caller
  (taaWorldResolve locals), normal+material fetched internally; returns `status == POST_FX_EXECUTION_STATUS_READY`.
- Wired in `TaaDiligent.cpp`: `#include`, `ssrFrameBegin(ctx)` beside `ssaoFrameBegin` in `taaFrameBegin`,
  `ssrExecute` under `ScopedDebugGroup "ssr"` after `postFXContext->Execute` in BOTH `taaWorldResolve` branches
  (taaOn: after the ssao block, before TAA attribs; !taaOn: after the ssao block).
  The `!taaOn` postFXContext block condition extended to
  `((ssaoReady() && ssaoOn()) || (ssrReady() && ssrOn()))` with the inner ssao block now gated
  `ssaoReady() && ssaoOn()` — SSR needs PostFXContext outputs even when SSAO is off.
- `DiligentRenderer.cpp`: `ssrInit()` beside `ssaoInit()`, `ssrDestroy()` beside `ssaoDestroy()`.
- CMake: `SsrDiligent.cpp` added to the skip-pch list (source list is GLOB_RECURSE, auto-picked-up).
- Default `ssrEnabled = false` — `ssrSettingsApply` is NOT yet called from `applyGraphicsSettings`
  (task 5); frame identical to baseline until then.

### Verified

- `./scripts/build.sh` clean; links against prebuilt `git/build-linux/DiligentFX/libDiligentFX.a`
  (SSR symbols present — nm count 24; NOTE: path is `diligent/git/build-linux/...`, NOT `diligent/build-linux/...`).
- Smoke runs (ENGINE_AUTOTEST=enter + ENGINE_SCREENSHOT): ssr off → exit 0; temporarily flipped
  `ssrEnabled=true`, rebuilt, ran → SSR Execute ran with zero errors/crash, exit 0, and the screenshot
  was byte-size-identical to the off run (empty stencil mask invariant holds: alpha=1.0 ≥ 0.2 threshold
  → no rays). Flip reverted, rebuilt clean. /tmp smoke shots removed.
- SSR contracts re-confirmed in source: all five RenderAttributes SRVs are DEV_CHECK_ERR-mandatory;
  `Execute` copies attribs internally on change (static attribs struct is safe, same as SSAO);
  `TRUE` comes from ShaderDefinitions.fxh C++ mode (`#define TRUE 1`).

### For later tasks

- Task 4 (composite): introduce `bool ssrRan` at top of `taaWorldResolve` (beside `ssaoRan`) capturing
  `ssrExecute`'s return in both branches; gate `ssrCompositeApply` on `!debugMv && ssrOn() && ssrRan`;
  radiance SRV = `ssrRadianceSRV()` (rgb = radiance, a = confidence); strength knob already exposed as `ssrStrength()`.
- Task 5 (settings): `ssrSettingsApply(enabled, strength)` signature ready; also decide persistence + DebugGui.
- First `ssrExecute` call compiles 7 SSR techniques synchronously — expect a one-frame hitch the first
  time SSR is enabled (same shape as SSAO's first-execute compile).

## round 2

### Done (task 3)

- GLTF PS footer (`c-engine/gltf/GltfDiligent.cpp` GetPSMainSource footer): `PSOut.WorldNormal` alpha
  now writes `Shading.BaseLayer.Srf.PerceptualRoughness` (shaded, USE_VERTEX_NORMALS branch).
  UNSHADED branch (wireframe only — nothing in c-engine sets RenderParams.Wireframe) and the
  no-vertex-normals fallback write constant 1.0. Note: the old no-vertex-normals fallback alpha 0.0
  is now 1.0 (rough ⇒ excluded from SSR; the (0,0,1) fake normal must not trace).
- Scope facts proven from DiligentFX source: `UNSHADED` never appears in RenderPBR.psh — main body
  (and local `Shading`) compile identically for all variants; the flag only strips to
  ALL_USER_DEFINED (bits 0..38, so COMPUTE_MOTION_VECTORS/USE_VERTEX_NORMALS survive) in the footer.
  `Shading.BaseLayer.Srf.PerceptualRoughness` is clamped to [0,1] by GetSurfaceReflectance.
- Terrain (`c-game/data/pak_1/materials/splat_terrain_ps.hlsl:965`): already wrote constant
  `float4(N, 1.0)` — no change needed, verified.

### Verified

- Build clean; settings.json untouched default run shape.
- **SSAO unchanged (empirical, SSAO ON via temp settings.json edit aoDisabled=false + showFps=false,
  restored after):** pre-change vs post-change footer screenshots byte-identical (A2==B2).
  Earlier HUD-polluted comparison showed diffs ONLY in the showFps cpu/gpu text — first A/B run is
  a trap unless showFps=false. SSR-off run with SSR enabled in code also byte-identical (B2==C2):
  radiance not consumed yet, as expected.
- **Alpha really carries roughness (RenderDoc, eid 1182 world RT[2] raw fp16):** 99.5% of
  valid-normal pixels = 1.00 (terrain/rough props), ~0.5% = 0.50–0.54 (eve's materials), min 0.50.
  Not constant ⇒ write is live, not optimized away.
- SSR executed with real alpha, zero errors, exit 0. `scripts/rdc.py list` shows the `ssr` group
  (world eid 808-1182, ssao 1225-1349, ssr 1356-1446).

### For later tasks

- **Task 6 visibility caveat:** parked scene has min perceptual roughness 0.50 > RoughnessThreshold
  0.2 ⇒ SSR stencil is empty here; no reflections will be visible from this vantage. Either ask the
  user to park at a glossier surface, or raise RoughnessThreshold (e.g. 0.45-0.6) for bring-up.
- `ssrEnabled` remains default false; task 5 owns ssrSettingsApply wiring.
- 1.6 GB /tmp/RenderDoc capture + dumps cleaned; settings.json restored (showFps=true,
  aoDisabled=true as user had it).

## round 3

### Done (task 4)

- `ssrCompositeApply` implemented in `TaaDiligent.cpp`, 1:1 mirror of `aoCompositeApply`:
  inline HLSL PS `ssrCompositePS` on the shared `blitVS`, POINT clamp sampler (1:1 target-sized
  composite), static CB + dynamic SRBs keyed on src texture (cache cleared in destroyTargets).
  New `ssrCompositeTex` (target-sized RGBA16F) created in createTargets / released in destroyTargets;
  `ssrCB` via CreateUniformBuffer in taaInit; PSO/PS/sampler/CB released in taaDestroy; `ssrSrbs` cleared.
- Shader math: `conf = ssr.a`; `fresnel = 0.04 + 0.96 * pow(saturate(1 - g_Normal.a), 2)` (roughness-
  weighted specular weight from the normal-buffer alpha — no view-vector reconstruction, chosen for
  v1 simplicity/risk); `reflected = lerp(c.rgb * EnvFallback, ssr.rgb, conf)`; `c.rgb += reflected *
  fresnel * conf * Strength`. EnvFallback = 1.0 constant in the CB (tunable knob for task 5).
  At conf = 0 the added term is exactly 0 (strict no-op — existing env IBL in the base color is what
  you see; the fallback can only ever blend toward it, never brighten by accident).
- Wiring in `taaWorldResolve`: `bool ssrRan` beside `ssaoRan`, captured from `ssrExecute`'s return in
  BOTH branches; composite call after the AO composite, gated `!debugMv && ssrOn() && ssrRan`,
  inputs `(srcColorSRV, ssrRadianceSRV(), taaNormalSRV())`. No debug group (mirrors aoComposite).

### Verified

- Build clean; default-off run exit 0 (pass skipped).
- Temp flips (ssrEnabled=true, threshold 0.2→0.6) + RenderDoc frame-300 capture prove end-to-end:
  `ssr` group present (eids 1209-1299), `ssrComposite` draw at eid 1319 with `cbSsrCompositeAttribs`
  + g_Source/g_SSR/g_Normal bound. fp16 replay math: 14.5k px receive additive term, max delta 0.027
  == radiance(≤0.112) * fresnel * conf(≤1) * strength(1.0); conf=0 pixels EXACT pass-through.
  Zero validation errors, exit 0, 60 fps / 2.47 ms gpu in the enabled-run HUD.
- IMPORTANT: the parked vantage DOES have SSR-eligible pixels (conf>0.9 on ~13.7k px at threshold
  0.6) — round 2's "no glossy surface in frame" was wrong for threshold 0.6; something rough ≤ 0.6
  IS in frame (small, ~0.3% of px). Traced radiance there is dim (≤ 0.112) because the reflected
  content is distant terrain/sky, hence the barely-visible effect.
- All temp flips reverted (ssrEnabled=false, threshold 0.2), rebuilt, re-smoked exit 0.
  1.5 GB capture + dumps + /tmp shots removed.

### For later tasks

- Task 5: `EnvFallback` float already in the CB if settings want it; `ssrStrength()` feeds Strength.
- Task 6: threshold 0.6 makes the parked view eligible (small dim reflections); a glossier park or a
  brighter env would show SSR better. The fresnel weight dims rough-0.5 pixels to ~0.28× radiance.
- PNG-based A/B undercounts: the composite's additions are fp16-scale (≤1-2 8-bit levels) — verify
  via GetTextureData fp16, not rdc.py PNG dumps (lesson applied above).

## round 4
Task 5 done: SSR settings plumbing end-to-end, mirroring the SSAO idiom.

- GraphicsSettings gained `bool ssr = true` + `float ssrStrength = 1.0f` (c-engine/renderer/Renderer.h); normalized 0..2 in graphicsNormalize; loaded in rendererGraphicsLoad from new keys `ssrDisabled` (inverted polarity, like ao/bloom/gi) + `ssrStrength` (Renderer.cpp).
- New settings templates in c-utils/settings/Settings.cpp: `ssrDisabled` boolean 0, `ssrStrength` double 1.0. Settings templates are REQUIRED for any new key — missing keys are seeded in memory at settingsInit (file catches up on next settingsWrite), but un-templated keys would trip settingsGet* asserts.
- DiligentRenderer applyGraphicsSettings now calls `ssrSettingsApply(s.ssr, s.ssrStrength)` after ssao — the round-1 statics (`ssrEnabled=false`) are now driven by real settings: SSR is ON with strength 1.0 by default at startup.
- DebugGui: the previously inert `reflectionEnabled` stub + `debugToggleReflection` are now real (drive GraphicsSettings.ssr via rendererGraphicsApply, like toggleAo); added "Reflections" button to debug.html LIGHTING section (pak_0_engine — rebuilt by build.sh).
- SettingsGraphicsGui: `ssrEnabled`/`ssrStrength` statics, `ssrLabel`, `toggleSsr` (persists ssrDisabled immediately), `ssrStrengthChange` slider with new `dirtySsr` in the deferred apply/persist path (persists ssrStrength), autotest `wire` now also flips SSR off (idempotent loop). graphics.html (pak_1) gained "Screen Space Reflections" toggle + "Reflections — Strength" slider (0..2, step 0.05) between AO-Intensity and GI.
- Verified: build clean; headless run boots exit=0 (settings keys seeded, no assert); ENGINE_AUTOTEST=settings + ENGINE_SETTINGS_AUTOTEST=graphics renders the Graphics page with "Screen Space Reflections: On" / "Reflections — Strength (1.00)"; ENGINE_GRAPHICS_SETTINGS_AUTOTEST=close closes via real BACK path with no RML errors; /tmp/ssr_graphics_page2.jpg shows the page.
- Task 6 (runtime visual check) can flip SSR via DebugGui "Reflections" button or the settings page; setting `ssrStrength` to 0 is a neutral A/B baseline.

## round 5

### Done (task 6 — final runtime check)

- Temp-flipped `RoughnessThreshold` 0.2→0.6 in `SsrDiligent.cpp` (proven eligible value for the
  parked view), rebuilt, and ran the full check. **All pass:**
- **Plain ENGINE_SCREENSHOT run (SSR on, threshold 0.6):** exit=0, clean shutdown, zero validation
  errors; HUD: 60.00 fps / 16.67 ms frame / 2.56 ms cpu / **2.36 ms gpu** — sane (vsync-bound,
  matches round 3's 2.47 ms). Parked player/camera untouched.
- **RenderDoc frame-300 capture + `scripts/rdc.py list`:** `ssr` group present (eids 1209-1299,
  output RGBA16F 2880x1627) with the full DiligentFX sub-chain nested (ComputeHierarchicalDepthBuffer,
  ComputeStencilMaskAndExtractRoughness, ComputeIntersection, SpatialReconstruction,
  ComputeTemporalAccumulation, ComputeBilateralCleanup).
- **fp16 replay of the final SSR radiance target (eid 1299, ResourceId::1297):** 20,913 px (0.446%)
  conf>0; 17,897 px conf>0.5; **15,807 px conf>0.9**; conf range 0..0.9995. Radiance dim-but-real:
  luminance @conf>0 max 0.0779 / mean 0.0337 / median 0.0376; rgb max 0.097/0.079/0.090 — consistent
  with rounds 2-3 (reflected content = distant terrain/sky). Judged via fp16, not 8-bit eyeball, per
  the task note. conf==0 & rgb>0.001 is only 67 px (bilateral edge halo, max rgb 0.08) — composite
  multiplies by conf so those add exactly 0.
- **Restored threshold to 0.2**, rebuilt, smoke run exit=0, no validation errors. Default shipped
  state: SSR on, threshold 0.2, strength 1.0 — visually inert from this vantage (min roughness 0.50
  > 0.2 ⇒ empty mask), conservative as designed. settings.json never modified (ssrDisabled=false,
  ssrStrength=1.0 as round 4 shipped). 1.5 GB capture + /tmp artifacts removed.
- Replay API gotchas hit (recurring): output targets only exist on real draw eids (pass-boundary eid
  gives empty GetOutputTargets); Descriptor field is `.resource`, not `.resourceId`, in this SWIG build.

### For later work

- SSR is verified end-to-end and shipped default-on. To actually SEE reflections from a screenshot,
  either park at a glossier surface or raise the default threshold — the fresnel weight (0.04 +
  0.96*(1-rough)^2) also dims rough-0.5 pixels to ~0.28x of the already-dim radiance. Brighter env /
  closer reflected geometry would make the effect visible at 8-bit.

## sign-off
Round 5 verifier PASS + task.md re-read: SSR from DiligentFX is implemented end-to-end (module, inputs, composite, settings, runtime proof). Signed off at round 5 of 10. Open question left for future tuning: visibility from the parked vantage at default threshold 0.2 is inert (park min roughness 0.50); raise threshold (~0.6 proven eligible), park at a glossier surface, or brighten env to see it at 8-bit.

## user report (round 6)
User: sky reflects on terrain water, but the player character's reflection is missing. Manager root-cause pass: splat_terrain_ps.hlsl:965 `Out.WorldNormal = float4(N, 1.0)` — terrain material buffer alpha constant 1.0, so SSR never traces from any terrain pixel; the visible sky sheen on water is the terrain PS's own IBL specular (`g_PrefilteredEnvMap` sample of reflect(-V,N) at line ~795), a cube probe that cannot contain the character. Real per-pixel roughness exists in the PS (`roughness`, line ~780, clamp 0.04..1.0, GGX-alpha convention via albedo.a bake + splat blending) but is not exported. Sign-off round 5 reopened; rounds continue at 6/10.

## round 6

### Done (task 7 — terrain roughness into WorldNormal.a)

- `splat_terrain_ps.hlsl:965`: `Out.WorldNormal = float4(N, 1.0)` → `float4(N, roughness)`,
  plus the stale outputs doc-comment updated. `roughness` is in scope at the write (both in
  `PSOutput main()`, declared at line ~780 as `clamp(roughnessS, 0.04, 1.0)`).
- **DEVIATION from the task text (important): NO sqrt applied — wrote `roughness` directly.**
  The task said the PS roughness is "GGX-alpha convention" needing sqrt→perceptual. That is
  wrong for this shader: its `roughness` behaves exactly like DiligentFX `PerceptualRoughness`
  (proven line-by-line vs PBR_Shading.fxh): LUT coord `float2(NdotV, roughness)` = fxh:291,
  Schlick `1.0 - roughness` = fxh:261, env mip `roughness * rCam.w` = fxh:322, and it is the
  shader itself that squares it (`AlphaRoughness = roughness * roughness` = the canonical
  fxh/PBR_PrecomputeCommon.fxh:29 chain). sqrt would have written sqrt(perceptual) and
  understated gloss. The write matches the gltf footer (`PerceptualRoughness` direct) and
  SSR's `LoadRoughness` contract (channel holds perceptual when IsRoughnessPerceptual=TRUE —
  verified in SSR_ComputeStencilMaskAndExtractRoughness.fx: sqrt only fires when the flag is
  false).

### Verified

- build.sh clean; pak_1 re-zipped (md5-stamped by data.sh); runtime-compiled shader loads, no
  compile errors, run exit 0; 60 fps / 2.81 ms gpu (vs 2.36 ms round 5 — SSR now actually
  traces). Screenshot clean, no artifacts. Parked player/camera untouched; settings.json never
  modified (SSR on, threshold 0.2 as shipped).
- RenderDoc frame-300 fp16 replay: world RT2 alpha now min 0.0400 (the clamp floor) / mean
  0.7676 / 5.33% of valid px < 0.2 — real variation, no longer constant 1.0. SSR radiance:
  **67,244 px conf>0** (61,488 > 0.5; 56,894 > 0.9) vs ~0 terrain-sourced pre-fix (terrain was
  alpha 1.0; gltf min 0.50 could not pass threshold 0.2) — terrain pixels now trace.
- SSAO unaffected: reads .xyz only (proven round 2 A/B); no other consumer of the normal
  buffer alpha exists except the SSR composite fresnel weight (its intended input — water
  pixels now get the full roughness-weighted weight instead of the constant-0.04 floor).
- Capture (~1.5 GB) + dumps + /tmp shots cleaned.

### For task 8 (runtime character-reflection check)

- 182,893 px in the round-6 capture frame sit below threshold 0.2 already — eligibility exists
  at the SHIPPED threshold from this vantage; no temp flip needed for a basic check. The water
  vantage may still want the user's parked camera framing (do not move it).
- Fresnel weight note: composite weight 0.04 + 0.96*(1-rough)^2 ≈ 0.83 at roughness 0.1 —
  water reflections should now be clearly visible at 8-bit, unlike rounds 2-5's dim gltf-only
  adds. Watch for over-bright water (additive on top of the existing IBL sheen) as the tuning
  follow-up.

## round 7

### Done (task 8 — runtime character-reflection verification) — ALL PASS

- **A/B proof (RenderDoc frame-300 pair, parked camera):** SSR ON vs OFF (`ssrDisabled` temp flip,
  settings.json byte-restored, md5 b75625dad44a44cb702d086ef62f6060). Eve's mirrored silhouette IS
  visible in the puddle below her feet in ON, absent in OFF. Evidence kept in /tmp:
  `task8_ab_on_below_eve.png` / `task8_ab_off_below_eve.png` (leg reflections vs flat sheen).
- **fp16 replay (ssr group eids 1209-1299, radiance ResourceId::1297, composite eid 1319
  g_Source/1161 + g_SSR/1297 + g_Normal/1113):**
  - Stencil is EXACTLY the water set: all 67,328 conf>0 px have WorldNormal.a roughness < 0.2
    (0 px pass on rough>=0.2). Water (rough<0.2) = 182,944 px total (matches round 6 exactly).
  - Radiance NEAR THE CHARACTER IS THE CHARACTER: normalized radiance crop of the below-eve
    puddle shows her two legs/boot highlights as a silhouette (not sky/terrain colors); left-mid
    puddle field reflects the sunlit hill (warm 0.31/0.24/0.15, conf mean 0.93), sky blue
    (0.02/0.04/0.09) nowhere in the traced radiance.
  - Over-brightening: bounded. Below-eve puddle in-lum 0.098 -> out 0.109 (x1.12); left field
    x2.68 (reflected sunlit hill, out max 0.619 < 1.0); right-mid x1.03; bottom-right x1.29.
    FRAME-WIDE mean lum +1.7% only; zero NEW clipped px (218 px >1.0 were already >1.0 sun
    glints, max 14.72 unchanged — SSR never adds to them).
- Both capture runs exit 0, zero validation errors; ON HUD 60fps / 2.34ms gpu (vsync-bound).
- Parked player/camera untouched (db rows decoded pre/post: camera -957.27,514.42,1425.88
  yaw -1.73 pitch 0.15; player -954.34,513.77,1426.34). 2x 1.5GB captures + all npy/png temp
  artifacts deleted.

### Operational discovery (important for future screenshot runs)

- **ENGINE_SCREENSHOT runs do NOT use the parked camera.** FlyingCamera.cpp:169 automation gate
  (ENGINE_SCREENSHOT/ENGINE_CAMERA/ENGINE_CAMERA_DOLLY/ENGINE_NO_PLAYER) skips the saved-view
  restore — "the scripted camera owns the view" = Game.cpp default vantage framing the WORLD
  SPAWN point (-500,513,164), eye = spawn+(180,75,180) = (-320,588,344). The db-parked camera
  is restored only in NON-gated runs — ENGINE_RENDERDOC_CAPTURE is NOT in the gate, which is
  why every round 2-6 RenderDoc capture showed the parked view. So: visual verification of the
  parked vantage MUST go through RenderDoc captures (dump the final backbuffer), not
  ENGINE_SCREENSHOT. Also ENGINE_SCREENSHOT_FRAME=100 (default) fires before db load anyway.
- Player spawn/db load is NOT gated (player always at the park); ENGINE_CAMERA vantages
  (topdown/close/ground/cast/shadow/character) all frame spawnPt, not the parked player.
- RenderDoc replay gotchas (recurring): TextureDescription has no `.name`; UsedDescriptor has
  no `.name` — pair `GetShaderReflection(stage).readOnlyResources[i].name` with
  `GetReadOnlyResources(stage)[i].descriptor.resource` by index; output targets only exist on
  draw eids; `.resource` not `.resourceId` on Descriptor.
- sqlite probing trap: `sqlite3.connect(path)` CREATES the file — two stray 0-byte dbs were
  accidentally created during this round and deleted. Real db is build/c-game/data/db/db.db
  (tables `camera`, `player`, not `transform`; camera blob = pos3+yaw+pitch f32).

### Task 8 verdict

Character reflection in water: VERIFIED end-to-end (stencil on water, character content in
radiance, visible in A/B, no over-brightening regression). SSR shipped state confirmed good:
threshold 0.2, strength 1.0, on. No tuning change needed; the left-field hotspot (x2.68) is
physically plausible (Fresnel-weighted mirror of a bright hill) and well under clipping.

## sign-off (reopened run)
Round 7 verification proves the user-reported gap is fixed at the shipped config (threshold 0.2, strength 1.0, on): SSR stencil == water pixels, character silhouette present in puddle radiance + A/B screenshots. 7 rounds of 10 used, all verifier verdicts PASS (6 build verifications; round 7 verification-only, no code change). Tuning knob if wet patches feel bright in motion: ssrStrength slider (Settings > Graphics) / ssrStrength setting. Operational note for future visual checks: ENGINE_SCREENSHOT runs bypass the parked camera (FlyingCamera.cpp:169 automation gate) — use ENGINE_RENDERDOC_CAPTURE for parked-view evidence.
