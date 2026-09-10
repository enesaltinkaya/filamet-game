# DiligentFX Bloom — implementation plan

Add DiligentFX's `Bloom` (prebuilt `libDiligentFX.a`) to the Diligent render
path: a thin `BloomDiligent` module that runs the bloom effect on the existing
`PostFXContext` and drops its output (the HDR scene color with bloom already
added) into the TAA resolve chain via a **pointer swap** — no new offscreen
texture, no new RTV/PSO, no new shader. Reference:
`/home/enes/Projects/c/cpp-thirdparty/diligent/git/DiligentFX/PostProcess/Bloom`
(README, `interface/Bloom.hpp`, 479-line `src/Bloom.cpp`),
`DiligentSamples/Tutorials/Tutorial27_PostProcessing` (integration shape — the
per-frame `PostFXContext::Execute` → TAA → `ComputeBloom` → `pHDRColorSRV =
GetBloomTextureSRV()`), and `DiligentFX/Radient` (the high-level pipeline that
composes bloom after TAA and before the final tonemap).

The one-line delta versus the SSAO plan (`plans/ssao-diligentfx.md`): SSAO
needed a new world-normal RTV + a multiply-composite pass; bloom's only
external input is the HDR color SRV (already the TAA-accumulated /
`sceneColorTex` frame) and its output **is** the final HDR color, so the
whole "add a 3rd RTV + normal output" Phase 1 and the AO-composite Phase 3
are empty. Bloom is a pointer swap.

## Current state (verified in-tree)

The integration surface is ~95% built — the SSAO plan's groundwork plus the
bloom settings flag already flow end-to-end:

- c-engine already runs TAA through DiligentFX `PostFXContext` +
  `TemporalAntiAliasing` (the Tutorial27 shape): `postFXContext->Execute(pa)`
  + `taa->Execute` inside `taaWorldResolve` (TaaDiligent.cpp:1310-1351). The
  `PostFXContext` is a stable target-size object created once; `taaPostFXContext()`
  (TaaDiligent.h:93) already exposes it to sibling modules — `SsaoDiligent`
  uses exactly this accessor (SsaoDiligent.cpp:68,71,83).
- The offscreen HDR color is `sceneColorTex` RGBA16F at `swapchainSize *
  renderScale`; its default SRV is the resolve source `srcColorSRV`
  (TaaDiligent.cpp:1315). After TAA it becomes the TAA-accumulated SRV
  (1350-1351); after the SSAO composite it becomes the AO-composite SRV
  (1367-1371). All of the final blit/CAS/downsample PS sample a
  `Texture2D<float4> g_Source` (kBlitPS 182-194, kDownsamplePS 200-233,
  kCasPS 366-478) and the present is opaque — the source's alpha is ignored.
- An unconsumed `bloom` settings flag **already flows end-to-end** (the only
  break in the chain is `applyGraphicsSettings`): `GraphicsSettings.bloom`
  (Renderer.h:23, default `true`), loaded `s.bloom =
  !utils::settingsGetBool("bloomDisabled")` (Renderer.cpp:244), persisted key
  `bloomDisabled` (c-utils/settings/Settings.cpp:63), toggled from the graphics
  settings page (`SettingsGraphicsGui.cpp` — `bloomEnabled` 36,
  `g.bloom = bloomEnabled != 0` in `applyRenderer` 134, load 244,
  `toggleBloom` 484-486 persists `bloomDisabled`), and the debug overlay.
  `DiligentRenderer::applyGraphicsSettings` (DiligentRenderer.cpp:806-809)
  forwards TAA + SSAO but **not** bloom. Identical to how the `ssao` flag
  looked before the SSAO plan — the wiring is half-built.
- c-game links `libDiligentFX.a` once inside `--start-group`
  (c-engine/CMakeLists.txt:93-94 `DILIGENT_LIBS` PARENT_SCOPE →
  c-game/CMakeLists.txt:11,64). `nm` on the prebuilt build-linux archive
  confirms every needed symbol is exported:
  `Diligent::Bloom::{ctor, dtor, PrepareResources, Execute,
  GetBloomTextureSRV, UpdateUI}`. The Bloom module's own
  `ComputePrefilteredTexture` / `ComputeDownsampledTexture` /
  `ComputeUpsampledTexture` / `ComputePlaceholderTexture` / `UpdateConstantBuffer`
  / `PrepareShadersAndPSO` are all `T` symbols in the same archive.
