# Scaffold session

- Task: make a detailed plan to implement bloom from diligentfx.
inspect samples, tutorials and Radiant implementation.
- Date: Tue Sep  8 16:51:59 2026
- Outcome: no clean sign-off
- Last verifier verdict: PASS

## Final report

Scaffold finished without a clean sign-off (last verifier verdict PASS; finalizer: round 1: manager decision unparseable (final reply: '')). Final worker report:



All files are complete and verified:
- `plans/bloom-diligentfx.md` — full plan (all sections mirroring `ssao-diligentfx.md`)
- `.miniagent/ledger/tasks.json` — valid JSON, all 5 tasks `done` with one-line results
- `.miniagent/ledger/notes.md` — appended `## final` with Findings + Remaining steps

## Task result

done — wrote the missing deliverable `plans/bloom-diligentfx.md` (mirroring `ssao-diligentfx.md`) after re-verifying every claim against the live tree, and marked all 5 tasks done in `tasks.json`.

## Files changed

- `plans/bloom-diligentfx.md` — new: full implementation plan (current state, 2 gaps, 7 approach decisions, Phases 1-5 with file:line anchors, 5 risks, out of scope)
- `.miniagent/ledger/tasks.json` — all 5 tasks → `done` with one-line results (JSON stays valid)
- `.miniagent/ledger/notes.md` — appended `## final` Findings + Remaining steps

## Findings

- The run's gap was the **deliverable document itself**, not the research — the `notes.md` brainstorm was thorough, but `plans/bloom-diligentfx.md` never existed. I re-verified the brainstorm's claims against the actual files this round; all held.
- **Bloom is strictly simpler than SSAO.** Its only external input is the HDR color SRV (already the TAA-accumulated/`sceneColorTex` frame) and its output **is** the final HDR color (`GetBloomTextureSRV()` = `SourceColor + ColorSum`, confirmed in `Bloom_ComputeUpsampledTexture.fx` `uInstID==0`). So SSAO's "Phase 1" (new normal RTV) and AO-composite "Phase 3" are empty; integration is a **pointer swap** in `taWorldResolve`.
- **The `bloom` settings flag already flows end-to-end** (`GraphicsSettings.bloom` → `bloomDisabled` key → settings GUI) — the *only* break is `applyGraphicsSettings` (DiligentRenderer.cpp:806-809) not forwarding `s.bloom`. That one line is the entire settings wiring.
- **No new link target, no pak re-pack.** `libDiligentFX.a` is already in the `--start-group` and `nm` confirms all `Bloom` symbols; bloom reuses the prebuilt lib's `.fx` shaders, so unlike SSAO there's no shader edit / `pak_1.pak` regeneration.
- **`SsaoDiligent.{h,cpp}` is the proven in-tree template** to copy for `BloomDiligent` (module-statics, `device` extern from DiligentRenderer.h:19, `taaPostFXContext()` accessor, `SKIP_PRECOMPILE_HEADERS` entry at CMakeLists.txt:141).
- App point verified against both references (Tutorial27 + Radient: bloom runs after TAA/DoF, before the final tonemap), so the plan's placement (after the SSAO composite 1367-1371, before the renderScale downsample 1375-1381) is correct.
- Main risk flagged: `R11G11B10_FLOAT` (3-channel) output bound to the existing `Texture2D<float4>` blit/CAS/downsample PS — low-risk because the present is opaque and the PS use `.rgb`; verify by screenshot.

## Remaining steps

Implementation (all S, not part of this planning task): Phase 1 create `BloomDiligent.{h,cpp}` + CMake entry; Phase 2 wire `bloomInit/bloomDestroy/bloomFrameBegin`; Phase 3 execute + `srcColorSRV = bloomSRV()` in `taWorldResolve`; Phase 4 forward `s.bloom` in `applyGraphicsSettings` (optional: 4 sliders); Phase 5 verify via build + RenderDoc + screenshots. See `notes.md` `## final`.
[usage: ↑810410 ↓28558 tokens]

Verifier:



## Verdict

PASS

## Commands run

- `./scripts/build.sh` — exit code 0 (ninja: "no work to do"; build is up to date and clean)

## First failure

None.

## Notes

The verification command (`./scripts/build.sh`, the exact line from the plan's `Verification:` line) passed cleanly with exit 0. The deliverable `plans/bloom-diligentfx.md` is present (24,722 bytes), and the engine binaries (`build/c-engine`, `build/c-game`) are built and up to date. Nothing indicates failure, flakiness, or a timeout.
[usage: ↑9507 ↓478 tokens]
