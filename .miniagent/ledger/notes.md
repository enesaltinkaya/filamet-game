# notes

## Findings (setup round)

- AO = DiligentFX `ScreenSpaceAmbientOcclusion` (c-engine/renderer/diligent/SsaoDiligent.cpp);
  `ssaoExecute(ctx, depthSRV)` needs only the current-frame depth SRV + the normal buffer
  (`taaNormalSRV()`) + the shared `PostFXContext`. All of these are produced every frame
  regardless of TAA: the world always renders into the offscreen chain (`taaColorRTV()`
  is non-null even with TAA off — targets are created unconditionally in `taaFrameBegin`,
  TaaDiligent.cpp:1005), the world passes write `normalTex` and `depthTex[frameIdx & 1]`,
  and `ssaoFrameBegin` is called unconditionally (TaaDiligent.cpp:1043).
- The only gate is in `taaWorldResolve` (TaaDiligent.cpp:1312): `ssaoExecute` runs only
  inside `if (taaOn && postFXContext && taa && cameraCB)`, and the AO composite
  (`aoCompositeApply`) requires `ssaoRan` — so TAA off ⇒ no AO ever. The comment at
  lines ~1363-1368 ("Skipped ... whenever SSAO did not produce this frame's AO map
  (TAA off / pending...)") documents the bug; it must be updated with the fix.
- AO composite applies `c.rgb *= 1 - (1 - ao) * strength` (strength = ssaoIntensity,
  default 1.0) into `aoCompositeTex` at offscreen size, before bloom/downsample/CAS/blit.
  `aoCompositeTex` is created in `createTargets` alongside the TAA targets (same size as
  the offscreen chain), so it exists with TAA off too.
- Settings: `data/settings.json`, keys `taaEnabled` (bool, default true) and
  `aoDisabled` (bool, default false = AO on). The engine seeds missing keys in memory
  without rewriting the file, so a minimal settings.json with just these two keys is
  safe for headless runs.
- Headless run pattern: `TERM=linux ENGINE_SCREENSHOT=/tmp/x.png
  ENGINE_SCREENSHOT_FRAME=300 ./scripts/run.sh` — run.sh builds first (ninja incremental),
  the game auto-enters the world (no ENGINE_MENU/autotest), captures one frame, and
  `rendererScreenshotDeliver` calls `engineStop()` so it exits on its own (no
  ENGINE_LOG_TIMEOUT needed).
- PIL 12.3.0 + numpy 2.5.2 are available for the screenshot A/B comparison.

## brainstorm

## Core difficulty

The fix looks like a one-line gate removal, but DiligentFX SSAO is a *dependent* pass:
`ScreenSpaceAmbientOcclusion::Execute` does its real pipeline only when
`pPostFXContext->IsPSOsReady()` is true (otherwise it renders a placeholder and returns
PENDING), and `PostFXContext::IsPSOsReady()` is only set inside `PostFXContext::Execute` —
which is also the only place the blue-noise texture SSAO samples (`g_TextureBlueNoise`,
bound from `Get2DBlueNoiseSRV(ZW)`; created empty in the context constructor, filled by
`ComputeBlueNoiseTexture` inside `Execute`) gets filled. The TAA-off path never calls
`PostFXContext::Execute`, so naively calling `ssaoExecute` with TAA off silently yields
PENDING (no AO) or, if the context PSOs were somehow ready, garbage from an
uninitialized noise texture. The real fix is to reproduce the TAA-on call sequence
(`postFXContext->Execute` → `ssaoExecute`) in a TAA-independent branch.

## Reductions / key lemmas

1. The offscreen chain is TAA-independent (verified): `createTargets` unconditionally
   creates sceneColor/motion/normal/aoComposite/down + depthTex[0..1]; the frame loop
   binds `taaColorRTV()` + `taaDepthDSV()` and all world passes write all three color
   buffers every frame — glTF PBR via `PSO_FLAG_COMPUTE_MOTION_VECTORS` (WorldNormal
   `: SV_Target2`, GltfDiligent.cpp:511/1093, set unconditionally), terrain and props PSs
   both emit `Normal : SV_Target2` unconditionally. `ssaoFrameBegin` (SSAO
   PrepareResources) and `postFXContext->PrepareResources` already run unconditionally in
   `taaFrameBegin`. So every SSAO input exists with TAA off.
2. SSAO dependency chain in DiligentFX (verified in thirdparty sources):
   - `SSAO::Execute` runs 8 compute passes only if `PrepareShadersAndPSO &&
     pPostFXContext->IsPSOsReady()`, else `ComputePlaceholderTexture` + returns PENDING
     (so our `ssaoExecute` returns false → `ssaoRan` stays false → composite skipped —
     a silent no-op, not a crash).
   - `PostFXContext::Execute` is the only place `m_PSOsReady` becomes true, and the only
     place blue noise is computed. `PostFXContext::PrepareResources` does NOT create or
     fill it (it early-returns when the size is unchanged).
   ⇒ The TAA-off branch MUST call `postFXContext->Execute(pa)` before `ssaoExecute`.
   - `PostFXContext::Execute` DEV_CHECKs non-null curr/prev depth SRV, motion SRV, and a
     camera attrib source — all available with TAA off (depthTex[0..1] and motionTex are
     created and written every frame; `cameraCB` is created in `taaInit`).
3. Ordering invariance: TAA-on order is `postFXContext->Execute` → `ssaoExecute` →
   `taa->Execute` → `aoCompositeApply` → bloom → downsample/CAS/blit. The TAA-off analog
   is the same minus `taa->Execute`: the SSAO map is a per-frame spatial product of the
   current-frame depth+normal (SSAO's own temporal accumulation converges within a few
   frames; with TAA off the depth is unjittered, so the input is actually cleaner), and
   the AO composite is a 1:1 point-tap multiply (`1 - (1-ao)*strength`) whose target
   (`aoCompositeTex`), PSO and SRB cache (keyed by source texture) are all
   TAA-independent — the first TAA-off composite just lazily builds one new SRB.
4. Regression containment: keep the existing `if (taaOn && ...)` block byte-identical;
   only add a `!taaOn` counterpart. The TAA-on output (which the AO composite order
   depends on) cannot change.
5. Verification reduction: with TAA off, the only rendering difference between
   `aoDisabled: false` and `aoDisabled: true` is the AO composite multiply, so
   mean(noAO − AO) > 0 with a nontrivial fraction of pixels differing is a sound A/B
   assertion at a fixed frame (300; SSAO history has converged, camera path
   deterministic). Defaults give a visible effect: GTAO, radius 1.0, intensity 1.0.
6. Known non-issue: on the legacy path (`taaColorRTV()` null ⇒ `sceneColorTex` null)
   `taaWorldResolve` early-returns; there is no offscreen depth/normal, so AO cannot run
   there — acceptable, unchanged.

## Candidate approaches

1. **Minimal TAA-off branch in `taaWorldResolve`** — add
   `if (!taaOn && postFXContext && cameraCB && ssaoReady() && ssaoOn()) { postFXContext->Execute(pa);
   ssaoExecute(ctx, taaDepthSRV(frameIdx & 1)); }` (hoisting the `pa` construction out of
   the TAA-on block or duplicating it), and delete the stale comment block at
   lines ~1363-1368 which documents the bug as intentional (project rule: no comments —
   deletion, not rewrite). Risk: duplicating the `RenderAttributes` setup; pitfall is
   forgetting `postFXContext->Execute` (PENDING/garbage, silent). Effort: small, ~20 LOC
   in one file.
2. **Hoist SSAO + context Execute out of the TAA block entirely** — restructure so
   `postFXContext->Execute` + `ssaoExecute` always run, then the TAA block (minus SSAO)
   runs conditionally. Cleaner long-term, but touches the TAA-on sequence (the very thing
   the plan says must stay untouched) — higher regression risk for zero functional
   gain. Effort: medium.
3. **Move AO to the frame loop in DiligentRenderer.cpp** — run Execute+ssaoExecute
   after the world passes, independent of `taaWorldResolve`, and pass the AO-applied SRV
   in. Architecturally cleanest (AO is not a TAA feature) but requires plumbing through
   the renderer and touching the composite call site — largest surface. Effort:
   medium-large.
4. **Document AO-requires-TAA and ship** — rejects the task. Not an option.

## Recommended approach

Approach 1. It is the only option that provably leaves the TAA-on path untouched while
restoring AO with TAA off, and the single load-bearing requirement — calling
`postFXContext->Execute(pa)` in the new branch (identical `RenderAttributes`: curr/prev
depth SRVs from `depthTex[frameIdx & 1]` / `depthTex[prev]`, `motionTex` SRV, `cameraCB`)
followed by `ssaoExecute(ctx, taaDepthSRV(frameIdx & 1))` — is what makes
`IsPSOsReady()` true and the blue-noise texture valid. For it to work: the offscreen
targets must exist (they do — verified), the composite guard `ssaoOn() && ssaoRan`
stays as-is, and the first frame(s) may return PENDING (harmless: composite skips and
history converges well before frame 300).

## Proposed tasks

1. Implement the TAA-off SSAO branch in `taaWorldResolve` (TaaDiligent.cpp:1312): same
   `postFXContext->Execute(pa)` + `ssaoExecute(ctx, taaDepthSRV(frameIdx & 1))` sequence as
   the TAA-on block, guarded by `!taaOn && postFXContext && cameraCB && ssaoReady() &&
   ssaoOn()`, setting `ssaoRan`; leave the TAA-on block byte-identical; delete the stale
   comment block at the AO-composite guard (lines ~1363-1368) rather than rewriting it.
   Verify the build (ninja incremental via `./scripts/run.sh` or direct build).
2. Run the pinned headless A/B verification from plan.md (TAA off, AO on vs AO off,
   frame-300 screenshots, mean-diff assertion). If the AO-on frame is not measurably
   darker, capture with `ENGINE_RENDERDOC_CAPTURE=1 ./scripts/run.sh renderdoc` and use
   `scripts/rdc.py list`/`dump` to inspect the `ssao` pass's resolved AO output and
   confirm `PostFXContext` common passes actually ran that frame.
3. TAA-on regression check: one headless run with `taaEnabled: true` (AO on) — same
   darkness assertion against a TAA-on + AO-off run, proving the untouched TAA-on path
   still composites AO (i.e. the refactor didn't disturb ordering).
