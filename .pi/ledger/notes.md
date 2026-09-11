# notes

## brainstorm (condensed)

- Pipeline = two halves that must agree: depth written by casters (player + splat terrain
  pass) vs attenuation read by receivers (per-pixel cascade pick in `splat_terrain_ps.hlsl`;
  CPU single-cascade pick in PBR/player path). Failure in either half looks identical.
- "All types" = shadowMode 1..4, each with different atlas/SRV: PCF binds GetSRV() + compare
  sampler; VSM/EVSM bind GetFilterableSRV() + linear sampler (per mode in splatBindDynamicResources).
- `fFixedDepthBias` normalized by cascade-z scale, PCF-only; VSM/EVSM bias from variance floor.
- Caster must run in ALL modes ≥1 (`splatShadowsOn()`): ConvertToFilterable derives VSM/EVSM
  moments from raw D32 — the old SplatTerrainDiligent.h comment claiming PCF-only was wrong (fixed).
- Confounder: PBR lit pass clips the character within ~20 m of camera (unresolved, unrelated);
  validation vantages must stay >30 m out.
- Instruments: ENGINE_SHADOW_READBACK (depth bbox), ENGINE_SPLAT_SHADOW_DEBUG, ENGINE_SCREENSHOT
  (has per-pixel gain — cannot decode absolute cbuffer values), ENGINE_CAMERA/ENGINE_TELEPORT,
  ENGINE_SHADOW_FADE=0, RenderDoc `shadow` pass group.

## round 1 — task 6 CPU oracle probe (done, PASS)

- `ENGINE_SHADOW_ORACLE=frameN` probe in ShadowDiligent.cpp updateFrameImpl() after
  DistributeCascades, BEFORE the PBR block's `ppos[1] += 1.0` (feet must stay unshifted).
  Runs {feet, head, nearest chunk-AABB corner, 40–60 m view-axis point} per cascade through
  raw `GetCascadeTransform(c).WorldToLightProjSpace` (col-vector, /w) vs the splat-PS receiver
  reconstruction (row-vector × transposed-stored `mWorldToLightView` + f4LightSpaceScale/ScaledBias,
  no /w). Criteria |Δuv|<2 tex, |Δz|<1e-4, in-box points only (out-of-box f32 rounding legitimately
  diverges; PS skips those pixels).
- **Result: PASS** — in-box points agree ≤6e-8 z on both cascades. Regression is NOT a
  caster↔receiver matrix-convention mismatch.
- Headless gotcha: without `ENGINE_AUTOTEST=enter` the game parks in the main menu, hasPlayer
  false, probes never fire. Working recipe:
  `ENGINE_HIDDEN_WINDOW=1 ENGINE_AUTOTEST=enter ENGINE_SHADOW_MODE=1 ENGINE_SHADOW_ORACLE=300
  ENGINE_LOG_TIMEOUT=120000` (timeout ms).
- "0/16 chunks drawn" at default vantage = corrupt saved player position, not a streaming bug.

## round 2 — task 7 symptom matrix (done)

- Vantage recipe: `ENGINE_HIDDEN_WINDOW=1 ENGINE_AUTOTEST=enter ENGINE_CAMERA=cast
  ENGINE_TELEPORT=-500,513,164 ENGINE_SHADOW_FADE=0 ENGINE_SHADOW_READBACK=200
  ENGINE_SCREENSHOT=... ENGINE_SCREENSHOT_FRAME=450`. `ENGINE_CAMERA=cast` = eye spawn+(−36,+4.3,−36),
  ~51.4 m from player (clip-safe), follows player. TELEPORT mandatory: saved player DB corrupt
  (180,−916776,180). Artifacts /tmp/shadowmatrix/.
- Debug ladder (task said 10/11 — wrong): `ENGINE_SPLAT_SHADOW_DEBUG` = sTail.w:
  1=cascade colors, **2=final attenuation**, **3=raw comparison** (red=shadowed, green=lit,
  magenta=out-of-box/not-PCF), 4+=frac-UV.
- Readback in modes 2-4 measures raw D32 (filterable atlas is a separate texture).
- | mode | terrain depth | player shadow on terrain | failing cell |
  | 0 off | n/a | none (baseline) | — |
  | 1 PCF | ✓ both cascades | ✓ | none |
  | 2 VSM | ✓ both cascades | ✓ faint | none |
  | 3 EVSM2 | ✓ both cascades | (player inside band) | **far cascade ≡ 0 full-shadow band** |
  | 4 EVSM4 | ✓ both cascades | (player inside band) | **same as 3** |
