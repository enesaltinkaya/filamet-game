# Environment variables

Every env var the engine (or `scripts/run.sh`) reads, with its effect.
Conventions: **"any value"** = set to anything non-empty; **"1"** = truthy
flag (`atoi`/`!= nullptr`); floats are parsed with `atof`.
All `ENGINE_*` vars are read at the point marked; most are latched on
first use (`static`), so set them before launch, not mid-run.

## Automated / headless runs

| Var                                 | Value                                                   | Effect                                                                                                                                            |
| ----------------------------------- | ------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ENGINE_SCREENSHOT`                 | path                                                    | Capture one frame as JPEG (quality 90) and quit. Also runs the window hidden (headless) and parks the player (the scripted camera owns the view). |
| `ENGINE_SCREENSHOT_FRAME`           | int                                                     | Which frame to capture (default 100 when `ENGINE_SCREENSHOT` is set; 0 means 1).                                                                  |
| `ENGINE_LOG_TIMEOUT`                | ms                                                      | Auto-stop the engine after N ms of wall time — the safety net for automated runs.                                                                 |
| `ENGINE_AUTOTEST`                   | `enter` \| `pause` \| `settings` \| `credits` \| `exit` | Main-menu scripted action, fired once. `pause` enters the world and opens the pause menu; `exit` quits.                                           |
| `ENGINE_PAUSE_AUTOTEST`             | `back` \| `settings` \| `mainmenu`                      | Paired with `ENGINE_AUTOTEST=pause` — scripted pause-menu action, fired once.                                                                     |
| `ENGINE_SETTINGS_AUTOTEST`          | `close` \| `audio` \| `video` \| `graphics`             | Open the matching settings page (or close it) in the settings GUI.                                                                                |
| `ENGINE_AUDIO_SETTINGS_AUTOTEST`    | `close` \| `effects50`                                  | `close` exercises the BACK path; `effects50` applies a slider change (bind → debounce → settings.json).                                           |
| `ENGINE_VIDEO_SETTINGS_AUTOTEST`    | `close` \| `uiscale15` \| `fullscreen`                  | Same pattern; `uiscale15` verifies slider → apply → persist, `fullscreen` verifies the window toggle.                                             |
| `ENGINE_GRAPHICS_SETTINGS_AUTOTEST` | `close` \| `wire`                                       | `wire` flips 5 toggles + sets 6 sliders, lets `update()` apply them to `data/settings.json`, then closes — the settings wiring test.              |
| `ENGINE_FAKE_ESC_FRAMES`            | comma-sep frames                                        | Inject a synthetic ESC keypress at each listed frame number (up to 16).                                                                           |
| `ENGINE_FAKE_DRAG`                  | any value                                               | Hold RMB + sweep yaw every frame — exercises the interactive orbit-drag path headlessly.                                                          |
| `ENGINE_STAGE_DUMP`                 | dir                                                     | Per-frame burst dump (frames `ENGINE_SCREENSHOT_FRAME`.., `ENGINE_SCREENSHOT_BURST` files, `..._BURST_STRIDE` spacing): `N_ssao.png` (AO map), `N_depth.png` (inverse-depth, 3 m/R8), `N_moved.txt` (camera-motion flags + dEye/dRot + camera basis), `N_world.png` + `N_accum.png` (TAA chain). |
| `ENGINE_WINDOW`                     | `WxH` (≥320x240)                                       | Force the window/client size (e.g. `640x360`). A full-size capture costs ~1.2 s of GPU readback + PNG per frame, and the fixed-step loop advances the sim by the 250 ms-capped frameTime per frame — a small window drops that to ~110 ms/frame, so bursts sample the camera dolly ~10x finer. |

## RenderDoc

Debug builds only (the whole block is `#ifndef NDEBUG`). The app-side API
talks to a preloaded `librenderdoc.so` (see `scripts/run.sh renderdoc`, which
also exports `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` and pins the radeon ICD).

| Var                               | Default                                               | Effect                                                                                      |
| --------------------------------- | ----------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| `ENGINE_RENDERDOC_CAPTURE`        | —                                                     | Arm the app-side capture.                                                                   |
| `ENGINE_RENDERDOC_CAPTURE_FRAMES` | 30                                                    | Frame at which the capture is triggered.                                                    |
| `ENGINE_RENDERDOC_DIR`            | `/tmp2/RenderDoc/c-game`                              | Capture filename prefix; files land at `<dir>_frameN.rdc`.                                  |
| `ENGINE_RENDERDOC_LIB`            | `/home/enes/Apps/renderdoc/build/lib/librenderdoc.so` | `librenderdoc.so` path for the `dlopen` (an already-loaded `LD_PRELOAD` copy is preferred). |