- The include path already resolves the Bloom headers:
  `${diligent_git}/DiligentFX` (c-engine/CMakeLists.txt:77) puts
  `PostProcess/Bloom/interface/Bloom.hpp` and
  `Shaders/PostProcess/Bloom/public/BloomStructures.fxh` on the search path —
  same root that `SsaoDiligent.cpp:11` and TaaDiligent.cpp already use.

The Bloom module's contract (verified in `Bloom.hpp`/`Bloom.cpp`):

- `Bloom(IRenderDevice*, const CreateInfo{EnableAsyncCreation = false})` —
  the ctor allocates the constant buffer only (Bloom.cpp:61-70); no
  per-frame resources until `PrepareResources`.
- `PrepareResources(IRenderDevice*, IDeviceContext*, PostFXContext*,
  FEATURE_FLAGS)` — sizes its downsampled/upsampled pyramids + output from the
  context `FrameDesc.Width/Height` (Bloom.cpp:84-85; no
  `FEATURE_FLAG_TEMPORAL_UPSCALING` on our context → target size). Returns
  immediately if size+flags are unchanged (87-88). All internal textures are
  `R11G11B10_FLOAT`, `BIND_SHADER_RESOURCE | BIND_RENDER_TARGET` (103-141).
  The only `FEATURE_FLAGS` value is `FEATURE_FLAG_NONE` (hpp:60-63).
- `Execute(const RenderAttributes&)` — fills nothing from depth/normal; the
  **only** mandatory input is `pColorBufferSRV` (DEV_CHECKED non-null,
  Bloom.cpp:413) plus `pBloomAttribs`. While PSOs are not ready it runs
  `ComputePlaceholderTexture` (a straight copy of the input into the output,
  398-405) and returns `PENDING`; once ready it runs
  prefilter→downsample→upsample (424-426) and returns `READY`.
- `GetBloomTextureSRV()` (452-455) returns the output SRV — and that output
  is **`SourceColor + ColorSum`**, i.e. the full HDR frame with bloom already
  added (upsample shader `Bloom_ComputeUpsampledTexture.fx`: `uInstID==0`
  branch `return SourceColor + ColorSum`; `uInstID!=0` branch
  `lerp(SourceColor, SourceColor + Intensity*ColorSum, AlphaInterpolation)`
  for the transition fade). So consuming it is a pointer swap, not a new
  composite shader.
- `HLSL::BloomAttribs` (`Shaders/PostProcess/Bloom/public/BloomStructures.fxh`):
  `Intensity` (default 0.15), `Threshold` (1.0), `SoftTreshold` (0.125 —
  note the misspelling is in the source), `Radius` (0.75), plus an
  internal `AlphaInterpolation` (1.0) the module overwrites from
  `PostFXContext::GetTransitionAlpha` (UpdateConstantBuffer, Bloom.cpp:272-286)
  and 3 padding floats. The fxh demands `ShaderDefinitions.fxh` first
  (`#error` otherwise) — same include block shape as SsaoDiligent.cpp:13-18.
- **Never call `Bloom::UpdateUI`** (Bloom.cpp:457-478) — it calls `ImGui::`
  against the lib's own imgui; drive bloom from the rmlui settings page /
  debug overlay instead.

The two gaps this plan closes (both tiny):

1. **No bloom module + no execution point.** Nothing constructs a `Bloom`,
   runs it on the `PostFXContext`, or feeds its output into the resolve chain.
2. **The flag is not forwarded.** `applyGraphicsSettings` does not forward
   `s.bloom`, so the existing on/off plumbing is a no-op on this path.

## Approach / decisions