- Diagnosis: regression is EVSM-specific. Raw D32 caster fine in all modes; PCF/VSM receive works.

## round 3 — task 9 root cause + fix (done)

- Root cause: cbuffer int/float type mismatch in the splat PS's hand-written mirror of
  `Diligent::HLSL::ShadowMapAttribs`. Mirror declared the tail as `float4 sTail`, but the C++
  tail is `BOOL bIs32BitEVSM; int iFixedFilterSize; float fFilterWorldSize; BOOL fDummy`.
  CPU writes `bIs32BitEVSM=1` as int (0x00000001); read as float = denormal 1.4e-45 ≈ 0 →
  `warpDepthEVSM` took `maxExp = (sTail.x > 0.5) ? 42.0 : 5.54` → 5.54 branch → warp has no
  32-bit dynamic range → Chebyshev one-tailed bound var/(var+d²) collapses to 0 in the far
  cascade (full shadow). PCF/VSM never read the flag — immune.
- Exponent convention (40/5, clamp 42) and slice indexing were correct in converter and
  receiver; atlas moments were always correct (CPU oracle: converter MATCH, p=1.0 far cascade).
- Proof was gain-robust: in-GPU threshold test (`sTail.x>0.5` FALSE everywhere vs posExp/fVSMBias
  TRUE), CPU ring-slot dump (new `ENGINE_SPLAT_SLOT_DUMP`: slot[644]=1.40129846e-45 = int 1 as
  float), and forcing maxExp=42 lit the band. Screenshot pipeline applies per-pixel gain
  (flat 0.5 → 113–165) — absolute cbuffer values cannot be decoded from screenshots.
- Fix: `c-game/data/pak_1/materials/splat_terrain_ps.hlsl` — typed tail decl
  `int bIs32BitEVSM; int iFixedFilterSize; float fFilterWorldSize; float fShadowDebugMode;` +
  `maxExp = (bIs32BitEVSM > 0) ? 42.0 : 5.54;`. Byte layout unchanged (16B), VS float4 mirror
  (unused) still consistent. Also corrected stale SplatTerrainDiligent.h comment; left
  `ENGINE_SPLAT_SLOT_DUMP`/`ENGINE_SPLAT_CB_DUMP` gated diagnostics in SplatTerrainDiligent.cpp.
- Files: splat_terrain_ps.hlsl, SplatTerrainDiligent.h, SplatTerrainDiligent.cpp.

## round 4 — task 8 acceptance (done, PASS)

- Full mode matrix re-run (round-2 recipe, artifacts /tmp/shadowmatrix_acc/):
  modes 3/4 far-cascade band gone (10.4% pixels changed = removed band only; now lit with
  detail, attenuation identical to VSM); modes 0/1/2 pixel-stable vs round 2 (≤0.05%, TAA floor);
  player foot-shadow blob attached in all modes (same placement across PCF/VSM/EVSM);
  no acne, no peter-pan; readback identical across modes 1-4 (2-3/16 caster chunks, atlas min z
  ~0.0012). Residual (pre-existing, non-shadow): far PBR mountains dark in debug=2 frames.
- Reference cross-check (task requirement) — no residual deviations:
  - `GetEVSMExponents` (Shadows.fxh:280-285: `Is32BitFormat ? 42.0 : 5.54`, min-clamp) and
    `WarpDepthEVSM` (`d=2z−1; (e^{e₊d}, −e^{−e₋d})`) are byte-equivalent to post-fix splat PS.
  - 40/5 = DiligentFX DEFAULT_VALUEs (BasicStructures.fxh:60-61) = sample slider max.
  - `bIs32BitEVSM` semantics match (DiligentFX derives from RGBA32/RG32 filterable format,
    ShadowMapManager.cpp:157; we set Is32BitFilterableFmt=true + flag=1).
  - Chebyshev/minVar/bleeding formulas identical; converter (ShadowConversions.fx:48-57) uses
    the same shared warp functions → atlas↔receiver agree by construction.
  - Radient: standalone wolf4oid/Radient 404s — it is the former name of DiligentFX; the PBR
    sample in the vendored tree (DiligentFX/Radient/) is C++-only using the same
    ShadowMapManager/Shadows.fxh code. No separate implementation to deviate from.
  - Intentional non-warp differences (no fix needed): splat receiver uses 8-tap Poisson
    post-filter vs DiligentFX SampleGrad+pre-blur; splat PS clamps lightDepth≥0 before warping.
- Acceptance verdict: **PASS** — all 4 shadow modes cast+receive correctly; regression resolved.
