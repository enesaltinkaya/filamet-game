# notes

## brainstorm

## Core difficulty

"Too much GPU time" is asserted, not measured — the terrain pass (up to 9 chunked `DrawIndexed` calls in the TAA offscreen world pass) has at least three independent cost sources (per-pixel shader work, screen coverage × TAA scale, draw-call/state overhead), and the fix differs for each, so the first round must attribute time rather than guess.

## Reductions / key lemmas

1. PS sample census from `splat_terrain_ps.hlsl` (worst case per pixel): 2 weight-array + 2 base (albedo/normal) + 16 splat chain (2 groups × 4 details × albedo+normal, `SampleGrad`) + 6 band (sand/cliff/snow × albedo+normal, **unconditional**) + 1 BRDF LUT + 1 irradiance cube + 1 prefiltered env cube (SampleLevel) + 4 shadow PCF taps (`SampleCmpLevelZero`) = **up to 27 texture samples**, all in a pixel shader that also writes 3 render targets (RGBA16F color + RG16F motion + RGBA16F normal, plus D32).
2. The single largest removable block is the 16-sample splat chain — it already early-outs on `influence == 0`, and the loader notes the weight data covers only ~17/100 UDIM tiles, so in Noop regions the PS already skips it. The 6 band samples have **no** early-out even though at most one of wSand/wCliff/wSnow is usually non-zero (altitude/slope-disjoint smoothsteps).
3. The explicit-grad `SampleGrad` detail sampling disables hardware anisotropy (noted in the shader header), so the 16x aniso on `g_DetailSampler` is likely dead cost-wise; the real cost is the 22 full-res 2D/array samples.
4. Coverage is bounded by two knobs already in the code: the 1-chunk camera window (≤ 3×3 = 9 chunks, `ENGINE_SPLAT_WINDOW=0` to compare) and the TAA offscreen resolution — if the world pass runs at 2x, terrain pixel cost is 4x; checking that number is a 5-minute read of the TAA chain setup.
5. Draw overhead: ≤ 9 chunks × (SetVertexBuffers + SetIndexBuffer + 2 transitions + DrawIndexed) per frame. Non-trivial on AMD only if chunks are small; the existing log `splatTerrain: frame N — X/Y chunks drawn` already reports the drawn count, so overhead-vs-fill can be separated by measuring with `ENGINE_SPLAT_WINDOW=0` (100 chunks) vs windowed (≤ 9) — if the delta is small, it's fill-bound.
6. The engine already has the exact machinery needed for cheap shader ablation: `ENGINE_SPLAT_DETAIL_METERS` is prepended as a `#define` into the runtime-compiled HLSL (`createSplatHlsl`), so env-gated `#define`s that zero out shader blocks (IBL, PCF, splat chain, bands) are a small, reversible edit with no repak.

## Candidate approaches

**A. Measure with RenderDoc first** — `run.sh renderdoc` capture, read per-pass GPU timings (qrenderdoc performance tab, or the Python replay module if it exposes pass timing), compare the `terrain` group against the rest of the world pass, and read the TAA offscreen scale. Risk: the Python replay API may not expose pass timings headless (qrenderdoc GUI does; the layer build is x11-only), so a GUI session or adding Vulkan timestamp queries may be needed. Effort: low.

**B. Shader ablation via env-gated #defines** — add `ENGINE_SPLAT_ABLATION` values (`chain`, `bands`, `ibl`, `pcf`) that prepend `#define`s stripping one cost block each; run with a fixed camera (or the `ENGINE_SCREENSHOT` frame) and compare frame times / RenderDoc pass times. Risk: ablation is per-variant manual runs (no automatic sweep), and visual output is broken by design — must not be left in a screenshot-compare path. Effort: low-medium (one shader edit + run harness).

**C. Cheap early-outs in the PS** — guard the 6 band samples on `wSand > 0 / wCliff > 0 / wSnow > 0` (the smoothstep values are already computed); the splat chain is already guarded. Visually exact (a zero weight multiplies the sample contribution to nothing), so no parity risk beyond branch cost. Effort: trivial, but it only helps if bands/texture fetch latency is actually the measured culprit (AMD hides latency well; 6 of 27 samples may be a small share).

**D. Structural changes** — repack splat details as combined albedo+normal textures (halves the 22 samples to ~11), merge the 9 chunk draws into 1 instanced/mega-mesh draw, enable the early-Z pipeline flag (note: 3 color targets + always-written motion/normal may already defeat early-Z on AMD). Risk: repacking touches the asset pipeline (chunker/Blender output); early-Z may be a no-op. Effort: high.

## Recommended approach

A → B → C in order: measure the terrain pass's actual share and the TAA scale (A), ablate the four cost blocks (B) to find which of the 27 samples / PCF / IBL dominates, then land the targeted fix (C, or a subset of D only if the measurement shows structural cost). This works if the engine runs long enough in headless mode to produce stable timings — the existing `ENGINE_LOG_TIMEOUT` / capture-frame machinery already supports fixed runs. If RenderDoc pass timing is unavailable headless, fall back to adding one Vulkan timestamp query pair around the splat pass (it's already a labeled `ScopedDebugGroup`, so the site is known).

## Proposed tasks

1. **Measure the baseline**: run the `run.sh renderdoc` capture; record the `terrain` pass GPU time vs the rest of the frame, the TAA offscreen resolution (read the TAA chain setup), the drawn chunk count from the `splatTerrain` log line, and the terrain's screen coverage. Deliverable: numbers appended to notes.md.
2. **Ablation harness**: add `ENGINE_SPLAT_ABLATION={chain|bands|ibl|pcf}` as prepended `#define`s in `createSplatHlsl`/`splat_terrain_ps.hlsl` (each variant strips one cost block, compiles cleanly, renders broken-but-valid); run each variant with the same fixed camera and record terrain pass time deltas. Deliverable: a per-block cost table.
3. **Land the confirmed fix**: apply the change targeting the measured top cost (expected: band sample early-outs; alternative: IBL/PCF simplification) and verify visual parity against an `ENGINE_SCREENSHOT` reference at the same camera.
4. **Verify the win**: re-capture with RenderDoc, confirm the terrain pass time dropped by the predicted share, and append before/after numbers to notes.md.
