# notes

## Prior session (aborted 2026-09-10, same task)
- C++ side already landed and stays: SplatTerrainDiligent.cpp loads
  images/terrain/{snow_default,sand_default,cliff_side_default}/{albedo,normal}.ktx2
  into SplatDetail band[3] (~L750-810) and binds g_Snow/Sand/CliffAlbedo/Normal in the
  splat SRB (~L1349-1361); the resource signature already lists all six (~L1282-1293).
- The PS (c-game/data/pak_1/materials/splat_terrain_ps.hlsl) has NONE of the band logic
  — that is the actual remaining work.

## Environment / conventions
- PS is HLSL runtime-compiled via glslang; `mix(float3, float3, float)` does NOT resolve —
  write blends as `a + (b - a) * t`.
- Detail sampling MUST stay explicit SampleGrad (ddx/ddy) at the world-tiled tiledUV —
  implicit LOD returns the last mip at those uv magnitudes (documented in PS header).
- Shader sources in c-game/data/pak_1/materials are repacked by ./scripts/build.sh into
  build/c-game/data/pak_1.pak — editing the .hlsl alone does nothing until repack.
- Old-engine parity reference: /home/enes/Projects/c/game-001-cpp/c-engine/data/pak_0_engine/
  shaders/pass/heightmap_terrain/heightmap_terrain.frag — sand near sea level (0 m,
  landMask = smoothstep(0.0, 0.2, worldY)), cliff = smoothstep(0.1, 0.4, slope) with
  slope = 1 - max(N.y, 0), snow by altitude/temperature applied LAST over cliff.
- Screenshot env: ENGINE_SCREENSHOT=path plus ENGINE_SCREENSHOT_FRAME=N (frame pick).
  Terrain draws in the 'terrain' debug group; scripts/rdc.py can dump it via RenderDoc
  if a plain screenshot frame doesn't show the terrain.

## brainstorm
### Core difficulty
The C++ side is verifiably complete (load L753–812, signature L1282–1293, bind L1349–1361 in SplatTerrainDiligent.cpp), so the task is a PS-only change — the real difficulty is picking band thresholds that fit a world whose sea level / height scale is not known from code alone (the old engine's sand band was parameterized by `climateParams.z` and its snow by climate temperature, neither of which exists in the new engine), plus a verification path that actually puts terrain in view.

### Reductions / key lemmas
1. No new vertex data is needed: `worldY = In.AnchoredPos.y + g_Anchor.y` (the anchor is the f32-rounded camera eye, SplatTerrainDiligent.cpp L836/L963, so AnchoredPos = world − anchor) and `slope = 1.0 - max(In.WorldNormal.y, 0.0)` on the UNperturbed geometry normal give both band drivers from existing inputs.
2. Old-engine parity (heightmap_terrain.frag) reduces to three scalar weights blended in the order sand → cliff → snow:
   - sand: `beachMask = smoothstep(-1.5, 0.2, worldY)` × `(1 - smoothstep(k*beachH, beachH, worldY))` — with no `beachH` param, fix the band top (default ~4 m);
   - cliff: `smoothstep(0.1, 0.4, slope)` (old engine ORs in a highland `smoothstep(0.55, 0.85, worldY/maxLandY)*landMask` rock band — optional, skip for v1);
   - snow: old engine drives it by isotherm/biome; without climate data the fallback is a fixed-altitude band `smoothstep(SPLAT_SNOW_LO, SPLAT_SNOW_HI, worldY) * landMask`.
3. Blend mechanics are forced by known quirks: `a + (t - a) * w` only (no `mix(f3,f3,f)`), explicit `SampleGrad(g_DetailSampler, tiledUV, du, dv)` on all six band samples (implicit LOD returns the 1×1 last mip at these uv magnitudes), band normals are tangent-space maps so they blend into `nT` BEFORE the TBN→world transform, and the sRGB albedo samples decode on sample like the other albedos.
4. Blend-in point: after the base-material `influence` blend, before `N0/T/B/N` construction — so the SV_Target2 `WorldNormal` output and all PBR terms pick up the band normals automatically, no lighting changes.
5. `g_Anchor.w` is written 0.0 and never read (L963); it is a free cbuffer slot that could carry `maxLandY` (max of chunk `aabbMax[1]`, computable at load) if a relative snow band is wanted — no cbuffer layout change, the HLSL mirror already has `float4 g_Anchor`.
6. The plan's "#ifndef defines tunable without repak" is only true if `createSplatHlsl` (L1000–1027) is extended to prepend `#define`s from env vars, exactly like the existing `ENGINE_SPLAT_DETAIL_METERS` block — the .hlsl lives inside pak_1.pak, so a bare `#define` edit still requires `./scripts/build.sh` repack.

### Candidate approaches
A. Fixed-metre `#ifndef`-guarded `#define`s in the PS (`SPLAT_SAND_LO=-1.5`, `SPLAT_SAND_HI≈4.0`, `SPLAT_CLIFF_LO=0.1`, `SPLAT_CLIFF_HI=0.4`, `SPLAT_SNOW_LO/HI` in metres), three weights, blend albedo + nT in old-engine order. Main risk: wrong threshold scale if world y is not metres/sea-level-0 (the 6.84 m/repeat tiling comment suggests metres — verify on the first screenshot). Effort: small (one PS edit + repack).
B. Relative snow via `g_Anchor.w` = `maxLandY` from chunk AABBs (2-line C++ change in load + per-frame staging, shader unchanged layout). More robust across world scales; risk: more moving parts and the snow band becomes relative to the single highest peak (a flat world gets no snow, matching old-engine `maxLandY > 1.0` gating). Effort: small-medium.
C. Full old-engine parity (climate/biome maps, wet-sand strip, triplanar cliff sampling, value-noise snowline breakup). Out of scope — the new engine carries no climate data, and triplanar is 3× the samples for little gain on xz-facing walls.
D. Extend `createSplatHlsl` with env-prepended `#define` overrides for the five thresholds (mirror of the `ENGINE_SPLAT_DETAIL_METERS` mechanism). Main risk: none meaningful (additive, VS ignores macros as it already does). Effort: trivial; pairs with A (or B).

### Recommended approach
A + D: fixed-metre `#ifndef` defaults in the PS with the C++ env-prepend extension, so thresholds are tunable without a repak. It keeps the change to one shader file plus ~10 lines in `createSplatHlsl`, matches old-engine band semantics where the data exists (sand near 0 m, slope 0.1–0.4 cliff, snow last over cliff), and D's log lines make tuning observable. It works if world y is in metres with sea level ≈ 0 (the old engine's assumption and the tiling-scale evidence); if the first screenshot shows bands landing at wrong altitudes, either adjust the env values or fall back to B's `maxLandY`-relative snow — the blend code is identical either way.

