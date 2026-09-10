# Splat terrain shadows — cast + receive — implementation plan

Make the Oghuzlands splat terrain a first-class shadow participant: it casts
into the DiligentFX `ShadowMapManager` cascade atlas and it receives with
per-pixel cascade selection (terrain spans the whole shadow distance, so the
player-style single-cascade pick is wrong for everything but a few metres
around the player).

## Current state (verified in-tree)

- **Shadow pass** (`renderer/diligent/ShadowDiligent.cpp`): DiligentFX
  `ShadowMapManager`, 2–3 cascades depending on quality tier, runs in
  `DiligentRenderer::draw` before the world RT setup (DiligentRenderer.cpp:463-467,
  `shadow` debug group). `renderCascadesImpl` loops cascades, binds each
  cascade DSV, and draws the **player only** (`gltfDiligentShadowDraw`,
  ShadowDiligent.cpp:359-363). The header already anticipates the gap: "the
  world's shadow draws return with the splat terrain pass" — they don't yet.
- **Terrain receive is already half-wired but single-cascade**: the splat PS
  samples one cascade (c-game/data/pak_1/materials/splat_terrain_ps.hlsl:414-421)
  using `sWorldToLightProj`/`sSlice` fed from `shadowDiligentPbrWorldToLightProj()`
  / `shadowDiligentPbrSlice()` (SplatTerrainDiligent.cpp:949-953) — the cascade
  covering the **player's** feet+torso, CPU-picked per frame. Far terrain
  samples the wrong cascade. `splatShadowsOn()` (SplatTerrainDiligent.cpp:844-849)
  is PCF-mode-only (shadowMode == 1) and falls back to a dummy 1x1 shadow SRV.
- **No terrain caster** anywhere in the shadow pass.
- **Splat pass structure** (SplatTerrainDiligent.cpp): frame cbuffer is a flat
  mirror of `HLSL::PBRFrameAttribs` + `g_Anchor` (SplatFrameStaging, :831-839),
  uploaded per frame through a dynamic ring buffer (MapBuffer + memcpy, :1510-1516).
  The VS subtracts the f32 anchor (splat_terrain_vs.hlsl:111) and emits
  `AnchoredPos`. Chunks are culled per frame by frustum + 1-chunk camera window
  (:1506-1552) and each `SplatChunk` carries a world-space AABB (:1509-1552).
  Shadow SRVs are re-set on the dynamic SRB slots only when the bound resource
  changes (:1461-1467) — a runtime switch, no pipeline rebuild, today.
- The splat pass does **not** poll `shadowDiligentGeneration` and its PS has
  **no tier-distance fade** (the PBR path fades `fLightAmount`→lit over the
  last 25 % of `shadowDiligentTierDistance()`, `ENGINE_SHADOW_FADE=0` to A/B —
  lessons.md 2026-09 entry: the padded light cube still samples ~3× past the
  tier distance, so a receiver-side fade is mandatory, never shrink the cube).

## Decisions

1. **Caster = dedicated depth-only draw, not the PBR pipeline.** The player
   caster re-uses `GLTF_PBR_Renderer` and needs throwaway cascade-sized color
   RTs because its PSOs are 3-RT. For terrain a purpose-built
   `splat_terrain_shadow_vs.hlsl` (pos → light-space, 50-line shader) + trivial
   PS + 0-color-target PSO is cheaper, needs no dummy RTs, and reads only
   position from the existing VBO (same 48 B `SplatVertex` layout, u16 IBO
   reuse — zero new resources).
