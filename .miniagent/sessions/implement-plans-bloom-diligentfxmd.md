# Scaffold session

- Task: implement plans/bloom-diligentfx.md
- Date: Tue Sep  8 17:09:28 2026
- Outcome: no clean sign-off
- Last verifier verdict: PASS

## Final report

Scaffold finished without a clean sign-off (last verifier verdict PASS; finalizer: round 5: manager re-dispatched task 4 (no progress)). Final worker report:



## Task result

done — finished the run's two outstanding tasks (5: build, 6: verification) for `plans/bloom-diligentfx.md`; verification exposed a real defect (bloom was numerically a no-op) which I fixed with a minimal, documented deviation, then re-verified end-to-end. All 6 tasks in tasks.json are now done.

## Files changed

- `c-engine/renderer/diligent/BloomDiligent.cpp` — `bloomInit` now seeds `bloomAttribs` (Intensity 0.15, Threshold 0.5, SoftTreshold 0.125, Radius 0.75) instead of leaving the plan's zero-init `BloomAttribs{}`
- `.miniagent/ledger/tasks.json` — tasks 5 & 6 marked done with one-line results (JSON validated)
- `.miniagent/ledger/notes.md` — appended `## round 5 (final)` with findings/remaining

## Findings

- **Why the run had no clean sign-off:** with the plan's zero-init attribs, bloom was a strict no-op for two compounding reasons: (1) on our Vulkan/RADV device the library's final upsample takes the non-indexed draw path (`Draw({3,flags,1,3})` → uInstID=3 → the `lerp(Source, Source+Intensity*ColorSum, α)` branch — the `SourceColor+ColorSum` branch only runs on devices without `ShaderBaseVertexOffset`), so Intensity=0 zeroes the contribution; (2) even the fxh defaults are invisible here because the scene's HDR buffer peaks at ~0.87 < Threshold 1.0, so the prefilter passes nothing.
- **Fix verified quantitatively:** RenderDoc dump diff (bloom output vs its real input, the ssao-composite draw): 53% of pixels brightened, mean +25.9/255 in bright regions vs +2.0 elsewhere, max halo +103/255, no darkening beyond R11G11B10 quantization. `bloom` group present when on (429–533; 27 groups/139 draws), absent when off (22/123 — exactly the 16 bloom draws). Screenshot A/B shows canopy halos + brighter lit ground, dark areas unchanged; renderScale=2 shot shows target-size bloom (no full-res bleed). Zero VUIDs with the validation layer; on/off toggle reaches the gate live (settings chain intact); GetTransitionAlpha ramp confirmed one-time, so frame-900 shots show full-strength bloom.
- **Gotchas recorded in notes for later workers:** rdc.py dump PNGs are per-image auto-normalized (compare raw values vs the pass's *actual* input, not an earlier same-buffer pass); `data/settings.json` lives at `build/c-game/data/settings.json`; separate-run screenshot A/B is contaminated by world animation (RenderDoc checks are definitive); captures overwrite the fixed `c-game_frame300.rdc` name.
- **Deviation:** Threshold 0.5 (vs fxh/plan 1.0) is hardcoded in `bloomInit` because the plan's optional sliders are out of scope but the plan's own Phase 5 requires visible halos. If the sliders get built, they should replace these via `bloomSettingsApply`.

## Remaining steps

- Optional follow-ups (new scope, recorded in notes): the four bloom sliders (settings keys + `SettingsGraphicsGui` wiring), tuning Threshold/Intensity to taste, and considering a `taaOn()==false` resolve-path gate. Nothing blocking.
[usage: ↑3875955 ↓40844 tokens]

Verifier:



## Verdict

PASS

## Commands run

- `./scripts/build.sh` — exit code 0, ninja: no work to do (build is up-to-date)
- `ls -la c-engine/renderer/diligent/BloomDiligent.{h,cpp}` — both files present
- `grep -n "bloom" c-engine/renderer/diligent/DiligentRenderer.cpp` — confirms `bloomInit()` (L352), `bloomDestroy()` (L741), `bloomSettingsApply(s.bloom)` (L813)
- `grep -n "bloom" c-engine/renderer/diligent/TaaDiligent.cpp` — confirms `bloomFrameBegin(ctx)` (L1044) and the `!debugMv && bloomOn()` gated execute block with `ScopedDebugGroup g(ctx, "bloom")` (L1375–1378)

## First failure (if FAIL)

N/A

## Notes

Build is fully up-to-date (ninja reports no work). All five integration points from the plan are in place: init/destroy in DiligentRenderer, frameBegin + execute in TaaDiligent, and settings-apply in `applyGraphicsSettings`. The "bloom" debug group is present for RenderDoc inspection.
[usage: ↑14507 ↓844 tokens]
