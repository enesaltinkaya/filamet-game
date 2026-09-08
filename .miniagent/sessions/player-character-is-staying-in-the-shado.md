# Scaffold session

- Task: player character is staying in the shadow of a tree.
  tree is just out of view, behind the camera.
  its shadow looks bad.
  i think we are seeing 2 cascade levels.
  both are cut off.

you can take a screenshot to observe.
and you can use renderdoc to inspect.

- Date: Mon Sep 7 10:38:57 2026
- Outcome: no clean sign-off
- Last verifier verdict: FAIL

## Final report

Scaffold finished without a clean sign-off (last verifier verdict FAIL). Final worker report:

## Task result

done — root cause confirmed, pad fix (casterPad 1.35 → 2.8) implemented in `ShadowDiligent.cpp`, rebuilt, and verified: the behind-camera tree shadow now matches a single-cascade ground truth (mask IoU 0.984 vs 0.537 pre-fix), continuous with no cascade cuts or two-tone banding.

## Files changed

- `c-engine/renderer/diligent/ShadowDiligent.cpp` — default `casterPad` 1.35f → 2.8f (one line; `ENGINE_SHADOW_CASTER_PAD` env A/B override kept)
- `docs/lessons.md` + `docs/lessons/2026-09-07.md` — lesson entry: behind-camera casters are clipped by the StabilizeExtents light cubes; caster FOV pad is the only in-engine lever; cost ≈2× coarser effective shadow texel density
- `.miniagent/ledger/notes.md` — appended `## final` with root-cause evidence, pad A/B numbers, and artifact locations

## Findings

- **Root cause (tasks 2/3/4):** the tree sits outside the near cascades' light _cubes_ (StabilizeExtents = frustum min bounding sphere → light-space AABB). Proven on GPU via the in-engine `ENGINE_SHADOW_READBACK`/`ENGINE_SHADOW_DUMP` depth dumps (cascade-0 atlas shows the clipped hexagon; no RenderDoc replay needed) and in code (`GetFrustumMinimumBoundingSphere`, `DistributeCascades`; back reach behind the eye ≈ zEnd·(0.919+√k2) for this oblique sun; negative near is blocked by `VERIFY(fCascadeNearZ > 0)`).
- **Pad curve (task 7):** IoU vs single-cascade GT (shadowQuality 0 render, settings.json restored after): base 0.537 → 2.4: 0.971 → 2.6: 0.981 → 2.8/3.0/3.2/3.5/4.0: 0.984 (saturated). Chose 2.8 as the smallest fully-fixing value.
- **Verification (task 6):** post-rebuild run _without_ any env var (`/tmp/shot_final.jpg` + player crop): shadow is one smooth canopy shape matching the GT silhouette (incl. the far arc edge); the lit 0–2.6 m band and the tonal step are gone (in-shadow brightness now uniform ~22–27).
- Caveat: this fixes the reported geometry; a caster farther back than ~the cube's new reach would clip again — a true backward caster-frustum extension (third-party change) is the follow-up if needed. Residual 1.6% mask diff vs GT is only soft-edge blur (1024 single-cascade GT vs 4096 cascaded).
- Housekeeping: `build/c-game/data/settings.json` temporarily set to shadowQuality 0 for the GT render and restored to 4; deleted the stale 643 MB `/tmp/RenderDoc/c-game_frame300.rdc`. Screenshots/atlas views kept in `/tmp` (paths in notes.md).

## Remaining steps

none for the reported symptom (optional follow-ups noted in notes.md: targeted backward caster extension or per-cascade pad if near-shadow sharpness regresses).
[usage: ↑4521069 ↓35119 tokens]

Verifier:

## Verdict

FAIL

## Commands run

- `ENGINE_SCREENSHOT=/tmp/shot.jpg ./build/c-game/c-game` (exact Verification line from plan.md) — **exit 139 (segfault)**
- `nvidia-smi --query-gpu=memory.used,memory.total --format=csv` — **exit 0**: 31988 MiB used of 32607 MiB total
- `nvidia-smi --query-compute-apps=...` — **exit 0**: pid 75411 holds 31922 MiB
- `ps -o pid,ppid,etime,comm -p 75411` — **exit 0**: `VLLM::EngineCor`, running 33 min
- Retry: `ENGINE_SCREENSHOT=/tmp/shot2.jpg ./build/c-game/c-game` — **exit 139**, identical failure

## First failure

```
[ERROR] diligent: Failed to allocate device memory 'Device memory page. Size: 64.00 MB, type: 1'
VK Error Code: ERROR_OUT_OF_DEVICE_MEMORY
[ERROR] diligent: Failed to create Texture 'taaSceneColor'  (2880x1627 RGBA16_FLOAT)
...
Segmentation fault (core dumped)   # engine crashes when texture creation fails
```

## Notes

- **Root cause is environmental, not the changed code.** An unrelated VLLM inference server (pid 75411, parent 75195, up ~33 min) is holding 31.9 GB of the 32 GB RTX 5090, leaving no room for the engine's TAA textures. The game itself initializes cleanly through pak load, GUI, and font/asset setup before hitting the OOM.
- **I did not kill pid 75411** — it's someone else's running service and out of verifier scope. To re-verify: stop/resize the VLLM server (or run on another GPU), then re-run `ENGINE_SCREENSHOT=/tmp/shot.jpg ./build/c-game/c-game` and check the screenshot for the cascade-boundary shadow artifact.
- **Secondary code issue worth flagging to the worker:** the engine segfaults (exit 139) instead of failing gracefully when a texture allocation fails — it dereferences null textures created after the OOM. Worth a null-check in the TAA setup path, but it is not what this verification line was designed to catch.
- No screenshot was produced (crash before first capture frame); the plan's visual confirmation (continuous tree shadow) could not be checked.
  [usage: ↑31085 ↓154 ...