2. **Receiver = per-pixel cascade pick, all attribs in a fixed-size cbuffer.**
   Add a separate `cbSplatShadow` cbuffer mirroring
   `Diligent::ShadowMapAttribs` (field-identical to
   `DiligentFX/Shaders/Common/public/BasicStructures.fxh:28` — the same bytes
   `shadowDiligentLightAttribs()` memcpy's, non-C++ branch:
   `f4CascadeCamSpaceZEnd[MAX_CASCADES/4]` as `float4[]`) plus a trailing
   `float4 f4ShadowFade` (x = `shadowDiligentTierDistance()`, 0 = fade off).
   **Fixed size, always present** → no cbuffer-size change, no pipeline
   rebuild, no generation polling: shadow on/off stays the existing runtime
   `splatShadowsOn()` gate + dynamic SRV re-set (real vs dummy SRV).
   Cascade pick: `z = dot(view-space pos, 0,0,1)` (the VS emits view z;
   `cCamView` is rotation-only so it is unjittered — matches the unjittered
   `diligentBaseProj()` the distribution uses, TAA-stable). Pick the cascade
   from `f4CascadeCamSpaceZEnd`, sample with that cascade's
   `f4LightSpaceScale`/`f4LightSpaceScaledBias` (the same math the FX sampling
   helpers do), existing 3×3 PCF unchanged.
3. **Culling for the caster = per-chunk light-frustum test, not the world
   pass' window.** The light cube reaches ~132 m at the lowest tier
   (lessons.md), well beyond the world pass' 1-chunk camera window, so reusing
   that cull would silently drop far casters. Transform each chunk AABB's 8
   corners (anchor-subtracted) by the cascade's untransposed
   `WorldToLightProjSpace`; draw the chunk unless every corner is outside the
   NDC box (|x/w|,|y/w| > 1 or z/w ∉ [0,1], small margin). Cheap, exact enough
   (the cube is the conservative superset of the real frustum).
4. **Scope: PCF mode (shadowMode == 1) first, matching today's
   `splatShadowsOn()`.** VSM/EVSM receive (filterable SRV, different sampling
   math) is follow-up work — the generation-counter/rebuild machinery exists
   in the PBR path for when it lands.

## Tasks

### 1. Caster pass (terrain casts)

- `c-game/data/pak_1/materials/splat_terrain_shadow_vs.hlsl` +
  `splat_terrain_shadow_ps.hlsl` (or inline PS in the C++ loader — the rmlui
  pattern loads from pak; keep both files for symmetry). VS: same 48 B input
  layout, `clip = mul(float4(p - g_Anchor, 1), cLightViewProj)` — matrices
  stored TRANSPOSED (runtime glslang convention, lessons.md 2026-09-05 entry);
  cbuffer is small: `float4x4 cLightViewProj; float4 g_Anchor;`.
- `SplatTerrainDiligent.{h,cpp}`: new
  `splatTerrainShadowDrawDiligent(ctx, w2lRowMajor, cascadeDSV)` — compile +
  create the depth-only PSO on first use (0 color targets, cascade DSV,
  depth-write, rasterizer bias from `shadowDiligentCasterBias()` — same slope/
  constant/clamp the PBR caster uses, so player and terrain bias agree in
  overlap), per-frame cbuffer upload (transpose the raw
  `GetCascadeTransform` matrix for the cbuffer), the light-frustum chunk cull,
  `SetVertexBuffer/SetIndexBuffer` per chunk and draw.
- `ShadowDiligent.cpp` `renderCascadesImpl`: inside the per-cascade loop, after
  `gltfDiligentShadowDraw`, call it under
  `Diligent::ScopedDebugGroup(ctx, "terrain")` (so `scripts/rdc.py dump
  shadow/terrain` works), gated on splat terrain being loaded (expose a
  `splatTerrainLoadedDiligent()` or reuse the non-null
  `splatTerrainDiligent()`).
- **Order/anchor gotcha** (lessons.md): the shadow pass runs BEFORE
  `worldDraw`; the splat pass must read this frame's anchor via
  `diligentWorldAnchor()` at draw time (same as `splatFrameFill`) — do not
  cache the last world frame's anchor, or the caster silhouette detaches by
  dEye during camera drags and TAA holds it for ~0.5 s (the exact
  `poseRebuild()` failure the player caster had).

### 2. Receiver pass (terrain receives, per-cascade)