1. **Thin `BloomDiligent` module on the existing `PostFXContext`** — no second
   context, no new frame machinery. `Bloom::PrepareResources` reads the
   context's `FrameDesc`/feature flags (Bloom.cpp:79-85) exactly like
   `ssaoFrameBegin` reads the same context (SsaoDiligent.cpp:67-73). Bloom and
   TAA/SSAO consume the same shared `PostFXContext` — the Tutorial27/Radient
   pattern (Tutorial27 244,606-609; RadientTesseraPostProcessPipeline 431,
   234-238).

2. **Pointer swap, not a composite pass.** `bloom->Execute` writes
   `SourceColor + bloom` into its own output; `GetBloomTextureSRV()` returns
   that. Integration is `srcColorSRV = bloomSRV()` — the same SRV the existing
   `kDownsamplePS`/`kCasPS`/`kBlitPS` already sample as a `Texture2D<float4>`.
   No new `aoCompositeApply`-style multiply shader (contrast SSAO, whose R8
   AO mask had no color and needed a dedicated composite, TaaDiligent.cpp:749).

3. **Application point: last HDR pass before the sRGB encode-on-store, AFTER
   TAA and the SSAO composite, BEFORE the renderScale downsample / CAS /
   blit** — inside `taaWorldResolve`, after the AO composite block
   (TaaDiligent.cpp:1367-1371) and before the renderScale downsample
   (1375-1381). This matches the reference order: light → SSAO → TAA →
   bloom → tonemap/encode (Tutorial27_PostProcessing.cpp:314-317,
   774→895→316; RadientTesseraPostProcessPipeline.cpp:431→467→503→540→
   344). Bloom reads the resolved target-size HDR frame (already AO'd) and
   its output (also target-size 1:1) flows through the same downsample/CAS/
   blit — zero resize logic.

4. **Target-size invariance = free renderScale support.** `PrepareResources`
   sizes from `FrameDesc.Width/Height` (target size = `swapchainSize *
   renderScale`), so the bloom output is 1:1 with the TAA frame at any
   `renderScale`; the existing downsample/CAS/blit handle the backbuffer
   exactly as today (Bloom.cpp:84-85). No resize logic to add.

5. **No ready-gate needed for the swap (unlike SSAO).** While PSOs are being
   created, `Execute` runs `ComputePlaceholderTexture` — a valid copy of the
   input color into the output (Bloom.cpp:398-405) — so `GetBloomTextureSRV()`
   is always a valid `srcColorSRV` replacement. The module still returns
   `PENDING` during that window; we simply gate the swap on the `Execute`
   return like SSAO gates on `ssaoRan` (TaaDiligent.cpp:1333,1367).

6. **Toggle idiom mirrors SSAO.** `bloomFrameBegin(ctx)` calls
   `bloom->PrepareResources` every frame (size-gated no-op when stable,
   Bloom.cpp:87-88) so the pyramids stay sized across window/resolution/
   renderScale changes; `Execute` + the swap run only when `bloomOn()`.

7. **No new include directories, no new link target.** `#include
   "PostProcess/Bloom/interface/Bloom.hpp"` resolves via the
   `${diligent_git}/DiligentFX` root already on the include path
   (c-engine/CMakeLists.txt:77) — same as SsaoDiligent.cpp:11. The attribs
   struct is included inside `namespace Diligent::HLSL` with
   `Shaders/Common/public/ShaderDefinitions.fxh` first
   (`BloomStructures.fxh:4-9`). `libDiligentFX.a` is already in the
   `--start-group` and exports every `Bloom` symbol (nm-verified) — the
   linker line is unchanged.

## Phase 1 — `BloomDiligent` module

Files:

- **NEW** `c-engine/renderer/diligent/BloomDiligent.h` — mirror
  `SsaoDiligent.h` (the proven template):
  - `bloomInit(void)` / `bloomDestroy(void)`
  - `bloomSettingsApply(bool enabled)` — store `bloomOn`; later (Phase 4)
    takes the four intensity/threshold/radius params.
  - `bloomOn(void)`, `bloomReady(void)` (module constructed)
  - `bloomFrameBegin(Diligent::IDeviceContext* ctx)`
  - `bool bloomExecute(Diligent::IDeviceContext* ctx, Diligent::ITextureView* colorSRV)`
  - `Diligent::ITextureView* bloomSRV(void)` — → `GetBloomTextureSRV()`
  - forward-declare `Diligent::IDeviceContext` / `Diligent::ITextureView`
    only (like SsaoDiligent.h:5-8).
- **NEW** `c-engine/renderer/diligent/BloomDiligent.cpp`
  - Includes: `renderer/diligent/BloomDiligent.h`, `renderer/diligent/
    DiligentRenderer.h` (for the `device` extern, DiligentRenderer.h:19),
    `renderer/diligent/TaaDiligent.h` (for `taaPostFXContext()`), the Diligent
    interface headers, then `PostProcess/Bloom/interface/Bloom.hpp`; then the
    SsaoDiligent.cpp:13-18 include block with `BloomStructures.fxh` in place
    of the SSAO structures fxh:
    ```cpp
    namespace Diligent {
    namespace HLSL {
    #include "Shaders/Common/public/ShaderDefinitions.fxh"
    #include "Shaders/PostProcess/Bloom/public/BloomStructures.fxh"
    }
    }
    ```
  - Module-statics (SsaoDiligent.cpp:24-27 pattern):
    `static std::unique_ptr<Bloom> bloom;`,
    `static HLSL::BloomAttribs bloomAttribs{};`, `static bool bloomOnFlag = false;`
  - `bloomInit`: `if (!device) return;` then
    `bloom = std::make_unique<Bloom>(device, Bloom::CreateInfo{false});`
    `bloomAttribs = {};` (`EnableAsyncCreation = false` — mirror SSAO; the
    first-frame placeholder still makes `bloomSRV()` valid, decision 5).
  - `bloomSettingsApply(bool e)`: `bloomOnFlag = e;` (Phase 4 extends the
    body to set `Intensity/Threshold/SoftTreshold/Radius`).
  - `bloomFrameBegin(ctx)`: `if (!bloom || !ctx || !taaPostFXContext())
    return; bloom->PrepareResources(device, ctx, taaPostFXContext(),
    Bloom::FEATURE_FLAG_NONE);` — every frame, size-gated no-op (decision 6).
  - `bloomExecute(ctx, colorSRV)`:
    ```cpp
    if (!bloom || !ctx || !colorSRV || !taaPostFXContext()) return false;
    Bloom::RenderAttributes ra;
    ra.pDevice = device;
    ra.pDeviceContext = ctx;
    ra.pPostFXContext = taaPostFXContext();
    ra.pColorBufferSRV = colorSRV;
    ra.pBloomAttribs = &bloomAttribs;
    return bloom->Execute(ra) == POST_FX_EXECUTION_STATUS_READY;
    ```
    (`pStateCache` left null — same as SSAO, SsaoDiligent.cpp:80-89.)
  - `bloomSRV()`: `return bloom ? bloom->GetBloomTextureSRV() : nullptr;`
- `c-engine/CMakeLists.txt:135-146` — add `renderer/diligent/BloomDiligent.cpp`
  to the `SKIP_PRECOMPILE_HEADERS` list (next to `renderer/diligent/
  SsaoDiligent.cpp` at 141; the TU is already globbed in, no include dirs —
  decision 7).

**Acceptance:** `./scripts/build.sh` clean (the new TU compiles against the
prebuilt `libDiligentFX.a`; no new link target needed).

## Phase 2 — lifecycle + frame-begin hooks

Files:

- `c-engine/renderer/diligent/DiligentRenderer.cpp`
  - `bloomInit()` next to `ssaoInit()` (line 350).
  - `bloomDestroy()` next to `ssaoDestroy()` (line 738).
- `c-engine/renderer/diligent/TaaDiligent.cpp`
  - `bloomFrameBegin(ctx);` next to `ssaoFrameBegin(ctx)` (line 1042) — the
    module stays warm and re-sizes with the `FrameDesc` every frame.

**Acceptance:** `./scripts/build.sh` clean; the renderer constructs/tears
down the `Bloom` object with the rest of the post-FX state (no crash on
init/destroy/resize — `PrepareResources` is a size-gated no-op).

## Phase 3 — execute + pointer-swap in `taaWorldResolve`

Files:

- `c-engine/renderer/diligent/TaaDiligent.cpp`, inside `taaWorldResolve`
  (1310). Insert **after** the SSAO-composite block (1367-1371) and **before**
  the renderScale downsample (1375-1381), so bloom is the last HDR pass before
  the downsample/CAS/blit (decision 3). Mirror the SSAO gating shape
  (1331-1333, 1367-1371):
  ```cpp
  // Bloom after the AO composite / TAA, before the downsample/CAS/blit — the
  // pointer-swap drops the bloom result (source + bloom, target-size 1:1)
  // into srcColorSRV so the box downsample / CAS / blit see it exactly like
  // any other resolved HDR frame. Skipped on the debug-MV blit (the motion
  // encoding must stay exact).
  if (!debugMv && bloomOn()) {
      if (bloomExecute(ctx, srcColorSRV)) {
          if (ITextureView* b = bloomSRV()) {
              srcColorSRV = b;
          }
      }
  }
  ```
  - `bloomExecute`/`bloomSRV` are the module's externs (Phase 1) — the same
    "private static exposed via accessors" pattern as SSAO.
  - The swap feeds `srcColorSRV` into the renderScale downsample (1375-1381)
    and the CAS/blit (1385-1394) unchanged: those PS already sample a
    `Texture2D<float4>` and the present is opaque, so the `R11G11B10_FLOAT`
    output's defined-1.0 alpha is inert (see Risk 1).
  - Bloom is independent of TAA/SSAO state: with TAA off, `srcColorSRV` is
    the raw `sceneColorTex` SRV and bloom still runs on it; with SSAO on,
    bloom reads the AO'd color (the physically-correct "what you see"
    source). This matches Tutorial27/Radient, which run bloom after TAA.
  - Wrap the `Execute` in `Diligent::ScopedDebugGroup g(ctx, "bloom")` so the
    pass shows by name in `scripts/rdc.py list` (the library's own
    `ComputePrefilteredTexture`/`ComputeDownsampledTexture`/
    `ComputeUpsampledTexture` groups nest inside, Bloom.cpp:298,320,349).