### Proposed tasks
1. Extend `createSplatHlsl` (SplatTerrainDiligent.cpp L1000–1027) to prepend `#define SPLAT_SAND_LO/-HI`, `SPLAT_CLIFF_LO/HI`, `SPLAT_SNOW_LO/HI` when `ENGINE_SPLAT_*` env vars are set, with a `utils::info` line like the existing metres override. Verify: compiles, run logs the overrides, no behavior change when unset.
2. Edit `c-game/data/pak_1/materials/splat_terrain_ps.hlsl`: declare the six `g_Snow/Sand/Cliff{Albedo,Normal}` Texture2Ds, add the five `#ifndef`-guarded threshold `#define`s, and after the base-material blend compute `worldY`, `slope`, and the sand/cliff/snow weights (old-engine order) blended into `albedo` and `nT` via `SampleGrad(tiledUV, du, dv)` — before the TBN transform, no new comments. Verify: shader compiles (compile error surfaces at pass init).
3. Repack (`./scripts/build.sh`) and run the verification command from the plan (`ENGINE_SCREENSHOT=/tmp/splat_bands.jpg ENGINE_SCREENSHOT_FRAME=300 …`). Verify: log is clean and the screenshot shows sand low / cliff on slopes / snow high; if frame 300 shows no terrain, pick another frame or dump the `terrain` pass via RenderDoc (`scripts/rdc.py`).
4. (Contingent, only if step 3 shows the snow band at the wrong altitude) switch the snow weight to the `maxLandY`-relative form: compute max chunk `aabbMax[1]` at load, store on `SplatTerrain`, stage it into `g_Anchor.w`, and replace the fixed `SPLAT_SNOW_*` smoothstep with `smoothstep(0.55, 0.85, worldY / max(g_Anchor.w, 1.0))`.