## Camera / player

| Var                   | Value                               | Effect                                                                                                  |
| --------------------- | ----------------------------------- | ------------------------------------------------------------------------------------------------------- |
| `ENGINE_TELEPORT`     | `x,y,z`                             | Override the spawn position (world metres, f32).                                                        |
| `ENGINE_CAMERA_DOLLY` | truthy                              | From wherever the camera sits when the world comes up, dolly straight backwards (opposite the look direction) 10 m over one leg, then back to the start over one leg, looping. Turnarounds are smooth (cosine, velocity zero at both ends); the orientation never changes. Parks the player (the scripted camera owns the view) — pair with `ENGINE_SCREENSHOT_FRAME` / `ENGINE_DEBUG_CAM` to inspect a frame that has actually moved. Interactively the same dolly is the **`V`** key: press it to arm the dolly at the live camera (it takes the view from player mode; `C` hands the view back, which ends the dolly) and press it again to stop with the camera back at the anchor — no env-var round-trip. |
| `ENGINE_CAMERA_DOLLY_LEG`   | float (s)  | Dolly leg duration (default 1.5; distance stays 10 m → speed = 10/leg s — e.g. 30 = a 0.33 m/s dolly for slow-motion A/B). |
| `ENGINE_CAMERA_DOLLY_FRAME_DT` | float (s/frame) | Advance the dolly by a FIXED dt each frame instead of wall-clock dt (0.0154 reproduces the 65 fps step at the default leg). Wall-clock dt ties the pose lattice to fps, so two runs never share a pose and temporal A/B measures ~3 gray of phase noise; with a fixed dt and `ENGINE_SCREENSHOT_BURST_STRIDE`, runs are pose-identical and TAA/jitter deltas become resolvable. |
| `ENGINE_AUTO_RUN`     | truthy                              | Auto-run forward from spawn; the third-person camera follows (the camera-follow test). Any W/S cancels. |
| `ENGINE_AUTO_JUMP`  | int (fixed ticks)                   | Fire a jump every N ticks (60 = 1 s) — the character-controller jump/air-state test. Needs player mode: pair with `ENGINE_AUTO_RUN=1` in automated runs. Implies the state-db write gate (see `automatedRun()`).                                       |
| `ENGINE_TPOSE`        | truthy                              | Always play the character T-pose (inspect hook).                                                        |
| `ENGINE_NO_ANIM`      | any value                           | Skip loading/playing the character animation clips.                                                     |
| `ENGINE_NO_PLAYER`    | any value                           | Keep the player parked (same gate as screenshot runs).                                            |
| `ENGINE_FOG_DENSITY`  | float                               | Override the exponential fog density (default 0.00035).                                                 |

## GLTF / character

| Var                   | Value     | Effect                                                                                                            |
| --------------------- | --------- | ----------------------------------------------------------------------------------------------------------------- |
| `ENGINE_GLTF_DEBUG`   | any value | Log GLB structure at load + first pose frames (nodes, clips, root row).                                           |
| `ENGINE_JITTER_PROBE` | any value | Log player eye/pos and glTF mesh anchor-space position every 5th frame — the f32-absolute-placement jitter check. |

## Renderer

