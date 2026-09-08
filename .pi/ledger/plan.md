# Plan

Strategy: the SSR scaffolding already exists and is committed at the baseline — `SsrDiligent.cpp` draws a terrain-only gbuffer (world normal xyz + roughness a, RGBA16F) gated by `ENGINE_SSR=1`, and `DiligentRenderer.cpp` runs it in a `ssr_gbuffer` debug pass. What is missing: (1) the actual DiligentFX `ScreenSpaceReflection` post-effect (created/executed per frame with camera CB, depth SRV, gbuffer SRV, motion input — the `PostFXContext` machinery already exists in `TaaDiligent.cpp`, and DiligentFX samples/docs live in cpp-thirdparty/diligent/DiligentFX as the API reference); (2) compositing the SSR radiance output into the final image; (3) the temporary terrain roughness reduction (clamp/override in `heightmap_terrain_ps.hlsl`, original value recorded in the ledger for revert); (4) removal of leftover `ssrdbg` fprintf debug prints in `ssrDiligentDestroy`. Keep all changes behind the `ENGINE_SSR` env gate where runtime-visible; no comments in new code (AGENTS.md). Visual verification is via `ENGINE_SCREENSHOT` + `ENGINE_SSR=1` screenshot diff against `repro_baseline.jpg`-era look.

Verification: ./scripts/build.sh && timeout -s KILL 40 bash -c 'ENGINE_SSR=1 ENGINE_SCREENSHOT=/tmp/verify_ssrfinal.jpg ./build/c-game/c-game'; ls -l /tmp/verify_ssrfinal.jpg

Baseline commit: 24edfc606f84760d0a0c331a255ae2cca945ad4d (dirty)
