# notes

## manager — round 6

Curation: task 5 marked done (worker round 5 + last verifier PASS — ssaoRadius/ssaoAlgorithm/ssaoIntensity threaded through GraphicsSettings/normalize/load, typed Settings.cpp templates with matching types, GUI deferred sliders + algorithm cycle with aoDisabled untouched as the on/off key, graphics.html/lua widgets + cycle handler, applyGraphicsSettings forwarding the real fields into ssaoSettingsApply; headless round-trip confirmed no full-file rewrite). No duplicates and no new sub-tasks: worker's findings (the four-layer type consistency that guards against the settings.json rewrite; the full headless GUI-open env-var chain; the deferred-slider vs immediate-cycle split; the html/lua being RMLUI data packed into pak_1.pak) are all local implementation details already absorbed. All of task 6's prerequisites now exist (pak-confirmed shaders, ssao group, composite, settings). Execution order completes 5 → 6. Next action: task 6 (phase-5 headless verification — clean build + pak shader confirm, RenderDoc capture checks, before/after ENGINE_SCREENSHOTs at two render scales with AO on/off, settings round-trip, GPU-time pass).

## manager — round 5

Curation: task 4 marked done (worker round 4 + last verifier PASS — aoCompositeTex + inline AO-multiply PS in TaaDiligent, composite gated on ssaoRan between the TAA block and the downsample, applyGraphicsSettings now forwarding ssaoSettingsApply; A/B screenshots show contact darkening with sky unchanged and no mis-registration at scale 2.0). No duplicates and no new sub-tasks: worker's findings (the ssaoRan gate is the correct AO-valid signal because GetAmbientOcclusionSRV returns an uncleared texture — undefined when Execute didn't run; no pak rebuild needed since the composite PS is an inline constexpr char; ssaoIntensity getter + ssaoIntensityValue rename; applyGraphicsSettings currently passes literals 1.0f/0/1.0f equal to the struct defaults) are all local implementation details already absorbed, and task 5's scope already covers replacing those literals with the real s.ssaoRadius/ssaoAlgorithm/ssaoIntensity fields. Execution order continues 5 → 6. Next action: task 5 (Settings + GUI wiring — GraphicsSettings fields, normalize/load, typed Settings.cpp templates, SettingsGraphicsGui + graphics.html widgets, keeping aoDisabled as the persisted on/off key) and the forward of real fields into ssaoSettingsApply so task 4's placeholder literals get replaced.

## manager — round 4

Curation: task 3 marked done (worker round 3 + last verifier PASS — SsaoDiligent module + taaWorldResolve call site, PrepareResources every frame, Execute after postFXContext->Execute, clean build). No duplicates and no new sub-tasks: worker's findings (ssaoExecute takes depthSRV as a param; SSAO gated on TAA being on; ssaoIntensity stored separately from the attribs; ssaoOn inert until task 4) are all local implementation details already absorbed, and task 4's existing scope already consumes ssaoAOSRV()/ssaoIntensity and wires applyGraphicsSettings→ssaoSettingsApply. Execution order continues 4 → 5 → 6. Next action: task 4 (aoCompositeTex + inline AO-multiply PS after TAA before downsample/CAS/blit, plus applyGraphicsSettings→ssaoSettingsApply which flips ssaoEnabled on and feeds intensity).

## manager — round 3

Curation: task 2 marked done (worker round 2 + last verifier PASS — 3-RTV world pass, normal SV_Target2 outputs, GLTF shadow dummy, pak rebuilt). No duplicates or new sub-tasks — worker's one deviation (gate on `PSO_FLAG_COMPUTE_MOTION_VECTORS` instead of the plan's `PSO_FLAG_FIRST_USER_DEFINED`) is a local implementation detail already absorbed into task 2; no downstream task is affected because tasks 3-6 only consume the TaaDiligent accessors, not the GLTF flag. Execution order 1 → 2 → 3 → 4 → 5 → 6 continues. Next action: task 3 (`SsaoDiligent` module driving DiligentFX `ScreenSpaceAmbientOcclusion` on the shared TAA `PostFXContext`, `Execute` immediately after `postFXContext->Execute` in `taaWorldResolve`).

Curation: task 1 marked done (worker completed, verifier PASS). No duplicates or new sub-tasks needed — worker findings confirm task 2 is unblocked as specified (normalTex target-size confirmed, no pak rebuild needed for task 1, accessors return null when TAA chain is down). Execution order continues 2 → 3 → 4 → 5 → 6. Next action: task 2 (3-RTV world pass + shader normal outputs + GLTF shadow dummy + pak rebuild).

## manager — round 1

Curation: no worker/verifier output yet; all 6 tasks pending and distinct, nothing merged or added. Execution order per plan: 1 → 2 → 3 → 4 → 5 → 6 (task 6 is verification of everything). Next action: task 1 (TaaDiligent normalTex + accessors, no shader edits yet — no pak rebuild needed for this step).

## brainstorm

## Core difficulty

