# World load time — optimization plan 2 (remainder)

Follow-up to plans/world-load.md (phases 1–5 landed). What's left, measured
2026-09-26 (ENGINE_LOAD_TIMING warm + gdb breakpoint stamps on
`RenderDeviceVkImpl::CreateShader` / `CreateGraphicsPipelineState`, perf is
blocked in this env — mlock permission):

    loadWorld total                  ~280–304 ms
      bytes prewarm + texture fill   ~120 ms   props zstd dominates
      terrain                           6–7 ms
      props                           75–78 ms  tinygltf parse + GPU buffers
      character + anim                   9 ms
      splatTerrainLoad                67–91 ms  2nd (jansson) parse of the
                                                same terrain GLB + weight arrays
    world loaded → first drawn frame ~534–601 ms
      ~240 ms  57 shader compiles + 41 PSO links — the DiligentFX PBR lib,
               UNCACHED (old plan phase 3 covered only in-tree shaders/PSOs)
      ~340 ms  first-frame init: TAA offscreen chain + world RT allocation,
               IBL set for 42 material SRBs, shadow pass, first GPU work

Facts measured this session:

- props zstd: 63.5 MB → 147 MB, `zstd -dc` = 88 ms single core; level -3
  decodes 83 ms for +4 MB (no win). Paks are zip-STORED, so a raw 147 MB
  entry is a ~10–15 ms NVMe read. terrain zstd is only 5.5 MB → 11 MB (the
  old plan's 210 MB figure is stale — the terrain was chunked down).
- Physics (Jolt) first-frame cost is negligible (44 static bodies).
- `shadercache/` dir has no driver slug (the `pso/` dir does); SPIR-V is
  driver-specific, so the blob cache can go stale across drivers.
- DiligentFX patch surface (DiligentFX/PBR/src/PBR_Renderer.cpp): 11×
  `m_Device.CreateShader(ShaderCI)` (550, 559, 799, 809, 861, 870, 2064,
  2102, 2325, 2416, 2425), 4× `m_Device.CreateGraphicsPipelineState(
PSOCreateInfo)` (565, 835, 896, 2208), 1× `m_Device.GetDevice()->
CreateGraphicsPipelineState(PsoCI, ppPSO)` (2435). GLTF_PBR_Renderer.cpp
  and USD_Renderer.cpp create none. Precedent for patching the tree:
  cpp-thirdparty/diligent/build.sh already sed-strips the wayland define
  idempotently after every update.

## Phase A — Cache the PBR lib's shaders + PSOs (−~240 ms first frame) ✦ biggest — DONE

Result (2026-09-26): warm `world loaded → first drawn frame` 534–601 →
294.8 ms. PSO file 37 KB → 118 KB (PBR entries). Screenshot A/B identical
(the only >128 diff pixels are the HUD ms readout). One design note: the
fallback definitions must NOT be inline in the header — an inline body is
inlined into the FX translation unit and the game's strong symbols are
never referenced (silent no-op, first-frame timing gives it away). They
live in fxpbr_cache_fallback.cpp as a separate archive member the linker
pulls only when unresolved (samples); the game links libc-engine.a before
libDiligentFX.a, so its strong definitions win.

Completes old-plan phase 3. The world pass (player + 41 props material
SRBs) runs through DiligentFX `PBR_Renderer`, which compiles RenderPBR
vsh/ps per PSO key (57 compiles) and links 41 pipelines on the first world
draw, bypassing both caches.

Steps:

1. c-engine: two `extern "C"` helpers (new FxCache.cpp or in
   ShaderCache.cpp):
   - `IShader* fxShaderCacheCreate(IRenderDevice*, ShaderCreateInfo&)` →
     the existing `shaderCacheCreate` (it already hashes source+macros+
     type+entry and reads sources via `pShaderSourceStreamFactory`, which
     is how the PBR lib feeds its embedded HLSL).
   - `IPipelineState* fxPipelineStateCreate(IRenderDevice*,
GraphicsPipelineStateCreateInfo&)` → set `ci.pPSOCache = psoCache()`
     then `CreateGraphicsPipelineState`; both helpers AddRef before
     returning (the lib assigns the raw ptr into RefCntAutoPtr).
2. build.sh (idempotent, re-applied after every Diligent update, same
   pattern as the wayland strip):
   - write `DiligentFX/PBR/src/fxpbr_cache.hpp` declaring the two
     prototypes (the FX build has no access to our include paths);
   - sed PBR_Renderer.cpp: insert `#include "fxpbr_cache.hpp"` after the
     first include; replace the three call forms above with the helpers.
3. Driver slug for the blob cache: `cachePath()` in ShaderCache.cpp takes
   the adapter slug from `dev->GetAdapterInfo().Description` (same
   sanitization as psoCachePath) into a subdir. Old flat .spv files miss
   once and get re-stored — acceptable, no migration.
4. The PSO cache already saves on exit (DiligentRenderer.cpp:796); the PBR
   entries join the blob automatically — the pso file will grow from 37 KB.

Validation: 2nd run `world loaded → first drawn frame` drops ~200 ms; log
shows `shaderCache: hit` for PBR shader names and the pso file grew;
screenshot A/B control pair at the parked vantage (render output must be
bit-identical — cache changes no bytes, only their origin).

Risk: third-party patch — re-applied by build.sh, but a DiligentFX rework
of PBR_Renderer.cpp can break the seds; keep them anchored on the exact
call strings and let a failed no-op sed show up as a regression in the
first-frame timing.

## Phase B — Ship props raw in the pak (−~80 ms loadWorld) — DONE

Result (2026-09-26): `bytes prewarm + texture fill` 116–125 → 62.3 ms,
loadWorld total 273 → 216.6 ms. Pak 281 → 360 MB. test2 exports as raw
`models/test2.glb` (convertModel <blend> 1); loader's zstd-magic sniff takes
the raw path untouched. Screenshot A/B identical.

`zstd -10` on props buys 84 MB of disk at the cost of 88 ms of decode on
the load critical path (measured; -3 is no faster). The pak is zip-stored
and the loader already sniffs the zstd magic, so raw bytes pass through
untouched.

Steps:

1. scripts/export-models.sh: skip the zstd pass for the mesh models (env
   flag, default raw; zstd stays as the opt-in for small files like
   eve/anim — their decode is < 5 ms).
2. Rebuild the pak (data.sh); pak grows 281 → ~365 MB.
3. Confirm `gltfReadModelBytesDiligent` takes the non-zstd path (log line)
   and the byte cache fills from the raw read.

Validation: `bytes prewarm + texture fill` 120 → ~30–40 ms; loadWorld
total −~80 ms; screenshot A/B control pair.

## Phase C — Warm the TAA chain + world RTs during the menu (−~50–150 ms first frame) — DONE

Result (2026-09-26): the offscreen chain + post-FX targets (the 64 MB
device-local pages) now allocate at renderer init (menu time); the first
world frame's wall time did NOT drop (290–298 ms, noise) — the allocation
wasn't the dominant first-frame cost. What remains there is ~42 cached-PSO
deserializations + pass inits + the first GPU frame (all 28 PBR shaders are
disk hits). Kept: no mid-entry allocation, and taaDestroy only runs at
renderer teardown, so the chain already survives menu returns (no destroy-
path change needed). Screenshot A/B identical.

Structure: taaFrameBegin's allocation prologue extracted into
`taaEnsureFrameResources` (idempotent per size, no camera/scene inputs);
`taaWarmup()` runs it from DiligentRenderer init after taa/ssao/ssr/bloom
Init. The TAA history stays pristine — PrepareResources alone never touches
it, and the frame-index continuity check resets it on the first real frame.

`taaInit()` runs at renderer init, but the TAA offscreen history chain
(2880×1627 RGBA16F set) and the world RTs (taa color/motion/normal +
depth) are allocated lazily on the first world frame — visible as the 64 MB
device-local pages in the first-frame log. The render thread sits idle
during the menu.

Steps:

1. Read `TemporalAntiAAging::Begin` to confirm where the chain allocates;
   add `taaWarmup()` that runs a Begin (and a dummy resolve if Begin alone
   doesn't allocate) at the current swapchain size, callable from
   DiligentRenderer init after the swapchain exists.
2. Pre-create the world RTs (taaColorRTV/motion/normal/depth) at the same
   point — they are swapchain-sized, world-independent.
3. Menu return: keep the chain/RTs alive (do not free in the gltfDestroy
   path) so re-entry doesn't re-pay; ~150 MB held during the menu is the
   trade (fine on this hardware).

Validation: MemoryManager page-alloc log lines move from the first world
frame to boot; `world loaded → first drawn frame` drops; screenshot A/B
control pair (TAA history starts empty either way — first world frame
after warmup must match a cold start, verify the warmup leaves no residue
in the history textures, e.g. by clearing them after the dummy resolve).

## Phase D — Splat CPU parse on the pool (−~40–60 ms loadWorld)

`splatTerrainLoadDiligent` (SplatTerrainDiligent.cpp:373) runs its own
jansson parse of the terrain GLB — CPU work with no device dependency —
serially after the model loads.

Steps:

1. Split it: `splatTerrainParse` (bytes → glbFindChunks → jansson →
   RawChunks; pure CPU, points into the glbByteCache buffer) vs
   `splatTerrainUpload` (per-chunk GPU buffers + weight arrays, render
   thread).
2. loadWorld: launch the parse job on the thread pool right after the
   bytes prewarm (it overlaps the serial terrain/props/char/anim Model
   loads); run the upload where splatTerrainLoad runs today.
3. The terrain GLB bytes must stay in glbByteCache until the upload (they
   do — the cache lives until gltfDestroy). jansson is thread-safe for
   independent parses.

Validation: loadWorld −~40–60 ms (the GPU weight-array half, ~30 ms,
stays serial); screenshot A/B control pair.

## Phase E — Parallel tinygltf parse (DEFER, ~50 ms)

Old plan phase 5's big remaining half: two-stage AssetLoader split (CPU
parse on a worker, GPU finish on the render thread). Previously attempted,
reverted for a heap corruption that ASan couldn't isolate (ASan build
crashes in radv Vulkan init). Needs a working heap-checker path first; not
worth it while phases A–D are open. Reassess last.

## Budget

    loadWorld   ~300 ms → ~150–170 ms   (B −~80, D −~50)
    first frame ~600 ms → ~300–350 ms   (A −~240, C −~50–150)
    disk        +84 MB pak (B)
    VRAM        +~150 MB held during menu (C)

Order: A (biggest, self-contained), B (trivial), C, D. Each phase is
independent and validated by its own A/B pair; no phase depends on another
landing first.
