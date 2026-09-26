# World load time — optimization plan

ENTER WORLD blocks the main thread ~0.75–1.0 s (menu freeze, no loading
screen) and the first world frames stutter for another ~0.3–0.5 s of runtime
PSO compilation. Measured 2026-09-26 (gdb breakpoint phase-timing + perf,
warm and fadvise-cold page cache — identical within noise, the disk is NVMe:
this is a CPU-work + serialization problem, not I/O).

Baseline (autotest enter, warm):

    boot → menu interactive                    ~250 ms
    loadWorld (sync, MainMenuGui.cpp:68)      ~650–1000 ms
      gltfSceneLoad  (terrain GLB, Diligent)    ~25 ms
      splatTerrainLoad                        ~310–550 ms
      gltfPropsLoad  (test2.zstd, 63 MB)      ~160–190 ms   (all libpng)
      gltfLoad eve   (eve.zstd, 16 MB)        ~190 ms
      animations + re-gltfInit                 ~20 ms
    first-frame PSO compiles after load       +~300–500 ms  (glslang + spirv-opt
                                                             + RADV pipelines)

## Phase 1 — Pack used weight-tile layers (−~300 ms, −~1 GB VRAM) ✦ biggest

`createWeightArray` (SplatTerrainDiligent.cpp:242) always stages
100 layers × 1024² × 11 mips = 546 MB per group (`WEIGHT_LAYERS = 100`,
line 63), Noop-fills every unused layer (line 268 loop), then Diligent
copies it into a fresh 1 GB host-visible page + 1 GB device-local page and
uploads it. Actual tiles: grass1 11/100, roads1 6/100 → ~93 MB of real data
inside 1.07 GB staged + uploaded (91 % filler). First-touch page faults on
the 546 MB vector also make the first array erratic (observed 154→389 ms
between runs).

The shader indexes by absolute UDIM layer
(splat_terrain_ps.hlsl:703-717: `layer = tile.y * 10 + tile.x`, then
`g_Weights0/1.Sample(float3(local, layer))`), so packing needs a remap.

Steps:

1. Loader: build the packed layer set = union of used udims across BOTH
   groups + one Noop layer (shared layout keeps one remap for
   `g_Weights0`/`g_Weights1`; unused slots point at the Noop layer).
   `createWeightArray` takes the packed list + a `u8 udimToLayer[100]`
   table instead of the fixed `tiles[WEIGHT_LAYERS]`.
2. Staging shrinks to (usedLayers+1) × 5.6 MB ≈ 100 MB total; skip the
   per-byte alpha fill for the single Noop layer (write it once, memcpy per
   unused udim is gone — only the packed Noop layer is filled).
3. Remap delivery: tiny `Texture2D g_LayerRemap` (100×1 R32_UINT, created
   from `udimToLayer`); PS does
   `layer = g_LayerRemap.Load(int3(tile.x, tile.y, 0)).r` before both
   weight samples. NOT a cbuffer array — per-pixel (non-uniform) indexing
   into UBO arrays is UB in Vulkan.
4. splat_terrain_shadow_ps.hlsl / any other g_Weights consumer gets the
   same remap.
5. Log line change: `weight array '<g>' N packed layers of 100 udims`.

Validation: identical screenshot A/B at the parked vantage (TAA noise floor
~1.8 % px, run a control pair — lessons 2026-09-09); MemoryManager log shows
the 1 GB pages gone; createWeightArray timing drops to ≤ 30 ms.

DONE 2026-09-26. Measured (ENGINE_LOAD_TIMING): createWeightArray 16.2 +
15.1 ms (was ~310–550 ms for the phase), splatTerrainLoad 75–77 ms,
loadWorld total ~461 ms; 14 packed layers (11 grass1 + 6 roads1 tiles, 3
udims shared); largest MemoryManager page for the arrays now 128 MB (was
1 GB); screenshot A/B at the parked vantage 0.01 % px Δ>30 vs 0.01–0.04 %
same-build control floor — visually identical. The shadow PS is empty
(no g_Weights consumer) — step 4 was a no-op.

## Phase 2 — Props + player textures as KTX2 (−~200 ms, smaller pak)

`test2.zstd` (63 MB) and `eve.zstd` embed PNG textures; tinygltf →
`Diligent_DecodePng` → libpng decodes them single-threaded at load
(inflate_fast + paeth + crc32 ≈ the whole 160–190 ms props phase — verified
by backtrace). Everything else in the pak already ships KTX2 (raw RGBA8 +
KTX_SS_ZSTD via toktx, see plans/blender-terrain.md; some props textures
already are external — `images/models/test2/eve_material.001_normalTexture.ktx2`).