The DiligentFX integration surface is ~90% already built (TAA runs on the same `PostFXContext`, `libDiligentFX.a` is already in c-game's `--start-group`, the D32 depth already carries `BIND_SHADER_RESOURCE` and is sampled as an SRV by TAA every frame). So the real difficulty is NOT plumbing — it's three precise, easy-to-get-wrong details: (1) creating a world-space normal buffer as a 3rd RTV on three *heterogeneous* world pixel shaders (heightmap terrain, props, GLTF-via-`GetPSMainSource`) plus the GLTF shadow path's cascade-sized dummy-RTV bookkeeping, and (2) placing SSAO's `Execute` at the one point where the shared context's reprojected/previous-depth + closest-motion textures are valid, and (3) a non-obvious assumption that a `(0,0,0,0)`-cleared normal + far-plane depth yields AO≈1 in the sky (not AO=0 / garbage).

## Reductions / key lemmas

1. **AO output is full-res in BOTH modes — the 1:1 composite holds.** `GetAmbientOcclusionSRV()` (cpp:465) actually returns `m_Resources[OCCLUSION_HISTORY_RESOLVED]`, NOT the `UpsampledOcclusion` the plan cites (cpp:283-290/463-466). The citation is imprecise, but the *conclusion* is right: the occlusion-history and resolved textures are ALWAYS sized at `m_BackBufferWidth/Height` (= offscreen-chain size, i.e. `swapchainSize * renderScale`) — see cpp:298-299 and 340-341. `FEATURE_FLAG_HALF_RESOLUTION` only shrinks the *spatial* core-AO passes (prefiltered/convolution depth, cpp:100,214,250); the temporal history and the resolved AO stay full-res. So the composite is 1:1 at the existing render-scale size with no resize logic, and the box downsample averages the AO'd color as today. Implementer note: bind `GetAmbientOcclusionSRV()` as the composite source; don't hunt for "UpsampledOcclusion".

2. **The AO format is `R8_UNORM`** (hpp:256); history/length are R16_FLOAT (hpp:257). The composite needs only `ao.r` where AO=1 → `out.rgb` unchanged, AO=0 → fully darkened. `out = in.rgb * (1.0 - (1.0 - ao.r) * strength)` is correct.

3. **"PrepareResources every frame, Execute only when on" is *required*, not stylistic — it is what makes re-enable reset history.** SSAO's `m_CurrentFrameIdx` is set from the context's `FrameDesc.Index` at the *top* of `PrepareResources` (cpp:70), **before** the size-stable early-return (cpp:86-87). So calling `PrepareResources` every frame advances `m_CurrentFrameIdx` even when the size hasn't changed. `Execute` (only called when `g.ssao`) sets `m_LastFrameIdx = m_CurrentFrameIdx` (cpp:819). The reset condition is `m_CurrentFrameIdx != m_LastFrameIdx + 1` (cpp:802) — while SSAO is off, `m_CurrentFrameIdx` keeps advancing but `m_LastFrameIdx` freezes, so on re-enable the gap fires a clean history reset. If you skip `PrepareResources` while off, the index won't advance and the continuity logic desyncs. This is the exact Hydrogent idiom and must be kept.

4. **SSAO shares the same camera-motion fade as TAA.** `Execute` reads `pPostFXContext->GetTransitionAlpha(...)` (cpp:799) on the *shared* context — the same one TAA drives. So during camera motion both TAA and SSAO lower their temporal weight, and right after (re)enable the AO ramps in over `m_FrameTimer.GetElapsedTimef()`. Screenshots taken immediately after toggling on will show *partial* AO by design (plan risk 2c, now confirmed at cpp:799,809-816). Wait a few frames / use a settled vantage before comparing.

5. **`ssao` defaults to `true`** (Renderer.h:19) and the persisted key is the *inverted* `aoDisabled` (Renderer.cpp:234, Settings.cpp:53). A fresh launch already runs AO (after the transition ramp). Every "SSAO off" baseline screenshot must explicitly set `aoDisabled=true`; the phase-1 "identical to pre-phase baseline" check is only meaningful with SSAO forced off.

6. **The world-space normal is already in scope in all three world PS — no new per-pass math.** Terrain: `geomNormal = normalize(vs.Normal)` (heightmap_terrain_ps.hlsl:267, the border-aware world-space stencil normal the VS already outputs). Props: `N` (props_ps.hlsl:131, rotated by the per-instance yaw, no scaling). GLTF: `VSOut.Normal` (unit world-space normal from `GLTF_TransformVertex`). All we do is add a `float4 Normal : SV_Target2;` to each output struct and write `float4(normal, 1.0)`. `PSTerrainOut`/`PSPropsOut` today carry `SV_Target0 Color` + `SV_Target1 Motion` (confirmed).

7. **`taaDepthDSV()` returns the *current* frame's depth** (`depthTex[frameIdx & 1]`, TaaDiligent.cpp:1007), and `taaWorldResolve`'s SRV at line 1125 is `depthTex[curr]` with `curr = frameIdx & 1`. So the new `taaDepthSRV(idx)` accessor just extracts that expression, and SSAO must pass `taaDepthSRV(curr)` — the same buffer/parity TAA uses. The world pass renders to `depthTex[curr]` before `taaWorldResolve` runs, so it holds this frame's depth by the time SSAO samples it.

## Candidate approaches

**A. Follow the plan's 5 phases in order — thin `SsaoDiligent` on the shared `PostFXContext` (recommended).**
- Sketch: world-pass 3rd RTV normal buffer → `SsaoDiligent` module whose `PrepareResources` runs every frame and whose `Execute`+composite run inside `taaWorldResolve` after `postFXContext->Execute` → R8 AO multiply after TAA, before downsample/CAS/blit → settings wiring.
- Main risk: the sky no-op assumption (lemma on `0,0,0,0` normal + far depth) — if the SSAO kernel does not guard far-plane pixels, the sky could darken. Mitigated by the phase-2 RenderDoc AO-map dump (verify sky region ≈1).
- Effort: medium, but most of it is mechanical; the risky surface is small (one assumption, one call-site ordering).

**B. Standalone SSAO pass between `props` and `taa_resolve` (rejected).**
- Sketch: a separate compute dispatch after the world pass, before the TAA block.
- Main risk: it reads the context's reprojected/previous-depth + closest-motion textures that `PostFXContext::Execute` produces — a standalone pass runs *before* that call and silently reads frame N-1 (uninitialized on frame 0). AO disocclusion would be off-by-one. This is the plan's decision-4 and it's correct.
- Effort: looks lower but is wrong; not worth it.

**C. Give SSAO its own second `PostFXContext`.**
- Sketch: a dedicated context + reproject/motion machinery.
- Main risk: duplicates the reprojected/previous-depth + closest-motion generation (bandwidth + GPU time), and SSAO's temporal stage is hard-wired to read *a* context's reprojected-depth/prev-depth/closest-motion (SSAO cpp:1057-1059) — a separate context means feeding it a second set of the same textures for no benefit.
- Effort: high, strictly worse.

**D. Generate the normal buffer in a separate G-buffer pass.**
- Sketch: a dedicated normal-only render pass after the world pass.
- Main risk: extra full-screen pass + extra readback of depth/normal that the world PS already has in scope; pure bandwidth waste versus piggybacking on the existing 2-RTV world pass.
- Effort: higher than A for zero correctness gain.

## Recommended approach

**A**, in the plan's phase order. It is the only approach that reuses the already-present `PostFXContext`/D32-SRV/link surface without duplicating it, and it confines all the real risk to two checkable facts (the sky no-op, and the `postFXContext->Execute`→SSAO→TAA ordering) rather than spreading risk across new machinery. It must be true for A to work that: (1) `GetAmbientOcclusionSRV()` is full-res and R8 (verified: yes, `OCCLUSION_HISTORY_RESOLVED`, full-res R8 in both modes), (2) SSAO's `Execute` runs *after* `postFXContext->Execute` in `taaWorldResolve` so it reads the just-produced context reproject/prev-depth + closest-motion, and (3) the `(0,0,0,0)`-cleared normal + far-plane depth composites to AO≈1 in the sky (the one unverified assumption — confirm by dumping the phase-2 AO map).

## Proposed tasks

1. **Normal buffer + accessors (Phase 1, TaaDiligent only — no shaders yet).** In `TaaDiligent.cpp` add `normalTex` (RGBA16F, `BIND_RENDER_TARGET | BIND_SHADER_RESOURCE`, target size, name `taaWorldNormal`) to `createTargets` (682-741) and release it in `destroyTargets` (668-680); add the four accessors next to `taaColorRTV/taaMotionRTV/taaDepthDSV` (991-1009): `taaNormalRTV()`, `taaNormalSRV()`, `taaPostFXContext()`, `taaDepthSRV(int idx)` (extract the `depthTex[idx]->GetDefaultView(SRV)` from line 1125). Build; confirm the accessors return the right views (null when the TAA chain is down). Verifiable: clean `./scripts/build.sh`, no behavior change (SSAO not wired yet).

2. **3-RTV world pass + normal outputs + GLTF/shadow bookkeeping + pak (Phase 1 rest).** Bump `NumRenderTargets` 2→3 and add `RTVFormats[2]=RGBA16_FLOAT` in HeightmapTerrainDiligent.cpp, PropsRenderDiligent.cpp, and GltfDiligent.cpp (shadow PSOs stay `NumRenderTargets=0` / no `SV_Target2`); add the `SV_Target2` normal write to the three PS (terrain `float4(geomNormal,1.0)`, props `float4(N,1.0)`, GLTF via the `GetPSMainSource` hook gated on `PSO_FLAG_FIRST_USER_DEFINED` set only by `worldDraw`, with the `USE_VERTEX_NORMALS` fallback); extend the GLTF shadow dummy pair (1129-1153) with a 3rd cascade-sized RGBA16F dummy and bind `rtvs[3]`. **Must** run `./scripts/build.sh` (runtime HLSL loads from `pak_1.pak`). Verifiable: `unzip -p build/c-game/data/pak_1.pak materials/props_ps.hlsl | grep SV_Target2`; RenderDoc capture shows the `player`/`terrain`/`props` groups with 3 RTVs and RT2 = unit normals; the `shadow` group still renders with the dummy bound (no VUID-06079).

3. **`SsaoDiligent` module + call site (Phase 2).** New `SsaoDiligent.{h,cpp}` (PCH-skip list, CMakeLists:141-145) driving `Diligent::ScreenSpaceAmbientOcclusion` on the TAA context: `ssaoFrameBegin` calls `PrepareResources(device, ctx, taaPostFXContext(), FEATURE_FLAG_HALF_RESOLUTION)` every frame; `ssaoExecute` builds `RenderAttributes{pDepthBufferSRV=taaDepthSRV(curr), pNormalBufferSRV=taaNormalSRV(), pPostFXContext=taaPostFXContext(), ...}` and runs inside `taaWorldResolve` immediately after `postFXContext->Execute` (line 1128) under `ScopedDebugGroup{ctx,"ssao"}`, only when `ssaoReady() && g.ssao`. Verifiable: `scripts/rdc.py list` shows an `ssao` group with the library's child compute groups; `scripts/rdc.py dump ssao` shows a non-trivial R8 map (darkening in crevices) **and ≈1 in the sky** (the key assumption — if the sky is not ≈1, the normal clear or a far-plane guard is the culprit).

4. **AO composite + settings wiring + verification (Phases 3-5).** Add `aoCompositeTex` + the inline `out.rgb*(1-(1-ao.r)*strength)` point-sample PS (cbuffer on the PRS/PSO per the 2026-09-06 static-cbuffer lesson), running between the TAA block and the downsample/CAS/blit, skipped under `ENGINE_TAA_DEBUG_MV`; forward `ssaoRadius/ssaoAlgorithm/ssaoIntensity` through `applyGraphicsSettings`, `GraphicsSettings`, normalize, typed Settings.cpp templates (next to `aoDisabled`, correct types or the whole settings.json is rewritten), the settings GUI, and `graphics.html`; keep `aoDisabled` as the persisted on/off key. Verifiable: before/after `ENGINE_SCREENSHOT`s (SSAO on vs `aoDisabled=true`) at render scales 0.5/1.0/1.5/2.0 — concave contact areas darken, sky/flat ground unchanged, AO tracks the scene (no bleed/mis-registration); settings.json round-trips without a full-file rewrite; `ENGINE_DEBUG_GPUTIME=1` delta vs the ≤1.5 ms post-world envelope.

## round 1 — task 1 (TaaDiligent normal buffer + accessors)

Done. TaaDiligent.cpp: added file-static `normalTex` (line 60, next to motionTex); created in `createTargets` (lines 709-712) as `taaWorldNormal`, RGBA16F, `BIND_RENDER_TARGET|BIND_SHADER_RESOURCE`, at TARGET size (created right after motionTex, before the downsample block, so `desc` still holds target WxH); released in `destroyTargets` (line 672); added to the null-check (line 734). TaaDiligent.h: declared the 4 accessors after `taaDepthDSV` (lines 89-93). TaaDiligent.cpp: implemented `taaNormalRTV()`/`taaNormalSRV()` after `taaMotionRTV` (1011-1017) and `taaDepthSRV(int)`/`taaPostFXContext()` after `taaDepthDSV` (1026-1032). `taaDepthSRV(idx)` returns `depthTex[idx]->GetDefaultView(SRV)` — byte-identical view to the `pCurrDepthBufferSRV` at line 1125. `taaPostFXContext()` returns `postFXContext.get()`. Clean `./scripts/build.sh` (exit 0, only pre-existing Diligent thirdparty header warnings); all 4 symbols confirmed in `libc-engine.a` via nm.

## Findings (task 1)

- `normalTex` reuses the shared `desc` struct; at the insert point `desc.Width/Height` are still the TARGET size (the backbuffer-sized copy only happens later for downTex), so it is correctly target-sized. `desc.ClearValue.Format` is left as RGBA16F (from sceneColor) which matches normalTex's format — no mismatch. The initial clear value does not matter for the normal buffer: task 2 clears the RTV to (0,0,0,0) in the world pass.
- No shader edits in this task → no `pak_1.pak` rebuild required (that's task 2). Behavior is unchanged for now: the normal RTV is created but never bound to the world pass yet.
- Accessors return null when the TAA chain is down (`normalTex`/`depthTex[idx]` null-checked), consistent with the sibling `taaColorRTV`/`taaMotionRTV`/`taaDepthDSV` idiom.

## round 2 — task 2 (3-RTV world pass + shader normal outputs + GLTF shadow dummy + pak rebuild)

Done. All C++ and shader edits in place; clean build with pak regenerated.

Changes:
- DiligentRenderer.cpp: world pass now binds 3 RTVs `{worldRtv, taaMotionRTV(), taaNormalRTV()}`, `SetRenderTargets(3,...)`, 3rd clear to `(0,0,0,0)`. Legacy 1-RTV fallback unchanged.
- HeightmapTerrainDiligent.cpp: lit PSO `NumRenderTargets=3`, `RTVFormats[2]=RGBA16_FLOAT` (shadow PSO at line 724 with `NumRenderTargets=0` untouched).
- PropsRenderDiligent.cpp: lit PSO `NumRenderTargets=3`, `RTVFormats[2]=RGBA16_FLOAT` (shadow PSO at line 587 with `NumRenderTargets=0` untouched).
- GltfDiligent.cpp: `rendererCI.NumRenderTargets=3`, `RTVFormats[2]=RGBA16_FLOAT`.
- GltfDiligent.cpp GetPSMainSource hook: adds `float4 WorldNormal : SV_Target2` gated on `PSO_FLAG_COMPUTE_MOTION_VECTORS` (present in worldDraw, absent in shadowDraw). Footer writes `PSOut.WorldNormal = float4(VSOut.Normal,1.0)` under `#if COMPUTE_MOTION_VECTORS` / `#if USE_VERTEX_NORMALS` with `float4(0,0,1,0)` flat fallback. UNSHADED and CustomData branches untouched.
- GltfDiligent.cpp gltfDiligentShadowDraw: added 3rd cascade-sized RGBA16F dummy (`pbrShadowDummyNormal`), binds `rtvs[3]` with `SetRenderTargets(3,...)`.
- heightmap_terrain_ps.hlsl: `PSTerrainOut` gains `float4 Normal : SV_Target2`; `result.Normal = float4(geomNormal, 1.0)` before return.
- props_ps.hlsl: `PSPropsOut` gains `float4 Normal : SV_Target2`; `result.Normal = float4(N, 1.0)` before return.

## Findings (task 2)

- The plan's "gated on `PSO_FLAG_FIRST_USER_DEFINED`" approach does NOT work with the PBR renderer's internal flag processing: `GLTF_PBR_Renderer::Render` does `PSOFlags &= RenderParams.Flags` where the initial `PSOFlags` is built from vertex/material/optional flags — user-defined flags are never in the initial set, so they get zeroed by the mask regardless of being in `RenderParams.Flags`. Used `PSO_FLAG_COMPUTE_MOTION_VECTORS` instead (set in worldDraw line 1081, absent in shadowDraw which only uses `PSO_FLAG_DEFAULT`). Its shader define `COMPUTE_MOTION_VECTORS` is already generated by `ADD_PSO_FLAG_MACRO(COMPUTE_MOTION_VECTORS)` in PBR_Renderer.cpp:1574.
- The GLTF shadow draw PSOs still declare 3 RTV formats (from `CreateInfo.NumRenderTargets=3`) but their shader only writes SV_Target0 (Color). The 2nd/3rd RTVs are the cascade-sized dummies that retain their initial contents — no write, no VUID violation.
- `unzip -p build/c-game/data/pak_1.pak materials/props_ps.hlsl | grep SV_Target2` and same for terrain confirm the edits are in the pak. `./scripts/build.sh` exit 0, pak size 69M.
- `geomNormal` (terrain) and `N` (props) are function-scoped locals in the PS main, confirmed in scope at the return site.

## Remaining steps

- Task 3 is ready: `SsaoDiligent` module driving DiligentFX SSAO on the TAA `PostFXContext`.

## round 3 — task 3 (SsaoDiligent module + call site)

Done. New `SsaoDiligent.{h,cpp}` module driving `Diligent::ScreenSpaceAmbientOcclusion` on the shared TAA `PostFXContext`; call site wired into `taaWorldResolve` after `postFXContext->Execute` under a `"ssao"` debug group. Clean `./scripts/build.sh` (exit 0, only pre-existing Diligent header warnings); all 8 public symbols confirmed in `libc-engine.a` via nm; `c-game` relinked.

### Files
- `c-engine/renderer/diligent/SsaoDiligent.h` (new): declares `ssaoInit/ssaoDestroy`, `ssaoSettingsApply`, `ssaoOn`, `ssaoReady`, `ssaoFrameBegin`, `ssaoExecute`, `ssaoAOSRV`.
- `c-engine/renderer/diligent/SsaoDiligent.cpp` (new): file-static `unique_ptr<ScreenSpaceAmbientOcclusion>` (CreateInfo{EnableAsyncCreation=false}), `HLSL::ScreenSpaceAmbientOcclusionAttribs ssaoAttribs{}` (value-init picks up the fxh DEFAULT_VALUE defaults: EffectRadius 1.0, EffectFalloffRange 0.615, RadiusMultiplier 1.457, TemporalStabilityFactor 0.9, SpatialReconstructionRadius 4.0, Algorithm GTAO), `ssaoEnabled` (default false), `ssaoIntensity`. `ssaoFrameBegin` calls `PrepareResources(device, ctx, taaPostFXContext(), FEATURE_FLAG_HALF_RESOLUTION)` every frame (gated only on infra: `!ssao || !ctx || !taaPostFXContext()`). `ssaoExecute(ctx, depthSRV)` builds `RenderAttributes{pDevice, pDeviceContext, pPostFXContext, pDepthBufferSRV=depthSRV, pNormalBufferSRV=taaNormalSRV(), pSSAOAttribs=&ssaoAttribs}`, returns `status == POST_FX_EXECUTION_STATUS_READY` (false on PENDING).
- `c-engine/renderer/diligent/TaaDiligent.cpp`: +`#include SsaoDiligent.h` + `ScopedDebugGroup.hpp`; `ssaoFrameBegin(ctx)` right after `taa->PrepareResources` in `taaFrameBegin` (line 868); in `taaWorldResolve`, after `postFXContext->Execute(pa)` (line 1154) and before the TAA block, `if (ssaoReady() && ssaoOn()) { ScopedDebugGroup{ctx,"ssao"}; ssaoExecute(ctx, taaDepthSRV(curr)); }` (line 1156-1159).
- `c-engine/renderer/diligent/DiligentRenderer.cpp`: +`#include SsaoDiligent.h`; `ssaoInit()` after `taaInit()`; `ssaoDestroy()` after `taaDestroy()`.
- `c-engine/CMakeLists.txt`: added `renderer/diligent/SsaoDiligent.cpp` to SKIP_PRECOMPILE_HEADERS.

### Findings (task 3)

- **`ssaoExecute` takes the depth SRV as a param, not `ctx`-only.** The task spec said `ssaoExecute(ctx)` with `pDepthBufferSRV=taaDepthSRV(frameIdx&1)`, but `frameIdx` is a file-static in TaaDiligent.cpp (not exposed). At the call site in `taaWorldResolve`, `curr` (= `frameIdx & 1`) is already in scope, so the cleanest form is `ssaoExecute(ctx, taaDepthSRV(curr))` — the depth SRV is passed in, and `ssaoExecute` still resolves `taaPostFXContext()` / `taaNormalSRV()` internally. `taaDepthSRV(curr)` is byte-identical to the `pCurrDepthBufferSRV` the TAA block feeds `postFXContext->Execute` (same `depthTex[curr]` parity), so SSAO samples the same depth buffer TAA just used.
- **SSAO runs only when TAA is on.** The call site is inside the `if (taaOn && postFXContext && taa && cameraCB)` block in `taaWorldResolve`, so SSAO is gated on TAA being enabled (in addition to `ssaoReady() && ssaoOn()`). This is required, not a bug: SSAO reads the reprojected/prev-depth + closest-motion textures that `postFXContext->Execute` produces, and that call only runs inside the TAA block. (Approach B in the brainstorm — a standalone SSAO pass — was rejected precisely for this off-by-one/stale-read reason.) If the user later wants SSAO-without-TAA, that is a separate architecture change, out of scope here.
- **`ssaoFrameBegin` (PrepareResources) runs every frame regardless of `ssaoEnabled`.** It is called unconditionally in `taaFrameBegin` (after `taa->PrepareResources`), gated only on infrastructure (`ssao` object exists + `taaPostFXContext()` non-null), NOT on `ssaoEnabled`. This is the Hydrogent idiom from the notes: advancing the context's frame index every frame (even while SSAO is off) is what makes the `m_CurrentFrameIdx != m_LastFrameIdx + 1` reset fire on re-enable. Skipping PrepareResources while off would desync the continuity logic.
- **`ssaoAttribs` value-init = fxh defaults.** In C++ mode the fxh `DEFAULT_VALUE(x)` macro expands to `=x`, so `ScreenSpaceAmbientOcclusionAttribs{}` gets every default member initializer from the fxh (EffectRadius 1.0, etc.). `ssaoSettingsApply` only overwrites `EffectRadius` + `Algorithm` (and sets `ssaoEnabled` / `ssaoIntensity`); the other fields keep their fxh defaults. `intensity` is NOT part of the attribs struct — it is stored in the separate `ssaoIntensity` static for the task-4 composite multiply.
- **`ssaoOn` defaults to false.** `ssaoEnabled` starts false and is only set true by `ssaoSettingsApply` (task 4 wires `applyGraphicsSettings` → `ssaoSettingsApply(g.ssao, ...)`). So until task 4 lands, the `ssaoReady() && ssaoOn()` gate is false and SSAO never executes — the infra is in place but inert. (The plan's `g.ssao` defaults to true, so after task 4 the first `applyGraphicsSettings` will turn it on.)
- **Includes resolve via the existing `${diligent_git}/DiligentFX` include dir** — `PostProcess/ScreenSpaceAmbientOcclusion/interface/ScreenSpaceAmbientOcclusion.hpp` and `Shaders/PostProcess/ScreenSpaceAmbientOcclusion/public/ScreenSpaceAmbientOcclusionStructures.fxh` both resolve without a new include path. The fxh is included inside `namespace Diligent::HLSL` (same pattern as TaaDiligent.cpp:40-46), after `ShaderDefinitions.fxh`.

### Remaining steps

- Task 3 complete. Next: task 4 (AO composite pass + `applyGraphicsSettings` → `ssaoSettingsApply` wiring, which flips `ssaoEnabled` on and feeds the intensity). The `ssaoAOSRV()` + `ssaoIntensity` are already exposed for task 4's composite multiply.

## round 4 — task 4 (AO composite pass + applyGraphicsSettings → ssaoSettingsApply)

Done. aoCompositeTex + inline point-sample AO multiply PS in TaaDiligent.cpp, wired between the TAA block and the renderScale downsample in taaWorldResolve; applyGraphicsSettings now forwards the ssao flag. Clean ./scripts/build.sh; A/B screenshots verified at renderScale 1.0 and 2.0, plus a TAA-off control.

### Files
- `c-engine/renderer/diligent/TaaDiligent.cpp`: `aoCompositeTex` (RGBA16F, TARGET size) next to normalTex in createTargets, released in destroyTargets (+ `aoSrbs.clear()`); new `kAoCompositePS` (1:1 point sample, `Texture2D<float> g_AO` — the library's R8 idiom — shared POINT sampler, `out.rgb *= 1.0-(1.0-ao)*Strength`), `createAoPSO` (RTV RGBA16_FLOAT, cbuffer `cbAoCompositeAttribs` set STATIC on the PSO with `SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE` + per-frame MapHelper — the CAS pattern), `aoSrbFor` (SRB cache keyed by color-source texture, g_AO pinned at creation), `aoCompositeApply` helper (MapHelper strength → ssaoIntensity(), draw to aoCompositeTex, return its SRV); in taaWorldResolve: `ssaoRan` captured from the TAA-block `ssaoExecute`, composite runs `if (!debugMv && ssaoOn() && ssaoRan)` between the debug-MV swap and the downsample, then srcColorSRV = aoCompositeTex SRV feeds downsample/CAS/blit. aoCB created in taaInit, all released in taaDestroy.
- `c-engine/renderer/diligent/SsaoDiligent.{h,cpp}`: added `float ssaoIntensity(void)` getter (the composite's only source of strength); the file static was renamed `ssaoIntensityValue` (a function cannot share the static's name in the same TU).
- `c-engine/renderer/diligent/DiligentRenderer.cpp`: `applyGraphicsSettings` now calls `ssaoSettingsApply(s.ssao, 1.0f, 0, 1.0f)` — fxh defaults (radius 1.0, GTAO, intensity 1.0), since the `ssaoRadius/ssaoAlgorithm/ssaoIntensity` GraphicsSettings fields don't exist yet.

### Findings (task 4)

- **The task's literal composite gate (`ssaoOn && ssaoAOSRV()`) is unsafe and was extended with `ssaoRan`.** `GetAmbientOcclusionSRV()` returns `m_Resources[OCCLUSION_HISTORY_RESOLVED]`, which is created in SSAO `PrepareResources` WITHOUT a clear (unlike the history textures, cleared to AO=1). `ComputePlaceholderTexture` (PENDING path) clears it to (1,1,1,1) = no occlusion, so PENDING is safe — but when SSAO's `Execute` never ran this frame (TAA off: the ssaoExecute call site is inside the TAA block; or infra guards fail), the texture holds UNDEFINED GPU memory and the composite would sample garbage. `ssaoRan` (the `ssaoExecute` return, captured in the TAA block) is the correct "AO valid this frame" signal. With `EnableAsyncCreation=false`, Execute is READY from the first call, so `ssaoRan == Execute ran`. Verified by A/B: TAA-off+SSAO-on vs TAA-off+SSAO-off = identical within run noise (a same-settings control pair measured 46.67% >6/255 pixels of pure inter-run grass-animation noise; the AO-on/off TAA-off pair measured the same 46.67% — AO contributes nothing when TAA is off).
- **AO direction + placement verified by screenshots** (frame-100 ENGINE_SCREENSHOT, same vantage): TAA+SSAO on vs `aoDisabled=true` at renderScale 1.0 — 61.6% pixels >6/255 different, 97.4% of them DARKER with AO on; contact darkening at grass bases, the character's suit crevices, under tree canopies; sky identical (the AO≈1-in-sky assumption holds at frame 100). At renderScale 2.0 (composite → box downsample → blit): 68.3% different, 97.8% darker, AO tracks geometry with no mis-registration. GPU overlay: 1.70 ms frame with TAA+SSAO+composite vs 1.28 ms with TAA only at scale 1.0 (~0.4 ms SSAO+composite); 5.60 ms at scale 2.0 (dominated by 4× world pixels).
- **`applyGraphicsSettings` forwards `ssaoSettingsApply(s.ssao, 1.0f, 0, 1.0f)` for now** — the radius/algorithm/intensity fields are task 5's scope (Renderer.h); when they land, that line becomes `ssaoSettingsApply(s.ssao, s.ssaoRadius, s.ssaoAlgorithm, s.ssaoIntensity)`. The literals equal the planned struct defaults, so behavior is identical either way. `ssaoSettingsApply` overwrites `ssaoAttribs.EffectRadius`/`Algorithm` on every call — harmless (idempotent defaults today).
- **No pak rebuild needed**: the composite PS is an inline `constexpr char` (like kBlitPS/kDownsamplePS/kCasPS), runtime-compiled from memory — no `.hlsl` file edits in this task.
- SRB cache correctness across resize: `destroyTargets` (fired on every resize in taaFrameBegin BEFORE ssaoFrameBegin rebuilds SSAO's textures) clears `aoSrbs`, so the cached g_AO view is never stale; same reasoning as the existing blit/cas/down caches.
- settings.json toggled back to its original state (`aoDisabled: false`, `taaEnabled: true`, `renderScale: 1.0`, valid JSON) after testing.

## round 5 — task 5 (Settings + GUI wiring)

Done. ssaoRadius/ssaoAlgorithm/ssaoIntensity threaded through the whole settings stack; the three GUI widgets (radius slider, GTAO/HBAO/VBAO cycle, intensity slider) are on the AO row. Clean `./scripts/build.sh`; pak re-zipped; round-trip verified headlessly. settings.json restored to its original state after testing.

### Files
- `c-engine/renderer/Renderer.h`: `GraphicsSettings` gains `float ssaoRadius=1.0f`, `int ssaoAlgorithm=0`, `float ssaoIntensity=1.0f` (kept `ssao`).
- `c-engine/renderer/Renderer.cpp`: `graphicsNormalize` clamps radius 0.1..10 / algorithm 0..2 / intensity 0..2; `rendererGraphicsLoad` reads the three keys (GetDouble/GetInt/GetDouble).
- `c-utils/settings/Settings.cpp`: templates `ssaoRadius`(double 1.0), `ssaoAlgorithm`(int 0.0), `ssaoIntensity`(double 1.0) next to `aoDisabled` — types match the GUI persist calls + the `validateSetting` jsonIsDouble/jsonIsInt checks exactly.
- `c-engine/renderer/diligent/DiligentRenderer.cpp`: `applyGraphicsSettings` now `ssaoSettingsApply(s.ssao, s.ssaoRadius, s.ssaoAlgorithm, s.ssaoIntensity)` — task 4's placeholder literals (1.0f/0/1.0f) replaced with the real fields.
- `c-game/game/settingsGui/graphics/SettingsGraphicsGui.cpp`: page state `ssaoRadius/ssaoAlgorithm/ssaoIntensity` + `ssaoAlgorithmNames[]` (GTAO/HBAO/VBAO) + label; `applyRenderer` forwards the three fields; `dirtySsao` flag in the deferred-slider machinery (`applySliderChanges` + `update()` settle + the wire-autotest `!(...)` close check); radius/intensity are deferred sliders (like casStrength) → `persistDouble`, the algorithm is an immediate cycle (like shadowMode) → `persistInt`; `aoDisabled` toggle (line ~447) untouched, stays the on/off key; luaRegisterFunction + rmlBindFloat/rmlBind + syncLabels + the added() seed/clamp.
- `c-game/data/pak_1/gui/settings/graphics/graphics.html`: three new `option` rows after the AO toggle — `AO — Radius` slider (min 0.1 max 10 step 0.1, `data-value="ssaoRadius"`, `format(ssaoRadius,2)`), `AO — Method` cycle button (`id=toggleSsaoAlgorithm`, `{{ssaoAlgorithmLabel}}`), `AO — Intensity` slider (min 0 max 2 step 0.05, `data-value="ssaoIntensity"`, `format(ssaoIntensity,2)`).
- `c-game/data/pak_1/gui/settings/graphics/graphics.lua`: `cycleHandlers` entry `toggleSsaoAlgorithm = { prev="toggleSsaoAlgorithmPrev", next="toggleSsaoAlgorithm" }` (the cycle button is the direction-aware mousedown/click pattern, same as the shadows cycle).

### Findings (task 5)

- **Types verified consistent across all four layers** (the docs/lessons.md 2026-09-04 "mismatch rewrites the whole settings.json" failure mode is what this guards): Settings.cpp templates (double/int/double) == Renderer.cpp reads (settingsGetDouble/GetInt/GetDouble) == GUI persists (persistDouble/persistInt/persistDouble) == `validateSetting` (jsonIsDouble/jsonIsInt/jsonIsDouble). Confirmed on disk: after a `wire` autotest persist, the file gained `"ssaoRadius": 1.0, "ssaoAlgorithm": 0, "ssaoIntensity": 1.0` (JSON valid, no full-file rewrite — all pre-existing user keys retained).
- **The headless GUI open requires the full chain** (a bare run never opens the graphics page, so `ENGINE_GRAPHICS_SETTINGS_AUTOTEST=wire` alone does nothing): `ENGINE_AUTOTEST=settings` (MainMenuGui.openSettings) + `ENGINE_SETTINGS_AUTOTEST=graphics` (SettingsGui.showGraphicsSettings) + `ENGINE_GRAPHICS_SETTINGS_AUTOTEST=wire` (the flip+persist), plus `ENGINE_LOG_TIMEOUT` as the exit safety net. Used that to drive the round-trip.
- **Radius/intensity use the deferred-slider path, algorithm uses the immediate-cycle path.** The split mirrors the existing code exactly: the two range inputs mark `dirtySsao` and settle/persist 50 ms after the last change (applyRenderer in `applySliderChanges` forwards the current algorithm too, so a slider move re-applies the whole block); the method button applies+persists `ssaoAlgorithm` immediately like `toggleShadowQuality`. `aoDisabled` is NOT touched by the ssao controls — toggling radius/algorithm/intensity leaves the AO on/off state unchanged.
- **`format(x, 2)` is a proven label format** (already used for `cursorScale`/`uiScale` in other pages), so `format(ssaoRadius,2)` / `format(ssaoIntensity,2)` render two decimals.
- **No pak rebuild for the C++** — graphics.html/graphics.lua are RMLUI data loaded from `build/c-game/data/pak_1.pak`, so `./scripts/build.sh` re-zips `c-game/data/pak_1` after the source edits (confirmed the packed html/lua carry the new widgets/handler). No `.hlsl` edits → no pak-shader regeneration needed.
- **settings.json restored to the pre-test original** (the wire test's taa-off/renderScale 1.3/etc. values are reverted). The ssao keys are not on disk in that state — they seed in-memory at `settingsInit` (proven: engine ran clean) and get written on the next GUI change, same as before this task.

### Remaining steps

- Task 5 complete. Task 6 (phase-5 headless verification) is next: RenderDoc capture (ssao group, 3-RTV world pass, RT2 normals, no new VUIDs), before/after ENGINE_SCREENSHOTs (AO on vs aoDisabled=true, two render scales), settings round-trip (now proven), and the GPU-time pass.
