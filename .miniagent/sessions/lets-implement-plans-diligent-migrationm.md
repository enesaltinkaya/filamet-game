# Scaffold session

- Task: lets implement plans/diligent-migration.md
- Date: Wed Sep  2 16:08:19 2026
- Outcome: no clean sign-off
- Last verifier verdict: PASS

## Final report

Scaffold finished without a clean sign-off (last verifier verdict PASS). Final worker report:



## Task result

done — finished Phase 0 prep (task 1): GLB inspected, re-export question settled by A/B evidence, 4 baseline screenshots captured, Diligent quickref written to notes.md. One deliberate deviation: the "re-export without `-kn -kv -ke`" step was voided (details below).

## Files changed

- `scripts/build-terrain.py` — comment only on the gltfpack call: documents that `-kn -kv -ke` are keep-flags (named nodes, TEXCOORD_0/TANGENT, extras) and why they must stay (6 lines added, no behavior change)
- `.miniagent/ledger/notes.md` — appended `## final` with GLB inspection, A/B test results, baseline notes, and the full Diligent quickref

## Findings

- **The plan's premise about gltfpack flags is wrong.** gltfpack 1.0 `-kn -kv -ke` are KEEP flags (keep named nodes / keep vertex attributes / keep extras), not string compression — compression would be `-c/-cc/-cz/-cf`, which are not used. A/B test (blender→terrain-chunker→gltfpack, all into /tmp): with the flags, output is structurally identical to the checked-in GLB (same 100 `terrain_chunk_X_Y` names, extras, POSITION/TEXCOORD_0/TANGENT/NORMAL, same 11,553,484-byte buffer); without them, the 100 named nodes collapse into **one anonymous node** and TEXCOORD_0 (the 0..10 UDIM UVs driving the splat) + TANGENT are stripped. Dropping the flags would violate the task's own acceptance criterion (`gltfEntitiesNamed` table unchanged).
- **No re-export is needed at all**: the current GLB is already plain JSON with 0 meshopt bufferViews, `extensionsRequired=[KHR_mesh_quantization]` only, and **0 embedded textures/images** — all terrain textures are external BC7 KTX2 loaded by the game's own path, so Diligent's GLTFLoader (tinygltf-based in this version, not cgltf) can load it as-is.
- **Baseline screenshots**: `/tmp/shot_diligent_baseline_{menu,world,topdown,close}.jpg` (2880x1627), via `scripts/run.sh` + `ENGINE_AUTOTEST=enter` (world shots) + `ENGINE_CAMERA=topdown|close`. Visually verified (menu panel+logo, splat terrain in all camera modes).
- **Gotcha: `ENGINE_LOG_TIMEOUT` is in milliseconds** (`atof(env)*MILLION` nanos in Engine.cpp) — with 90 the run died in 90 ms before frame-3's readback, so no JPEG was ever written; `ENGINE_LOG_TIMEOUT=300` works and the run self-terminates in ~1 s. Use ≥300 in all validation runs.
- **Quickref corrections for Phases 1–4**: `ReadPixelsFromBuffer` does not exist in this Diligent version — screenshot readback must be CPU-access buffer + `IDeviceContext::CopyTexture(CopyTextureAttribs)` + `Map`; interfaces use `VIRTUAL void METHOD(...)` macros (no plain virtuals); headers live in source-tree `*/interface` dirs (`build-linux/include` holds only glslang); RenderStatePackager header is under `include/`, not `interface/`; best reference app is `DiligentSamples/Samples/GLTFViewer` (GLTF + PBR + IBL via `SetIBLResourceViews` + bbox + animation end-to-end).

## Remaining steps

- Task 2 (Phase 1 core renderer) per plan, applying the readback and `ENGINE_LOG_TIMEOUT` corrections above.
- Tasks 3–6 per plan; Phase 2 can feed the existing GLB straight into `GLTF::Model` (no asset pipeline changes).
[usage: ↑3607463 ↓31554 tokens]

Verifier:



## Verdict

PASS

## Commands run

- `./scripts/build.sh` — exit 0 (`ninja: no work to do`, incremental build clean)
- `ls -la build/c-game/c-game` — binary present (read-only sanity check)

## First failure (if FAIL)

None.

## Notes

Build is incremental (nothing to compile), so this confirms the tree is in a consistent, already-built state rather than exercising a full clean build. No test target exists in the Verification line.
[usage: ↑6373 ↓315 tokens]