- `splat_terrain_vs.hlsl`: emit one more interpolant — view-space z
  (`cCamView._31*p.x + _32*p.y + _33*p.z`). `splat_terrain_ps.hlsl`: declare
  `cbSplatShadow` (ShadowMapAttribs mirror + `f4ShadowFade`); replace the
  single-cascade block (PS :414-421) with: view-z → cascade index →
  `ShadowPos = (anchoredPos * scale.xyz + bias.xyzw)` → `LightDepth =
  ShadowPos.z - sa.fFixedDepthBias` (already cascade-z-normalized, lessons.md
  2026-09-04 entry — do NOT use a fixed NDC constant) → same
  `filterShadowPCF3`. `sWorldToLightProj`/`sUV`/`sSlice` in the PBRFrameAttribs
  mirror and `shadowDiligentPbr*()` usage in the splat pass get deleted
  (the PBR glTF path keeps its own single-cascade pick for the player).
- `SplatTerrainDiligent.cpp`: `splatFrameStaging` grows a
  `ShadowMapAttribs` field; per frame memcpy `shadowDiligentLightAttribs()`
  (it already contains the distributed `ShadowAttribs` — ShadowDiligent.cpp
  :171-232) and set `f4ShadowFade.x = shadowDiligentTierDistance()`.
  `splatShadowsOn()` unchanged as the runtime gate.
- Cascade-blend seam: a hard pick can show a 1-cascade step at the boundary.
  Start with the hard pick (the PBR path lives with it for the player); if a
  visible band appears, add the `fCascadeTransitionRegion` blend between the
  two covering cascades (both samples, lerp by transition t) — one extra
  sample, cheap.

### 3. Verification (per task, in order)

- **Caster writes depth**: `ENGINE_SHADOW_READBACK=frameN` (ShadowDiligent.cpp
  :369+) — before: player-only silhouette ("geometry bbox uv" a small blob);
  after: terrain covers a much larger uv region per cascade, cascade 2
  non-empty at distance. `ENGINE_SHADOW_DUMP=/tmp/depth` for the raw maps.
- **Receiver correctness**: RenderDoc capture (docs/renderdoc-capture.md —
  `run.sh renderdoc`, `ENABLE_VULKAN_RENDERDOC_CAPTURE=1`, `TERM`/`VK_ICD_*`
  per AGENTS.md; `scripts/rdc.py list` shows `shadow/terrain`; qrenderdoc for
  the depth atlas): inspect a far-terrain pixel's sampled cascade slice
  matches its view z. Screenshot A/B:
  `ENGINE_SCREENSHOT=/tmp/shot.jpg` with a sun angle raking across the map —
  terrain should now self-shadow (hillsides, depressions) and cast onto
  itself; the player shadow must stay attached (no regression from the
  new caster bias).
- **Seams/cutoffs**: orbit the camera; no cascade band, no hard shadow
  termination past the tier distance (fade does its job), `ENGINE_SHADOW_FADE=0`
  A/B confirms the fade is what terminates it (not the cube).
- **TAA**: drag the camera over terrain — no shadow flicker/ghosting
  (un-jittered distribution + un-jittered view z pick are the invariants).
- `ENGINE_SPLAT_TERRAIN=0` A/B: the PBR fallback path is untouched
  (its shadow code is only touched to remove the splat-side consumers).

## Risks / known landmines

- **Transposed-matrix convention mismatch** is the #1 historical failure here
  (two conventions coexist: PBR FX path = row-major untransposed, runtime
  glslang path = transposed — the caster W2L must be transposed *once* for
  the cbuffer; `GetCascadeTransform(...).WorldToLightProjSpace` is the
  untransposed matrix). Wrong axis sign = shadows sample mirrored, depth
  readback looks "healthy" but everything is off.
- **Bias budget**: caster slope bias (default 2.0 texels) + receiver
  `fFixedDepthBias` both in play; self-shadowing on flat ground or
  peter-panning at grazing angles means one of them is doubled/dropped.
- Depth-only PSO (0 color targets + DSV) is valid in Diligent but confirm the
  PSO desc accepts an empty RTV list on this Vulkan build before writing the
  loader; fall back is the glTF-style dummy cascade-sized RT (GltfDiligent.cpp
  :1484+).
- `ShadowMapAttribs` cbuffer mirror must stay field-identical to the
  non-C++ branch of BasicStructures.fxh (the `float4`-packed
  `f4CascadeCamSpaceZEnd`) — a size drift breaks the memcpy silently.