## round 1
- Task 1 audit: C++ band support is complete and internally consistent — no fixes needed.
  - Load (SplatTerrainDiligent.cpp L752-795): bandDirs = {"snow_default","sand_default","cliff_side_default"};
    images/terrain/<dir>/{albedo,normal}.ktx2 (albedo sRGB=true, normal linear), failure paths release,
    committed to t->band[b] at L801-803. All six ktx2 files exist in c-engine/data/pak_0_engine/images/terrain/
    (engine pak repacked by scripts/build.sh via data.sh in c-engine/).
  - .h: SplatDetail band[3] (L89); members name/albedo/normal/albedoView/normalView; released on destroy (L1617-1622).
  - Resource signature (L1282-1293): exactly g_SnowAlbedo/g_SnowNormal/g_SandAlbedo/g_SandNormal/g_CliffAlbedo/g_CliffNormal,
    all PIXEL static TextureSRV. SRB bind (L1349-1359) uses the same six names, index-aligned with bandDirs
    (band[0]=snow, [1]=sand, [2]=cliff); missing-view guard fails the pass with a warn.
  - Consequence for tasks 2/3: the PS may declare all six names freely — Diligent's setView/GetStaticVariableByName
    is a no-op for shader-undeclared names, so the pass already works with zero band declarations today.
    PS currently declares none of the six (confirmed by grep).

## round 3 (manager curation)
- Tasks 2 and 3 marked done per the round-2 worker notes (PS band logic + env-prepended threshold defines landed; build/repack clean, shader compiles, overrides log).
- Task 4 narrowed: cliff + snow already visually confirmed (SNOW_LO=800/SNOW_HI=1100); only the sand band (0..~4 m) is unverified — needs a low-altitude viewpoint. No new sub-tasks; escape hatch (g_Anchor.w maxLandY-relative snow) stays contingent inside task 4.

## round 2
- Task 2 implemented and runtime-verified.
  - PS (c-game/data/pak_1/materials/splat_terrain_ps.hlsl): six band Texture2D declarations
    (g_Snow/g_Sand/g_Cliff Albedo+Normal); six #ifndef-guarded defaults
    SPLAT_SAND_LO=-1.5 / SPLAT_SAND_HI=4.0, SPLAT_CLIFF_LO=0.1 / SPLAT_CLIFF_HI=0.4,
    SPLAT_SNOW_LO=30.0 / SPLAT_SNOW_HI=60.0 (fixed-metre, brainstorm A); after the base-detail
    influence blend: worldY = In.AnchoredPos.y + g_Anchor.y, slope = 1 - max(In.WorldNormal.y,0);
    wSand = smoothstep(SPLAT_SAND_LO,SPLAT_SAND_HI,worldY) * (1 - smoothstep(0.25*HI,HI,worldY))
    (old-engine beach shape, LO..~1m full sand fading to 0 by HI), wCliff = smoothstep(CLIFF_LO,CLIFF_HI,slope),
    wSnow = smoothstep(SNOW_LO,SNOW_HI,worldY). Blend order sand → cliff → snow into albedo then
    tangent-space nT, all as c + (t - c)*w, all six via SampleGrad(g_DetailSampler, tiledUV, du, dv),
    band normals (2s-1) blended BEFORE the TBN transform (so SV_Target2 WorldNormal picks them up).
    glslang HLSL parses the negative literal in smoothstep(-1.5, ...) fine (compiled OK at runtime).
  - C++ (SplatTerrainDiligent.cpp createSplatHlsl, after the ENGINE_SPLAT_DETAIL_METERS block):
    static 6-entry table ENGINE_SPLAT_{SAND,CLIFF,SNOW}_{LO,HI} → #define SPLAT_* prepended when the
    env var is set and atof is finite (negative allowed, unlike the meters >0 guard), utils::info
    "splatTerrain: band threshold override <ENV> = <v>" per shader (VS+PS each log, like meters).
- Verified: ./scripts/build.sh clean (repacks pak_1.pak); runtime run (see env gotchas below) logs
  NO splatTerrain warnings and reaches "splatTerrain: chunk grid 4x4 derived" (that line only prints
  after both shaders compiled + pass init succeeded); env overrides log and take effect.