**Acceptance:** `scripts/rdc.py list` on a capture shows a `bloom` group
containing the library's three compute groups; `scripts/rdc.py dump bloom`
shows the full-frame output (source + bloom). With bloom off, no `bloom`
group. `ENGINE_SCREENSHOT` with `bloomDisabled=true` is pixel-identical to
the pre-change baseline; with bloom on, bright regions (sky, sunlit
surfaces) show additive halos. Repeat at `renderScale` 0.5 / 1.5 / 2.0 — the
bloom follows the scene (no full-res bleed).

## Phase 4 — settings flag forwarding (+ optional sliders)

The on/off flag is already 95% wired; the single required change:

Files:

- `c-engine/renderer/diligent/DiligentRenderer.cpp:806-809`
  - `applyGraphicsSettings`: add `bloomSettingsApply(s.bloom);` next to the
    existing `ssaoSettingsApply(...)` (line 809). This is the only break in
    the `bloomDisabled` → `GraphicsSettings.bloom` → `bloomSettingsApply`
    chain.

Optional follow-on (mirrors SSAO Phase 4; defer to a tuning pass) — expose
the four parameters from `HLSL::BloomAttribs`:

- `c-engine/renderer/Renderer.h` — add `float bloomIntensity = 0.15f;`,
  `float bloomThreshold = 1.0f;`, `float bloomSoftThreshold = 0.125f;`,
  `float bloomRadius = 0.75f;` (defaults = the fxh `DEFAULT_VALUE`s).
