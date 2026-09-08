# plan

**Task**: make ambient occlusion work even when TAA is off.

**Root cause** (verified in source): AO is DiligentFX ScreenSpaceAmbientOcclusion, and its
`ssaoExecute` call lives only inside the `if (taaOn && postFXContext && taa && cameraCB)`
block of `taaWorldResolve` (c-engine/renderer/diligent/TaaDiligent.cpp:1312); the
AO-composite pass just below it (`aoCompositeApply`, gated on `ssaoOn() && ssaoRan`) is the
only place AO is applied to the image. When TAA is off, `ssaoRan` stays false, so the AO
map is never produced and never composited — AO silently does nothing.

**Strategy**: the world always renders into the offscreen chain regardless of TAA state —
`taaFrameBegin` creates the targets unconditionally, the frame loop binds `taaColorRTV()`
whenever it exists, the world passes write the normal buffer (`normalTex`) and this
frame's depth (`depthTex[frameIdx & 1]`) every frame, and `ssaoFrameBegin` is already
called unconditionally in `taaFrameBegin`. So SSAO's inputs exist with TAA off. But
`ssaoExecute` is a *dependent* pass: it silently no-ops (returns PENDING, so `ssaoRan`
stays false and the composite is skipped) unless `PostFXContext::Execute` has run — the
only place `IsPSOsReady()` is set and the blue-noise texture is filled — and the TAA-off
path never calls it. So the TAA-off branch must run the same `postFXContext->Execute(pa)`
then `ssaoExecute(ctx, taaDepthSRV(frameIdx & 1))`, whenever `ssaoReady() && ssaoOn()`,
independent of `taaOn`.

**Approach**: minimal change inside `taaWorldResolve` only — keep the existing TAA-on
ordering (SSAO between `postFXContext->Execute` and `taa->Execute`, which the TAA-on
output depends on) and add a TAA-off branch (guarded by `!taaOn && postFXContext &&
cameraCB && ssaoReady() && ssaoOn()`) that builds `pa` identically to the TAA-on block
and runs the same `postFXContext->Execute(pa)` + `ssaoExecute(ctx, taaDepthSRV(curr))`
sequence (so `IsPSOsReady()` is set and blue noise is valid); then remove the now-stale
comment at the AO-composite guard
("TAA off / pending — the SSAO resolved texture is then undefined") which currently
documents the bug as intentional. No shader, PSO, texture, or DiligentRenderer.cpp
changes: the composite pass, bloom, downsample, CAS and blit stages all stay in the same
order, so the TAA-on path is untouched. The pinned verification is a headless A/B
screenshot run (TAA off + AO on vs TAA off + AO off, via `data/settings.json`) that
asserts the AO-on frame is measurably darker in the world area.

Verification: cd /media/extra/Projects/c/filament-game && printf '{"taaEnabled": false, "aoDisabled": false}\n' > data/settings.json && TERM=linux ENGINE_SCREENSHOT=/tmp/ao_taa_off.png ENGINE_SCREENSHOT_FRAME=300 ./scripts/run.sh > /tmp/ao_taa_off.log 2>&1 && printf '{"taaEnabled": false, "aoDisabled": true}\n' > data/settings.json && TERM=linux ENGINE_SCREENSHOT=/tmp/ao_taa_off_noao.png ENGINE_SCREENSHOT_FRAME=300 ./scripts/run.sh > /tmp/ao_taa_off_noao.log 2>&1 && python3 -c "import numpy as np, sys; from PIL import Image; a=np.asarray(Image.open('/tmp/ao_taa_off.png').convert('RGB'),dtype=np.float32); b=np.asarray(Image.open('/tmp/ao_taa_off_noao.png').convert('RGB'),dtype=np.float32); d=b-a; m=d.mean(); f=(d.mean(axis=2)>3.0).mean(); print(f'AO CHECK mean(noao-ao)={m:+.3f} frac>3={f:.4f}'); sys.exit(0 if (m>0 and f>0.01) else 1)"