- Screenshots (frame 300, camera y≈683 at (180,683,180), player y≈585):
  - defaults → the whole visible mountain is snow (snow band 30-60 m far below viewpoint: the
    weight mechanism works but the defaults don't fit this world).
  - ENGINE_SPLAT_SNOW_LO=800 SPLAT_SNOW_HI=1100 → base grass on the flats, cliff texture clearly on
    the steep right-facing slope, no snow in view. Cliff + snow paths confirmed visually; sand is
    NOT verifiable from this altitude (band is 0..~4 m) — task 4 needs a low-altitude view (e.g. a
    camera near y≈0, possibly via ENGINE_CAMERA) to confirm sand, or trust the shared code path.
- World scale (for task 4 tuning): oguzlands chunks have maxLandY ≈ 1202 m (chunk 10 AABB
  y 425→1202), most visible terrain 400-700 m. Suggested env for a first look:
  SNOW_LO≈800 SNOW_HI≈1100 (≈0.66-0.9 of maxLandY, old-engine rock-altitude parity), CLIFF 0.1/0.4,
  SAND defaults as-is. If fixed metres misbehave, the maxLandY-relative snow fallback (brainstorm B,
  g_Anchor.w is still free) is the escape hatch — blend code unchanged.
- Env gotchas (affect tasks 3/4 verification):
  1. MainMenuGui auto-enter-world is COMMENTED OUT (MainMenuGui.cpp ~L152) — headless world runs
     need ENGINE_AUTOTEST=enter.
  2. ENGINE_LOG_TIMEOUT units are broken in effect: Engine.cpp does
     utils::nanos() + value * MILLION (nanos) → value×1e-6 s. "120" gives a 0.12 s net; use e.g.
     ENGINE_LOG_TIMEOUT=30000000 for a 30 s margin.
  3. World load is synchronous inside ecsInit (before the main loop), so the timeout check only runs
     after ~1 s of loading regardless.
  4. One transient core dump occurred in an early run during "dataManagerRead: models/t…" (before
     first draw, code untouched by this task; not reproduced in 3 subsequent runs of the same
     binary — possibly memory-pressure, watch for it again in task 4).
- Verification run that works:
  ENGINE_AUTOTEST=enter ENGINE_SCREENSHOT=/tmp/splat_bands.jpg ENGINE_SCREENSHOT_FRAME=300
  ENGINE_LOG_TIMEOUT=30000000 (+ ENGINE_SPLAT_* for tuning) with ENGINE_HIDDEN_WINDOW=1,
  VK_ICD_FILENAMES=.../radeon_icd.json, TERM=xterm.

## round 4 (manager curation)
- Task 4 marked done per the round-3 worker findings above (sand verified, snow default baked to 800/1100, clean repack + no-env re-verification). No duplicates to merge; no new sub-tasks — the round-3 notes already close every open item (transient core dump non-reproducing over 5 runs). Task is complete.

## round 3 — task 4
- Sand band VERIFIED. The 0..~4 m band only exists at the outer island rim (all chunks have
  aabbMin y=1 at the boundary; interior is a 350-600 m plateau). A top-down shot
  (ENGINE_CAMERA=topdown, /tmp/top1.jpg) showed the island with a sand-coloured rim tracing
  the whole coastline; a low-altitude shot showed it clearly.
- Low-altitude vantage found empirically via ENGINE_CAMERA_DOLLY (no custom-position
  ENGINE_CAMERA exists — modes are topdown/close/character/default, all near spawn y≈683):
  the dolly moves the eye along a straight line while the scripted view direction stays
  fixed at the default camera's fwd ≈ (-0.667, -0.167, -0.667). Best shot: fly PAST the
  (+x,+z) corner and look back at the island:
  ENGINE_CAMERA_DOLLY="80,-13,80" + ENGINE_SCREENSHOT_FRAME=2960 → eye ≈ (4140, 40, 4140).
  That single frame reads all three bands at once: sand across the whole low coast,
  cliff on the steep mid slope, snow cap on the 800-1200 m peak
  (/tmp/low4.jpg env-tuned, /tmp/low_default.jpg defaults — identical).
- Timing calibration: headless runs at 60 fps (frame 2000 ≈ 33 s; world ready ≈1 s in).
  F ≈ 60.7·(dolly_seconds + 1). Frames where the eye is BELOW the local terrain surface,
  or above the plateau looking past the map edge, produce all-sky empty frames —
  probe shots, don't assume.
- Thresholds final (baked into PS defaults now): SPLAT_SAND_LO=-1.5/HI=4.0 (unchanged —
  reads perfectly), SPLAT_CLIFF_LO=0.1/HI=0.4 (unchanged), SPLAT_SNOW_LO/HI changed
  30/60 → 800/1100 in splat_terrain_ps.hlsl. With the old 30/60 default the shipped world
  (maxLandY≈1202) rendered ENTIRELY white (top1.jpg proves it) — the default had to follow
  the world. Fixed-metre snow behaved fine, so the maxLandY-relative (g_Anchor.w)
  contingent was NOT needed.
- Verification: ./scripts/build.sh repacked pak_1.pak; post-bake runs (no ENGINE_SPLAT_*
  env) log no splatTerrain warnings, reach chunk-grid init, and reproduce the coherent
  low-altitude shot. Plan verification (frame 300, /tmp/splat_bands.jpg) unchanged from
  round 2 (grass/cliff near slope, no regression).
- The transient dataManagerRead core dump from round 2 did not reproduce in 5 runs.