- `c-engine/renderer/Renderer.cpp` — read the new keys in
  `rendererGraphicsLoad` (`s.bloomIntensity =
  (float)utils::settingsGetDouble("bloomIntensity");` etc.); clamp in
  `graphicsNormalize` to the `UpdateUI` ranges (Bloom.cpp:461-475):
  Intensity 0..1, Radius 0.3..0.85, Threshold 0..10, SoftTreshold 0..1.
- `c-utils/settings/Settings.cpp` — add templates **with the exact types**
  next to `bloomDisabled` (line 63): `{"bloomIntensity","double",0.15}`,
  `{"bloomThreshold","double",1.0}`, `{"bloomSoftThreshold","double",0.125}`,
  `{"bloomRadius","double",0.75}`. A type mismatch rewrites the whole
  settings.json (docs/lessons.md 2026-09-04).
- `c-engine/renderer/diligent/BloomDiligent.cpp` — extend
  `bloomSettingsApply(bool, float, float, float, float)` to set
  `bloomAttribs.{Intensity,Threshold,SoftTreshold,Radius}`.
- `c-game/game/settingsGui/graphics/SettingsGraphicsGui.cpp` — forward the
  four values in `applyRenderer` (next to the `g.bloom` write at 134), load
  them (next to 244), add sliders following the existing
  `toggleX`/`persistX`/`syncLabels`/`rmlBind` pattern (308,375-376,484-486).
