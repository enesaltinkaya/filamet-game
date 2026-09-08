# notes

Context: previous run of the same task committed the SSR gbuffer terrain pass (SsrDiligent.cpp/h, GBUFFER_OUTPUT hlsl branch, ssr_gbuffer pass in DiligentRenderer). Prior-round notes archived in notes-2026-09-08-prev.md. Repro screenshot from that run: repro_baseline.jpg in the ledger dir.

## brainstorm

## Core difficulty

Not the ray marching — DiligentFX `ScreenSpaceReflection` is a drop-in post effect already compiled into `libDiligentFX.a` (PostProcess is an unconditional subdirectory). The real difficulty is plumbing: it needs five per-frame inputs (color, depth SRV, normal, roughness, NDC motion) plus a `PostFXContext::Execute` that currently happens *inside* `taaWorldResolve` — i.e. after the point where SSR must run — so the TAA resolve flow has to be re-sequenced, and the SSR radiance then has to be composited back into the chain before the TAA/blit.

## Reductions / key lemmas

1. Every required input already exists at the right format/convention. SSR (per `DiligentFX/PostProcess/ScreenSpaceReflection/README.md`) wants: HDR color (we have the linear RGBA16F `sceneColorTex`), single-float depth SRV (`taaDepth[i]` are D32_FLOAT with BIND_SHADER_RESOURCE — only a missing SRV accessor), world-space normals in [-1,1] (gbuffer rgb is the normalized shading normal — already in range), roughness in a selectable channel (`RoughnessChannel` is channel-indexed via `.Load` in `SSR_ComputeStencilMaskAndExtractRoughness.fx` — alpha = channel 3 works; `IsRoughnessPerceptual=true` since the shader stores artist roughness, not squared — the same gbuffer SRV can serve as *both* `pNormalBufferSRV` and `pMaterialBufferSRV`), and NDC motion vectors (TAA's RG16F motion buffer uses the exact PBR/PostFXContext convention SSR shares).
2. `PostFXContext::PrepareResources` (per-frame) is already called in `taaFrameBegin` with the shared camera-attribs CB; `ScreenSpaceReflection::PrepareResources` only needs that context. So SSR is created once (engine-init, behind `ENGINE_SSR`) and per-frame only needs its `PrepareResources` + `Execute`.
3. Ordering constraint: SSR's `Execute` consumes `PostFXContext::Execute` outputs (blue noise etc.), but that call is buried in `taaWorldResolve`. Hence the mandatory refactor: hoist `postFXContext->Execute(pa)` out of `taaWorldResolve` into an explicit `taaPostFXExecute(ctx)`; new frame order: world passes → `ssr_gbuffer` → `taaPostFXExecute` → [SSR: prepare + execute + composite] → `taaWorldResolve` (now without its own Execute call).
4. Terrain roughness is ~0.6 (snow) to 0.9 (grass/beach) at the end of the material chain in `heightmap_terrain_ps.hlsl` (after the `lerp(roughness, 0.6, snowT)` / `lerp(..., 0.85, beachT)` lines, before the GBUFFER_OUTPUT block that writes `float4(N, roughness)`). SSR's default `RoughnessThreshold` is 0.2, so a temporary global scale of ~0.15 (≈0.09–0.14 effective) puts all terrain under the threshold — and it hits the main PBR path too (same variable feeds `GetSurfaceReflectanceMR`), which is what makes the effect visible. Revert = delete one scale line; exact hunk goes in the ledger.
5. SSR keeps its own radiance/variance history (its own temporal accumulation + bilateral cleanup), reset by frame-index discontinuity/resize — same robustness profile TAA already proved out here.

## Candidate approaches

A. **Full DiligentFX SSR + confidence-composited radiance.** Instantiate `ScreenSpaceReflection` behind `ENGINE_SSR`; per-frame feed (scene color, taaDepth SRV, gbuffer SRV ×2, motion SRV, default `ScreenSpaceReflectionAttribs` + reduced-roughness-aware threshold); composite `sceneColor + confidence * radiance` via a small fullscreen-quad PS into a temp RGBA16F, and pass that SRV to `taaWorldResolve` as the source override (new optional param). Risk: the compositing is physically naive (no Fresnel/T2 split-sum, so SSR adds specular radiance uniformly scaled by confidence) — acceptable for a temporary look check, but the additive term can bloom out if untempered (tune a small gain, e.g. `0.5*confidence`). Effort: ~250–350 lines C++ + one trivial PS, plus the TAA re-sequencing.

B. **SSR debug-view first, composite second.** Wire exactly like A but instead of compositing, blit `GetSSRRadianceSRV()` (and optionally the gbuffer) to the backbuffer behind a second env var (`ENGINE_SSR_DEBUG=1`). Lower risk for validating the plumbing (input formats, camera/depth conventions) before trusting the composite, but it doubles the screenshot-validation round-trips. Effort: A + ~40 lines.

C. **Hand-rolled single-pass raymarch PS** (skip DiligentFX). Full control, no context juggling — but re-implements hi-z, VNDF, denoise for a temporary experiment. Rejected: days of work for a look check.

D. **FEATURE_FLAG_HALF_RESOLUTION / PREVIOUS_FRAME variants of A.** Half-res ray tracing cuts the raymarch cost; PREVIOUS_FRAME changes which color buffer it samples. These are one-enum changes on top of A — keep as tuning knobs, not separate paths.

## Recommended approach

A (with the debug-blit from B available cheaply as a fallback since the SRV is one variable swap). It reuses 100% of the already-validated machinery (PostFXContext, camera CBs, motion convention, gbuffer) and isolates all new surface area in SsrDiligent.cpp plus a two-line TAA refactor; the naive confidence-weighted composite is the right amount of physics for "can we see SSR working" and reverts cleanly. Must be true for it to work: (1) `taaDepth[i]` SRVs are legal to sample at target size (they are, BIND_SHADER_RESOURCE); (2) the gbuffer pass has already run this frame against the *current* depth texture (it does — it depth-tests against `taaDepthDSV`); (3) SSR's frame-index continuity keeps holding across the engine's existing frame flow (same guarantee TAA relies on).

## Proposed tasks

1. TAA re-sequencing + accessors: hoist the `postFXContext->Execute(pa)` block out of `taaWorldResolve` into a new exported `taaPostFXExecute(ctx)` (taaWorldResolve calls it internally when SSR isn't driving it), and add small accessors to TaaDiligent: `taaDepthSRV(u32 frameIdx)`, `taaMotionSRV()`, `taaColorSRV()`. Verify: build + headless screenshot with ENGINE_SSR unset is pixel-unchanged vs baseline.
2. SSR effect wiring: in SsrDiligent, create `ScreenSpaceReflection` (lazy, `ENGINE_SSR`), per-frame `PrepareResources(device, ctx, postFXContext, flags)`, and a new `ssrDiligentExecute(ctx)` that fills `RenderAttributes` (color = taaColorSRV, depth = taaDepthSRV(curr), normal = gbuffer SRV, material = same gbuffer SRV, motion = taaMotionSRV) with `RoughnessChannel=3`, `IsRoughnessPerceptual=true`, defaults otherwise; expose `ssrRadianceSRV()`. Verify: build; ENGINE_SSR=1 headless run reaches `taa_resolve` without new warnings (SSR still not composited — its output only visible via RenderDoc).
3. Composite pass + frame ordering: fullscreen-quad PS writing `scene + gain*conf * ssrRadiance` (conf = radiance.a) into a temp RGBA16F target sized with the offscreen chain; give `taaWorldResolve` an optional source-SRV override; reorder DiligentRenderer to world → ssr_gbuffer → taaPostFXExecute → ssrExecute+composite (only when `ENGINE_SSR`) → taaWorldResolve(compositeSRV). Verify: `ENGINE_SSR=1 ENGINE_SCREENSHOT` screenshot shows reflections vs baseline; RenderDoc pass list shows the SSR passes.
4. Temporary roughness reduction + cleanup: insert one line scaling `roughness` (~×0.15) in `heightmap_terrain_ps.hlsl` right after the snow/beach lerps (covers both GBUFFER_OUTPUT and the PBR path); record the exact line in the ledger for revert; delete the leftover `ssrdbg` fprintfs in `ssrDiligentDestroy`. Verify: build + final `ENGINE_SSR=1` screenshot; reflections visibly land on terrain.

## round 1

- Task 1 (TAA re-sequencing) done. `TaaDiligent.cpp/h`:
  - `taaPostFXExecute(ctx)` is now the single place that runs `postFXContext->Execute(pa)` for the current `frameIdx` (camera CB + curr/prev depth SRVs + motion SRV, same attrs as before). It sets a per-frame flag; `taaFrameBegin` resets it each frame. `taaWorldResolve` calls `taaPostFXExecute` internally only when the flag is clear, so the no-SSR frame sequence is byte-for-byte the same GPU work at the same point.
  - New accessors: `taaDepthSRV(u32 frameIdx)` (masks `& 1` like `taaDepthDSV`), `taaMotionSRV()`, `taaColorSRV()`.
  - `taaPostFXExecute` is deliberately NOT gated on `taaOn` — SSR needs PostFXContext output (blue noise etc.) even with TAA off; the internal call from `taaWorldResolve` keeps the old `taaOn` gate.
- Verified: clean build; headless screenshot runs OK with ENGINE_SSR unset.
- Pixel-identity caveat: the engine is NON-deterministic across runs (two runs of the SAME binary differ in ~3.3M px, whole screen, at frame 3 — async load/animation state). So screenshot A/B vs a pre-change binary cannot prove pixel identity. Identity holds structurally: nothing calls `taaPostFXExecute` yet, so `taaWorldResolve` runs Execute exactly as before. For later tasks, screenshot comparisons are indicative only (compare scene regions, expect UI/character-region noise); RenderDoc dumps are the reliable tool.
- Screenshot recipe that works (run.sh fails without TERM; `clear` + set -e): run the binary directly:
  `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json ENGINE_AUTOTEST=enter ENGINE_HIDDEN_WINDOW=1 ENGINE_SCREENSHOT=/tmp/x.png timeout -s KILL 30 ./build/c-game/c-game`
  (`.png` for lossless, `.jpg` for JPEG q90; one-shot at frame 3).
  NOTE: `ENGINE_LOG_TIMEOUT` is in MILLISECONDS here (`atof*1e6` ns in Engine.cpp) — do not pass small "seconds" values.

## round 2

- Task 7 + 5 done. Shutdown SEGV root cause (verified against the Diligent source in cpp-thirdparty/diligent/git):
  - In this Diligent build, `ITexture::GetDefaultView` returns the view **without AddRef** (interface doc in `Graphics/GraphicsEngine/interface/Texture.h`: "does not increase the reference counter ... Release() must not be called"), and default views **share the texture's refcounter** (`TextureVkImpl::CreateViewInternal` passes `this` as the view's refcounters for default views).
  - So `gbufferRtv` was a non-owning alias of the texture's single ref (refcount = 1, held by the `gbufferTex` global). `ssrDiligentDestroy`'s `gbufferRtv->Release()` dropped the shared counter 1→0, destroying the texture (and both default views), and the subsequent `gbufferTex->Release()` ran on freed memory — corrupted-vtable SEGV, exactly at the old line 180. Same latent double-release on the resize path in `createGbufferTexture`.
- Fix in `SsrDiligent.cpp`: never Release default views — both spots now just null `gbufferRtv` and Release `gbufferTex` (the texture's destructor runs `DestroyDefaultViews`). Pipeline/shader releases are correct and kept (gbufferPS holds the explicit `AddRef` from `createGbufferPS`; gbufferPipeline owns 1 ref from CreateGraphicsPipelineState).
- Task 5: all `ssrdbg` fprintfs removed, now-unused `#include <cstdio>` dropped.
- Verified: build clean; 3x `ENGINE_SSR=1` headless runs exit 0 with full "renderer: diligent device released" (previously exit 139/coredump after "rtv released"); no-SSR run also exits 0; screenshots written (~470 KB each).
- WARNING for task 2: `ssrGbufferSRV()` returns a NON-OWNING default-view pointer — consumers must not Release it and must NOT stuff it into a `RefCntAutoPtr`/`RefCntAutoPtrDerefAdd` (that would repeat this exact crash); use raw `ITextureView*` bindings only. `taaDepthSRV`/`taaMotionSRV`/`taaColorSRV` from round 1 return views the caller DOES own (check TaaDiligent before binding similarly) — verify their ownership before putting them in RefCntAutoPtr too.

## round 3
- Task 2 (SSR effect wiring) done:
  - `TaaDiligent.h/cpp`: new accessors `taaPostFXContext()` (returns the owned PostFXContext raw ptr) and `taaFrameIndex()` (engine frame counter; `& 1` selects the current depth buffer).
  - `SsrDiligent.h/cpp`: `ssrDiligentExecute()` — lazily creates `ScreenSpaceReflection` (FEATURE_FLAG_NONE, synchronous PSO creation like TAA) on first ENGINE_SSR frame; per frame: `taaPostFXExecute(context)` (idempotent flag-guard) so this frame's blue noise / reprojected-depth context outputs exist before the effect runs, then `PrepareResources(device, context, pctx, NONE)` + `Execute` with color=`taaColorSRV()`, depth=`taaDepthSRV(taaFrameIndex())` (current parity), normal=material=`ssrGbufferSRV()` (same RGBA16F gbuffer: xyz world normal + a artist roughness), motion=`taaMotionSRV()`, `RoughnessChannel=3`, `IsRoughnessPerceptual=true`, all other attribs at fxh defaults. `ssrRadianceSRV()` returns `GetSSRRadianceSRV()` (output RGBA16F, rgb radiance + a confidence).
  - `DiligentRenderer.cpp`: `ssrDiligentExecute()` called in a new `ssr` ScopedDebugGroup right after the `ssr_gbuffer` group (before `taa_resolve`); not composited yet — task 3 owns the composite + final frame order.
- Ownership audit (round-2 critical): `taaColorSRV`/`taaMotionSRV`/`taaDepthSRV` all return `GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)` — NON-OWNING default views, same as `ssrGbufferSRV`. Consumers must never Release them and must never wrap them in RefCntAutoPtr/RefCntAutoPtrDerefAdd. Raw-pointer binding (RenderAttributes, SRB Set) is correct. `ssrRadianceSRV()` is likewise non-owning (ResourceRegistry holds the ref).
- Verified: build clean. ENGINE_SSR=1 headless run exits 0, screenshot written. RenderDoc frame-300 capture (`/tmp/RenderDoc/c-game_frame300.rdc`, kept for task 3) pass list shows the full chain: `ssr_gbuffer` (153-210) → `ssr` group: `PreparePostFX` (blue noise, reprojected/prev depth, closest motion) then `ScreenSpaceReflection`: ComputeHierarchicalDepthBuffer → ComputeStencilMaskAndExtractRoughness → ComputeIntersection → SpatialReconstruction → ComputeTemporalAccumulation → ComputeBilateralCleanup (212-341), final radiance output R16G16B16A16_FLOAT 2880x1627; dump at eid 341 = /tmp/rdc-dump/event341_eid341_out0.png (content present; 16F clamps 0-1 on save so it looks dark, expected). No SSR pass before `taa_resolve` runs twice: PostFXContext::Execute happens once (first caller wins via postFXExecutedThisFrame), now inside the `ssr` group — `taa_resolve` still shows its own TemporalAccumulation nested (the TAA Execute is separate from the context Execute).
- Note for task 3: the SSR blue-noise pass (PreparePostFX) now renders BEFORE the ssr group — with TAA enabled, `taaPostFXExecute` from inside `ssrDiligentExecute` is the first (and only) caller; `taaWorldResolve` re-checks the flag. Task 3 just needs to move the explicit `taaPostFXExecute` call up and add the composite between `ssr` and `taa_resolve`.
- `HLSL::ScreenSpaceReflectionAttribs` lives in `Shaders/PostProcess/ScreenSpaceReflection/public/ScreenSpaceReflectionStructures.fxh` (needs ShaderDefinitions.fxh first, same include pattern as TaaDiligent.cpp); defaults: DepthBufferThickness 0.025, RoughnessThreshold 0.2 (task 4's ~x0.15 roughness scale puts all terrain under it).

## round 4
- Task 3 (composite + frame reorder) done:
  - `SsrDiligent.cpp/h`: new fullscreen-quad composite pass. Inline HLSL VS (SV_VertexID fullscreen triangle, same source as TaaDiligent's kBlitVS) + PS `out = scene.rgb + gain * ssr.a * ssr.rgb` (conf = radiance alpha) compiled at runtime; writes a temp `ssrComposite` RGBA16F texture sized with `taaTargetSize`. Exported `ssrDiligentComposite()` (returns non-owning default SRV — never Release, never RefCntAutoPtr-wrap) and `ssrDiligentEnabled()`. Gain is baked into the PS source string at first compile via `ENGINE_SSR_GAIN` (default 0.5) — per-process knob, no CB plumbing. SRB cached keyed on (sceneTex, radianceTex) textures; recreated if either changes.
  - `TaaDiligent.h/cpp`: `taaWorldResolve(ctx, backRTV, srcOverride = nullptr)` — override replaces the scene-color SRV; everything downstream (TAA accumulation, box downsample, RCAS, blit) consumes it unchanged.
  - `DiligentRenderer.cpp` final frame order: world → `ssr_gbuffer` → `taaPostFXExecute` (only when `ssrDiligentEnabled()`, keeps no-SSR path byte-identical) → `ssr` (effect exec, its internal taaPostFXExecute is flag-guarded idempotent) → under `worldDrewThisFrame`: `ssr_composite` → `taa_resolve` with composite SRV (nullptr when SSR off).
- Verified: build clean. ENGINE_SSR=1 headless run: zero ssr/composite warnings, exit 0. RenderDoc frame-300 pass list confirms order: ssr_gbuffer (157-214) → ssr (255-383) → ssr_composite (388-395, output RGBA16F 2880x1627) → taa_resolve (398-414). `scripts/rdc.py dump ssr_composite` saved /tmp/rdc-dump/ssr_composite_eid395_out0.png (16F clamps 0-1 on PNG save, looks dark — expected, same as round-3 radiance dump). Screenshots at /tmp/t3_ssr.png vs /tmp/t3_nossr.png differ substantially, but per round-1 the engine is non-deterministic run-to-run so that is NOT evidence of the effect landing — RenderDoc is the ground truth; final visual check is task 6's job (after task 4's roughness cut).
- Pitfalls hit (rule-first): (1) `ShaderCreateInfo.SourceLanguage` MUST be set to `SHADER_SOURCE_LANGUAGE_HLSL` for inline shaders — default is GLSL and the failure is a cryptic glslang parse error ("0:14 unexpected IDENTIFIER") that looks like a source bug. (2) glslang HLSL requires `};` after struct definitions — a missing semicolon after the PS's input struct parses as garbage ("Expected ; / Expected declaration" on the main() line, one line PAST the struct). The error lines point at the NEXT statement, not the missing token. (3) The engine logs the shader error blob via Diligent's log but truncates/empties the source context — to see full glslang output, capture the `IDataBlob` from `CreateShader(ci, &shader, &blob)` (note: `IDataBlob` here has `GetConstDataPtr`, no `GetContentSize`).
- Note for task 4/6: composite adds SSR radiance into the TAA-accumulated source (feed-forward), so TAA integrates the composite; if reflections flicker temporally after the roughness cut, first suspect SSR temporal accumulation (its own history) rather than TAA.

## round 5

## REVERT: roughness

Temporary look change (task 4) — delete the marked line to revert; no other changes in this file.

File: `c-engine/renderer/diligent/shaders/heightmap_terrain_ps.hlsl` (the `c-game/data/pak_1/materials/heightmap_terrain_ps.hlsl` copy is regenerated from it by `scripts/build.sh`'s pak step — edit the shaders/ copy; both were edited for consistency)

Exact inserted hunk (after the snow/beach lerps, formerly lines 458-459):

```
     roughness = lerp(roughness, 0.6, snowT);
     roughness = lerp(roughness, 0.85, beachT);
+    roughness *= 0.05;
+    // Keep the terrain glossy so the screen-space reflection reads: a matte
+    // dielectric spreads the GGX lobe into an imperceptible sheen, so SSR
+    // needs a smooth (low-roughness) surface to show a visible reflection.
+    roughness = min(roughness, 0.08);
```

Revert = delete the 5 lines above only (two code lines + the 3-line comment). It sits after the snow/beach lerps and before the GBUFFER_OUTPUT block, so it feeds both the PBR path and the SSR gbuffer (channel 3 / RoughnessChannel=3). Effective roughness: ~0.03 (snow) to ~0.08 (grass/beach) — well under the SSR RoughnessThreshold 0.2. Tuned down from the round-5 x0.15 to x0.05 + min(,0.08); re-verified 2026-09-08 with build + frame-300 screenshot (/tmp/glossy_check.png, clean render, exit 0). Caveat for SSR: the gbuffer normal still carries the micro-band perturbation (applied before the GBUFFER_OUTPUT early-out), so SSR rays will be noisy rather than mirror-smooth.

## Task 6 screenshot frame

Chosen `ENGINE_SCREENSHOT_FRAME=300` for mid-game world verification. Probed 120 / 300 / 600 with `ENGINE_AUTOTEST=enter` (binary run directly, VK_ICD_FILENAMES + ENGINE_HIDDEN_WINDOW=1, .png): all land in-world with the full terrain visible, player at stationary spawn (flat beach biome, distant tree shadows), camera identical at 120/300/600 (spawn camera doesn't move), 60 fps steady. 300 = ~5 s after world entry, well past asset streaming/TAA-SSR history warmup, and matches the RenderDoc frame-300 capture convention from earlier rounds. Probe images kept at /tmp/t4_f{120,300,600}.png.

Recipe (no-SSR control identical):
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json ENGINE_SSR=1 ENGINE_AUTOTEST=enter ENGINE_HIDDEN_WINDOW=1 ENGINE_SCREENSHOT=/tmp/x.png ENGINE_SCREENSHOT_FRAME=300 timeout -s KILL 60 ./build/c-game/c-game`

Build after the edit: clean; terrain renders (HLSL recompiled for both the PBR compile and the GBUFFER_OUTPUT gbuffer compile in the same run), exit 0.

## round 6

- Task 8 done: SSR is visually verifiable. Pipeline is correct end-to-end; the "no visible difference" was a signal-magnitude problem, not a plumbing bug.
- Where the visible signal was lost (RenderDoc, fresh frame-300 capture with current build, ENGINE_SSR=1):
  - The OLD /tmp/RenderDoc capture in the dispatch was pre-roughness-cut: stencil mask (R8) all-zero → every SSR buffer zero → composite was a pure scene copy. Current capture: mask live on 88% of pixels (roughness 0.09-0.19 < threshold 0.2), rays march, hits validate.
  - SSR final radiance (bilateral cleanup output, tex 683) is ~0 almost everywhere by design: at the flat-beach spawn the mirror rays of the low terrain point at the SKY, and DiligentFX ValidateHit rejects background (depth-far) hits — radiance only exists where a ray hits geometry (player silhouette + distant dunes). The scene is dusk/dark (sun intensity 0, max scene linear ~0.25), so even those hits carry ~0.13-0.23. At default GAIN 0.5 the composite delta is ~0.01 linear = imperceptible.
  - taaWorldResolve composite-SRV override IS in the path: TAA on (default, GraphicsSettings.taa=true) — RenderDoc shows the TAA TemporalAccumulation draw reading the ssrComposite output (tex 758); TAA off — srcOverride feeds the plain blit/CAS directly (code). No env knob to force TAA off; debug GUI only.
  - Radiance confidence (radiance.a): max 0.22, ~80 px > 0.05 at frame 300 (DepthBufferThickness 0.025 falloff on distant hits keeps it low).
  - Texture ID map (fresh capture): 581 ssr gbuffer (N,rough), 639 stencil/roughness, 646/651 intersection specular/PDF, 656 spatial recon, 672 temporal accum, 683 bilateral radiance (GetSSRRadianceSRV), 758 ssrComposite, 355 sceneColor, 402 TAA accumulated, 800 backbuffer.
  - Minor (not fixed, out of scope): PreparePostFX runs TWICE per frame when ENGINE_SSR is set (explicit taaPostFXExecute in DiligentRenderer.cpp ~line 599 + internal call at the top of ssrDiligentExecute). Redundant GPU work; the internal call could be dropped.
- Winning recipe (in-world frame-300 A/B; the run.sh-fails-without-TERM pitfall applies, run the binary directly):
  ```
  # before (no SSR):
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json ENGINE_AUTOTEST=enter ENGINE_HIDDEN_WINDOW=1 \
    ENGINE_SCREENSHOT=/tmp/ssr_ab/off.png ENGINE_SCREENSHOT_FRAME=300 timeout 60 ./build/c-game/c-game
  # after (SSR on, raised gain):
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json ENGINE_SSR=1 ENGINE_SSR_GAIN=8 ENGINE_AUTOTEST=enter \
    ENGINE_HIDDEN_WINDOW=1 ENGINE_SCREENSHOT=/tmp/ssr_ab/on_8.png ENGINE_SCREENSHOT_FRAME=300 timeout 60 ./build/c-game/c-game
  ```
  GAIN must be ~8 (4 is faint, 12 comparable, default 0.5 invisible). Result at frame 300 (flat-beach spawn): the player's mirror reflection appears as a bright streaky figure on the sand in front of the player, plus a faint horizon-dune band on the right; the before image has none of it. Kept pair: /tmp/ssr_ab/{off.png, on_4.png, on_8.png, on_12.png, side_by_side_off_on8.png, diff_x10.png}. Engine is run-to-run nondeterministic, so diffs include a little horizon/shadow noise (328 px >0.5, scattered) — the reflection region (x 1200-2200, y 700-1500) is where the real signal is (p99 delta ~0.07, max ~0.36 sRGB at gain 8).
- Why the spawn is a bad showcase: only geometry in the mirror direction reflects (sky hits rejected by design); best in-world content there is the player character itself. Any future "make SSR pop" work should either (a) keep GAIN high, (b) reflect the sky (background hits), or (c) pick a view with bright geometry in the mirror direction (sun is off in this scene).