| Var                  | Value     | Effect                                                                                |
| -------------------- | --------- | ------------------------------------------------------------------------------------- |
| `ENGINE_NO_GPU_TIME` | any value | Disable the per-frame GPU-time query (A/B the timing overhead).                       |
| `ENGINE_DEBUG_CAM`   | any value | Log the camera eye/center/up + view matrix every frame.                               |
| `ENGINE_RML_PROBE`   | any value | Verbose rmlui log: frame commands/bounding boxes, texture table, per-batch draw info. |
| `ENGINE_AO_MOTION_RESET`    | float (px/frame) | Camera-motion threshold for the SSAO history reset (eye translation at near-plane depth + view rotation, converted to screen pixels). Default 0.1; 0 = never reset (keep history while the camera moves — the ghost baseline). |
| `ENGINE_SSAO_RESET_TRACE`   | any value | Log each SSAO accumulation-reset state change (ACTIVE/off) with this frame's dEye + dRot. |
| `ENGINE_SSAO_NO_HISTORY`    | any value | Force the SSAO accumulation reset every frame (single-frame AO — the ghost-free ground truth). |
| `ENGINE_TAA_MOTION_PX`          | float (px/frame) | Jitter-gate cutoff: at this image displacement of a point at `ENGINE_TAA_MOTION_Z` depth the TAA Halton jitter is fully faded out (linear fade starting at 0.25× the value). Default 2. Above it the frame renders unjittered: with no history to dither, the jitter only shimmers contrast edges. `ENGINE_TAA_MOTION_JITTER_OFF=1` disables the gate. |
| `ENGINE_TAA_MOTION_Z`           | float (m)        | Reference depth of the jitter gate's px/frame measure (the near field). Default 12. |
| `ENGINE_TAA_MOTION_VECTOR_DIFF` | float            | DiligentFX `TAA_MOTION_VECTOR_DIFF_FACTOR`: per-pixel-motion scale that kills TAA's history alpha (`alpha *= saturate(1 - |mv_uv| * factor)`). Engine default 16; the library default 256 kills the history near 1 px/frame of flow. Tuning knob: measured across dolly speeds, 1–48 lower the edge variance with no visible ghost change. |
| `ENGINE_JITTER_SCALE`           | float            | TAA sub-pixel jitter amplitude (default 0.25 px; 0 = no jitter). |
| `ENGINE_FX_SHADER_DEFINES`      | `NAME=VAL;...` | Extra HLSL defines appended to every DiligentFX post-FX shader compile — the channel `ENGINE_TAA_MOTION_VECTOR_DIFF` writes through; set it directly to override the engine default. |

## Shadow (cascades)