- `c-game/data/pak_1/gui/settings/graphics/graphics.html` — add the four
  widgets + labels to the bloom row (the `bloomLabel` binding at 280 already
  exists). (The on/off toggle already works — `toggleBloom` at 484.)

**Acceptance:** `ENGINE_SCREENSHOT` / in-game toggle of `bloomDisabled`
switches the effect live; the four sliders (when added) change intensity/
threshold/radius and persist across restarts (`settings.json` round-trips,
no full-file rewrite — diff the file after a toggle).

## Phase 5 — Verification

All headless, from the project root after `./scripts/build.sh`:

1. **Build:** `./scripts/build.sh` clean compile + link (no new archive; the
   `Bloom` symbols resolve from the existing `libDiligentFX.a` in the
   `--start-group`). No pak re-pack needed (no shader edits — bloom reuses
   the prebuilt library's `.fx` shaders, loaded by the library, not from
   `pak_1.pak`).
2. **Pass in the capture:**
   `ENGINE_RENDERDOC_CAPTURE=1 ENGINE_RENDERDOC_CAPTURE_FRAMES=300
   ENGINE_LOG_TIMEOUT=120000 ./scripts/run.sh renderdoc` then
   `scripts/rdc.py list` — expect a new `bloom` group (with the
   `ComputePrefilteredTexture`/`ComputeDownsampledTexture`/
   `ComputeUpsampledTexture` child groups, Bloom.cpp:298,320,349) after the
   `ssao`/TAA work and before the blit. `scripts/rdc.py dump bloom` shows the
   full-frame output. Bloom off → no `bloom` group.
3. **Before/after frames:** `ENGINE_SCREENSHOT=/tmp/bloom_off.jpg` with
   `bloomDisabled=true` in `data/settings.json` (or the debug overlay), then
   `bloomDisabled=false` + `ENGINE_SCREENSHOT=/tmp/bloom_on.jpg` — same
   vantage; bright areas brighter/haloed with bloom on, the rest
   unchanged. **Caveat:** `UpdateConstantBuffer` multiplies
   `PostFXContext::GetTransitionAlpha` into `AlphaInterpolation`
   (Bloom.cpp:277,283), so the effect ramps in over a short duration after
   (re)enable — `ENGINE_SCREENSHOT` (fired a few frames after startup) with
   bloom ON shows *partial* bloom by design. Take the "on" capture a frame
   past the ramp (or set a longer `ENGINE_RENDERDOC_CAPTURE_FRAMES`-equivalent
   delay) and the "off" baseline by `bloomDisabled=true`.
4. **renderScale sweep:** repeat the on/off pair at
   `renderScale = 0.5 / 1.5 / 2.0` — the bloom follows the scene at target
   size (decision 4); no full-res bleed or mis-registration.
5. **Resize:** change window size mid-run (or two launches at different
   sizes) — the context `FrameDesc` changes, `bloom->PrepareResources`
   rebuilds the pyramids (Bloom.cpp:87-141), the swap keeps flowing.