Steps:

1. scripts/export-models.sh: after gltfpack, post-process the staged GLB
   (python, like gltf-standardize.py) — extract embedded PNG bufferViews →
   toktx → `images/models/<model>/<name>.ktx2` external files + rewrite the
   image URI; drop the buffer views from the GLB.
2. Rebuild pak (data.sh), confirm the log shows ktx2 texture creates for
   props/eve and no `Diligent_DecodePng` in a perf profile.
3. Visual A/B on props + player (same control-pair rule).

BC7 (like the preloaded 66 ktx2, UASTC→BC7 on the thread pool) can replace
raw RGBA8 later if VRAM matters; raw+zstd keeps this phase a pure
container change with zero quality risk.

DONE 2026-09-26. `scripts/gltf-extract-textures.py` (toktx raw RGBA8 +
KTX_SS_ZSTD, single mip for exact PNG-path parity, semantic filename suffix)
wired into `export-models.sh` after the rotation-f32 step; engine side in
GltfDiligent.cpp (GLTF::TextureCacheType pre-filled from the TextureManager
— Diligent's own KTX loader rejects KTX2, see lessons 2026-09-26). Measured
(ENGINE_LOAD_TIMING, warm): gltfLoad eve 193→8.9 ms, loadWorld total
461→317 ms; RenderDoc: props draws bind the 2048² ktx2 (11 sRGB + 7 UNORM
formats correct), player binds all 3; whole-run perf profile has ZERO
png/inflate samples. Visual A/B at the parked vantage 0.01 % px Δ>30 (=
control floor) — the vantage frames no props, so props correctness was
verified by binding inspection instead. NOTE the pre-phase2 baseline was
broken data: the shipped test2.zstd had 32 1×1 placeholder PNGs embedded
and the real textures as unreferenced files (morning session's unfinished
experiment) — props load was never PNG-bound; it is now GLB-parse bound
(61 MB zstd → 147 MB mesh GLB — phase 4/5 territory). Pak 225→281 MB (raw
RGBA8 ktx2 larger than PNG on disk; BC7 follow-up would shrink ~4×).

## Phase 3 — Shader + PSO caches (−~300–500 ms of boot/first-frame stutter)

All pipelines compile from HLSL at runtime (`device->CreateShader` with
`SHADER_SOURCE_LANGUAGE_HLSL` — splat/rmlui/gui in-tree; DiligentFX ones
inside libDiligentFX.a). Two independent halves:

**3a. Engine SPIR-V blob cache** (kills glslang + spirv-opt for our shaders):
wrap the in-tree compile sites (SplatTerrainDiligent.cpp:1081 shader loader,
RmluiDiligent, GuiDiligent) with a hash (name + full preprocessed source +
defines) → `data/shadercache/<hash>.spv` lookup; on hit pass
`ShaderCreateInfo` byte-code path, on miss compile + store. The splat
sources carry env-tunable `#define`s (POM/band overrides) — the hash MUST
cover the final source string after those inserts.

**3b. `IPipelineStateCache`** (kills driver-side pipeline builds incl.
RADV): `device->CreatePipelineStateCache(PipelineStateCacheCreateInfo)` at
renderer init, load blob from the settings/data dir, attach
`pPSOCache` (PipelineState.h:835) to every in-tree
`CreateGraphicsPipelineState`; write `GetData()` blob back on exit.
Vulkan PSO cache is GPU+driver-bound (PipelineStateCache.h header note) —
store under a device/driver-named file, silently discard mismatches.
DiligentFX-internal PSOs (TAA/SSAO/bloom/shadow/GLTF PBR) can't attach
without patching DiligentFX (prebuilt; we do rebuild it via
cpp-thirdparty/diligent/build.sh) — optional follow-up: thread a global
cache pointer through their PSO create infos.

Validation: second run of the game shows no glslang/spvtools samples in
perf during boot+load; first-frame times even out (PassStatsGui).

## Phase 4 — Parse oghuzlands.zstd once (−~30–50 ms)

`gltfReadModelBytesDiligent` (GltfDiligent.cpp:691) runs twice for the same
GLB: once via `gltfSceneLoadDiligent` (Diligent loader callback,
GltfDiligent.cpp:716-722) and once directly at the top of
`splatTerrainLoadDiligent` (SplatTerrainDiligent.cpp:343) — each pass =
pak read + 5.5 MB zstd decompress + full jansson/tinygltf parse of the
chunk meshes.

Steps: small GLB byte cache in GltfDiligent.cpp keyed by pak path —
`gltfSceneLoadDiligent`'s ReadWholeFileCallback stores the decompressed
bytes, `splatTerrainLoadDiligent` consumes them; free after splat load (or
keep until `gltfDestroy`). Keep the jansson parse (it extracts different
data than the Diligent loader) — only the read+decompress is deduped.

DONE 2026-09-26. `glbByteCache` (map<path, decompressed bytes>) lives in
gltfReadModelBytesDiligent itself — the single read+decompress choke point —
so the Diligent loader callback fills it and splatTerrainLoadDiligent (plus
the runtime surface probe) hit it; cleared on gltfDestroyDiligent. Measured
(ENGINE_LOAD_TIMING, warm): splatTerrainLoad 78.2→62–69 ms (−~13 ms — the
5.5 MB zstd decompress+pak read was cheaper than the 30–50 ms projected),
loadWorld total 333→322–334 ms (props parse noise dominates). Screenshot
control pair at the parked vantage 0.0127 % px Δ>30 (= control floor) —
render path untouched (same bytes to both parsers).

## Phase 5 — Defer/parallelize the sync load (reassess after 1–4)

If phases 1–4 land, projected sync load ≈ 250–300 ms; a frozen menu for
that long may still be acceptable. If not:

- Move the CPU halves (GLB parse, ktx2 tile decode, PNG→ gone after
  phase 2) onto the existing thread pool (15 cores idle today); marshal
  device-object creation + uploads on the main thread between frames.
- props / eve / terrain are independent → parse in parallel.
- Minimal perceived-latency variant: keep the sync load but render a
  "loading" frame before it starts (enterWorld currently runs inside the
  RML click handler, MainMenuGui.cpp:59-87).

DONE (partial) 2026-09-26. The safe half landed: the model pak I/O + zstd
(the pure-CPU half of each Model load) now runs on the thread pool in
parallel (`gltfModelBytesPrewarmLaunch`/`Wait` → `glbByteCache`), and the
one-time texture-cache fill (the 39 ms GPU ktx2 ITexture creates) is done
on the render thread in the gap while the pool runs the zstd, so the two
overlap. The Model loads that follow hit the byte cache + texture cache and
are parse + buffer-create only. Measured (ENGINE_LOAD_TIMING, warm):
loadWorld total 349.7 → 255–264 ms (−~90 ms), first-drawn frame 623.7 →
~578 ms (−~45 ms); prewarm+fill phase ~116 ms (props zstd dominates, the
smaller models' zstd overlap with it), terrain 72→6 ms, props 185→70 ms.

The big remaining half (parallelizing the tinygltf _parse_ — ~63 ms for
props) needs a two-stage Diligent AssetLoader split (CPU parse on a worker
thread, `FinishGPUResources` for the buffer creates on the render thread).
That patch was built and reverted: it introduced a subtle heap corruption
(latent out-of-bounds in the defer load/finish, manifesting on a later
malloc in `animSourceSetup`) that ASan could not isolate (the ASan build
crashes in radv Vulkan init). The Diligent AssetLoader is back to stock;
re-attempting the parse parallelization is the follow-up, ideally with a
working ASan/heap-checker path for the Diligent loader.

## Budget after phases 1–4

    loadWorld   ~1000 ms → ~250 ms   (weights ~300→30, props ~170→~40,
                                      eve ~190→~60, GLB dedupe −40)
    boot        ~250 ms → ~100 ms    (3a covers boot-time compiles)
    first-frame stutter ~0           (3b)
    VRAM        −1.0 GB device pages, −1.0 GB host-visible pages

## Measurement harness (do first)

Permanent ms-level phase timing so progress is verifiable without gdb:
`ENGINE_LOAD_TIMING=1` gates `utils::info` lines with `utils::nanos()`
deltas around gltfSceneLoad / splatTerrainLoad (incl. each
createWeightArray) / gltfPropsLoad / gltfLoad / gltfLoadAnimations, plus
ENTER WORLD → first drawn frame. Baseline table above was measured with
throwaway gdb scripts; the env-gated lines replace them.