| Var                        | Value     | Effect                                                                                                                              |
| -------------------------- | --------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| `ENGINE_SHADOW_MODE`       | 0–4       | Override `shadowMode` at startup: 0=off, 1=PCF, 2=VSM, 3=EVSM2 (default), 4=EVSM4.                                                    |
| `ENGINE_SHADOW_FOCUS_BOX`  | `0`       | Disable the player-focused cascade-0 re-fit (A/B: stock slice-sized cascade 0).                                                      |
| `ENGINE_SHADOW_FOCUS_HALF` | float (m) | Override the focus-box half-extent (4–40 m; default auto: caster height / tan(sun elevation) + focus margin, clamped 6–32 m).         |
| `ENGINE_SHADOW_SLOPE_BIAS` | float     | Shadow-caster `SlopeScaledDepthBias` (all casters, all cascades). Default 0.5; 2.0 = the old stock value (wall-base shadows detach and flash while the camera moves). |
| `ENGINE_SHADOW_TRACE`      | frameN    | One-shot trace line: band, focus box, cascade z rows, texel sizes, player light NDC, cascade ring radius (`R`), player screen position (`px`/`py`). |
| `ENGINE_SHADOW_TRACE_STRIDE` | frames   | Trace cadence (1–1000, default 50): `1` = every frame, for per-frame cascade/caster traces.                                        |
| `ENGINE_SHADOW_NO_SNAP`    | any       | Disable cascade texel snapping (manager `SnapCascades` + the anchor-relative centre snap) — cascade centres go bit-constant.       |
| `ENGINE_SHADOW_C1_TEXEL_OFFSET` | float | Shift cascade 1's box centre by N texels (A/B: how much a cascade-1 grid shift is worth on screen).                              |
| `ENGINE_SHADOW_ORACLE`     | frameN    | One-shot caster-vs-receiver matrix consistency check (pass/fail per point/cascade).                                                  |
| `ENGINE_SHADOW_RINGS`      | `0`       | Disable player-anchored cascade rings: cascade k = cube centered on the player with radius `focusHalf·(tierDist/focusHalf)^(k/(N-1))`, z-gate = player view-Z + radius. Default on for all shadow modes; `0` = stock camera-frustum distribution. |
| `ENGINE_SHADOW_FILTER_WORLD` | float (m) | World-space shadow filter footprint, baked default **0.04** (the sun's penumbra at ~4 m of caster height). With it `ShadowMapManager::ConvertToFilterable` derives each cascade's atlas blur radius from its own light-space scale, so every cascade filters the same WORLD area; the splat receiver scales its tap kernel and the Chebyshev variance floor by the same per-cascade factors. `0` = the legacy fixed-texel kernel (cascade 1's penumbra = 13x cascade 0's at the 2048/2-cascade tier = the cascade-boundary seam). Applies to VSM/EVSM modes. |
| `ENGINE_SHADOW_TRANSITION`     | float (0–1) | `ShadowAttribs.fCascadeTransitionRegion` (default 0.1): width of the cascade-to-cascade blend band, as a fraction of the cascade's z-range (and of its box-edge distance — both sides fold into the same `max()`, DiligentFX parity). |
| `ENGINE_SHADOW_BLEND_DITHER`   | float (0–1) | Amplitude of the interleaved-gradient-noise dither added to the cascade-blend factor in the terrain receiver (default 0): breaks the blend band's iso-lines into per-pixel noise for the AA/TAA to average. |
| `ENGINE_SHADOW_UNION`          | 0/1       | Terrain cascade composition: 1 (default) = multiply every covering cascade; 0 = lit-wins (the finest covering cascade is authoritative, the props receiver's policy). |
| `ENGINE_SPLAT_SHADOW_DEBUG`    | mode      | Terrain PS shadow debug output: 1 = cascade coverage (the FINEST covering cascade), 2 = final attenuation, 3 = raw per-cascade attenuation (magenta = uncovered), 4 = EVSM pos/neg moments, 6 = view-z + warped moments + tap UV, 7 = slope/cliff/sand weights, 100+ = material terms (100 roughness, 101 spec term, 102 normal, 103 NdotH). |

## IBL

| Var                         | Value | Effect                                                                                                               |
| --------------------------- | ----- | -------------------------------------------------------------------------------------------------------------------- |
| `ENGINE_IBL_ENV`            | name  | Environment file from `images/studiolights/` (stem or filename); default `kloofendal_48d_partly_cloudy_puresky_1k`.  |
| `ENGINE_IBL_INTENSITY`      | float | Global IBL scale (diffuse + specular, each path's IBLScale); also the DebugGui IBL intensity ±0.25 steps.            |
| `ENGINE_IBL_SPEC_INTENSITY` | float | Specular-only attenuation: scales the prefiltered env the spec lobe samples; diffuse irradiance stays full.          |
| `ENGINE_IBL_SPEC_CLAMP`     | float | Luminance clamp on the specular env copy (default 100, ≤0 disables). Blanks the HDRI's baked sun disk — the analytic |
|                             |       | sun already supplies that energy; unclamped it prefiltered into a sheen spike on sun-facing reflections.             |

## GUI (rmlui)

| Var                | Value     | Effect                                                                     |
| ------------------ | --------- | -------------------------------------------------------------------------- |
| `ENGINE_NO_RMLUI`  | any value | Disable rmlui entirely — no menus shown (ESC is ignored in-world).         |
| `ENGINE_DEBUG_GUI` | any value | Show the debug GUI headlessly (interactive toggle is Ctrl+B).              |
| `ENGINE_STATS_GUI` | any value | Show the stats + pass-stats GUIs headlessly (toggles are Ctrl+D / Ctrl+P). |

## General debug

| Var            | Value | Effect                                                                                                              |
| -------------- | ----- | ------------------------------------------------------------------------------------------------------------------- |
| `ENGINE_DEBUG` | `1`   | `utils::isDebug()`: enables the rmlui debugger UI and debug signal-catcher behaviour. Exported by `scripts/run.sh`. |

## Standard / external

Read, not engine-specific (listed because they change behaviour):

| Var                                             | Effect                                                                                                                                  |
| ----------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| `XDG_CONFIG_HOME` / `HOME`                      | `c-utils/cfgpath` base for the `settings.json` location.                                                                                |
| `XDG_DATA_HOME` / `HOME`                        | Data path (cfgpath).                                                                                                                    |
| `XDG_CACHE_HOME` / `HOME`                       | Cache path (cfgpath).                                                                                                                   |
| `WAYLAND_DISPLAY`                               | If unset (X11), display scale is also stored as `cursorScale`; on Wayland the rmlui cursor scaling is left alone.                       |
| `TERM`                                          | Not read by the engine — `scripts/run.sh` runs `clear` under `set -e` and aborts if it's unset (so no-TTY invocations must set it).     |
| `LD_PRELOAD`, `ENABLE_VULKAN_RENDERDOC_CAPTURE` | RenderDoc layer mode (see above) — the implicit Vulkan layer is required because Diligent's Vulkan backend loads entry points via volk. |
| `VK_ICD_FILENAMES`                              | Set by `scripts/run.sh` to the radeon ICD.                                                                                              |