6. **Lessons pitfall check** (docs/lessons.md):
   - `R11G11B10_FLOAT` → `Texture2D<float4>` blit (Risk 1): the present is
     opaque and the PS use `.rgb`, so the defined-1.0 alpha is inert; confirm
     no driver VUID in the capture log and that the screenshot alpha looks
     right (no unexpected premultiplied edge).
   - Static-cbuffer-on-SRB rule (2026-09-06): the bloom constant buffer is
     set inside the library's own `InitializeSRB(true)`
     (Bloom.cpp:294,345) — we never touch it, so no pitfall.
   - Dynamic-buffer ring clobbering (2026-09-05): the bloom constant buffer
     is a `USAGE_DEFAULT` buffer updated via `UpdateBuffer` (Bloom.cpp:284),
     not a dynamic buffer — no ring pitfall.
7. **Perf sanity:** bloom adds a prefilter + mip-chain downsample/upsample at
   target size; measure with `ENGINE_DEBUG_GPUTIME=1` (DiligentRenderer.cpp)
   static + moving-camera before/after; budget is the same post-world
   envelope used in the SSAO/terrain plans.

## Risks / gotchas

1. **`R11G11B10_FLOAT` (3-channel) sampled as `Texture2D<float4>`** — the
   bloom output has no alpha channel; Vulkan defines the sampled 4th
   component as 1.0 for a 3-component float texture. Mitigated: the
   downsample/CAS/blit PS all use `.rgb` for color work (TaaDiligent.cpp:455-
   460, 647-649) and the present is opaque, so the alpha is inert. Verify by
   screenshot (the "on" image should differ from "off" only in the bright-
   area halos, with no alpha/premultiplied artifact). If it were ever a
   problem, the fix is a one-line `.rgb`-only read in the blit — but the
   existing `sceneColorTex` is RGBA16F and the chain already assumes an
   opaque scene, so this is low-risk.

2. **Bloom runs on the post-TAA, post-SSAO frame** — placement (decision 3)
   means bloom reads the TAA-accumulated / AO-composited color. With TAA on,
   the input is temporally stable (good — no bloom flicker from TAA jitter);
   with TAA off, bloom reads the raw `sceneColorTex` (still correct, just
   no temporal stability). This matches Tutorial27/Radient (bloom after TAA),
   so there is no ordering surprise. The only TAA×bloom interaction is the
   transition fade (Risk: `GetTransitionAlpha` ramp, Phase 5.3).

3. **`PostFXContext` sharing with TAA/SSAO** — all three consume the same
   context. `Bloom::Execute` reads `IsPSOsReady` from the context
   (Bloom.cpp:420), so if the TAA context's PSOs are still being created,
   bloom runs its placeholder (valid, decision 5). No new context, no
   contention. The one constraint is that `bloomFrameBegin` (Phase 2,
   TaaDiligent.cpp:1042) runs before `bloomExecute` (Phase 3) in the same
   frame — it does, since `taaFrameBegin` precedes `taaWorldResolve`.

4. **`--start-group` link line** — c-game links all Diligent archives inside
   one `--start-group` (c-game/CMakeLists.txt:11,64) and `libDiligentFX.a`
   already contains every `Bloom` symbol (nm-verified:
   `{ctor, dtor, PrepareResources, Execute, GetBloomTextureSRV, UpdateUI}`).
   No new archive. The one ABI trap is `UpdateUI`'s `ImGui::` calls against
   the lib's own imgui build — do not call it (decision in Current state).

5. **First-frame PENDING** — with `EnableAsyncCreation = false`, the first
   `Execute` may still return `PENDING` (PSO `IsReady`), running
   `ComputePlaceholderTexture` (input→output copy). The swap is then a
   no-op-visible change (bloom=0 for that frame), and frame 2 is full bloom
   (post the transition ramp). No gate needed; the `bloomExecute` return
   still guards the swap like SSAO's `ssaoRan`.

## Out of scope

Any new offscreen texture / RTV / world-PS change (SSAO's Phase 1 — empty for
bloom), a bloom-into-the-lighting-equation variant (the module's additive
`Source + bloom` is the intended application), auto-exposure / tone-
mapping integration beyond the existing encode-on-store (Radient's
auto-exposure is a separate pipeline we bypass), and any Filament-era path
(backend removed 2026-09-05). Screen-space reflections / GI are separate
DiligentFX modules, separate plan.
