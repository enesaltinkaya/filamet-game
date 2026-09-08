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
| `ENGINE_SCREENSHOT_FRAME`           | int                                                     | Which frame to capture (default 3; 0 means 1).                                                                                                    |
| `ENGINE_LOG_TIMEOUT`                | ms                                                      | Auto-stop the engine after N ms of wall time — the safety net for automated runs.                                                                 |
| `ENGINE_AUTOTEST`                   | `enter` \| `pause` \| `settings` \| `credits` \| `exit` | Main-menu scripted action, fired once. `pause` enters the world and opens the pause menu; `exit` quits.                                           |
| `ENGINE_PAUSE_AUTOTEST`             | `back` \| `settings` \| `mainmenu`                      | Paired with `ENGINE_AUTOTEST=pause` — scripted pause-menu action, fired once.                                                                     |
| `ENGINE_SETTINGS_AUTOTEST`          | `close` \| `audio` \| `video` \| `graphics`             | Open the matching settings page (or close it) in the settings GUI.                                                                                |
| `ENGINE_AUDIO_SETTINGS_AUTOTEST`    | `close` \| `effects50`                                  | `close` exercises the BACK path; `effects50` applies a slider change (bind → debounce → settings.json).                                           |
| `ENGINE_VIDEO_SETTINGS_AUTOTEST`    | `close` \| `uiscale15` \| `fullscreen`                  | Same pattern; `uiscale15` verifies slider → apply → persist, `fullscreen` verifies the window toggle.                                             |
| `ENGINE_GRAPHICS_SETTINGS_AUTOTEST` | `close` \| `wire`                                       | `wire` flips 5 toggles + sets 6 sliders, lets `update()` apply them to `data/settings.json`, then closes — the settings wiring test.              |
| `ENGINE_FAKE_ESC_FRAMES`            | comma-sep frames                                        | Inject a synthetic ESC keypress at each listed frame number (up to 16).                                                                           |
| `ENGINE_FAKE_DRAG`                  | any value                                               | Hold RMB + sweep yaw every frame — exercises the interactive orbit-drag path headlessly.                                                          |

## RenderDoc

Debug builds only (the whole block is `#ifndef NDEBUG`). The app-side API
talks to a preloaded `librenderdoc.so` (see `scripts/run.sh renderdoc`, which
also exports `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` and pins the radeon ICD).

| Var                               | Default                                               | Effect                                                                                      |
| --------------------------------- | ----------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| `ENGINE_RENDERDOC_CAPTURE`        | —                                                     | Arm the app-side capture.                                                                   |
| `ENGINE_RENDERDOC_CAPTURE_FRAMES` | 30                                                    | Frame at which the capture is triggered.                                                    |
| `ENGINE_RENDERDOC_DIR`            | `/tmp/RenderDoc/c-game`                               | Capture filename prefix; files land at `<dir>_frameN.rdc`.                                  |
| `ENGINE_RENDERDOC_LIB`            | `/home/enes/Apps/renderdoc/build/lib/librenderdoc.so` | `librenderdoc.so` path for the `dlopen` (an already-loaded `LD_PRELOAD` copy is preferred). |

## Camera / player

| Var                   | Value                                                  | Effect                                                                                                  |
| --------------------- | ------------------------------------------------------ | ------------------------------------------------------------------------------------------------------- |
| `ENGINE_CAMERA`       | `topdown` \| `close` \| `land` \| `landtop` \| `props` | Pick a fixed validation vantage after world load (instead of the default framing).                      |
| `ENGINE_CAMERA_DOLLY` | `x,y,z`                                                | Constant camera velocity in m/s (also marks the run as automated → player parked).                      |
| `ENGINE_TELEPORT`     | `x,y,z`                                                | Override the spawn position (world metres, f32).                                                        |
| `ENGINE_AUTO_RUN`     | truthy                                                 | Auto-run forward from spawn; the third-person camera follows (the camera-follow test). Any W/S cancels. |
| `ENGINE_TPOSE`        | truthy                                                 | Always play the character T-pose (inspect hook).                                                        |
| `ENGINE_NO_ANIM`      | any value                                              | Skip loading/playing the character animation clips.                                                     |
| `ENGINE_NO_PLAYER`    | any value                                              | Keep the player parked (same gate as screenshot/dolly runs).                                            |
| `ENGINE_FOG_DENSITY`  | float                                                  | Override the exponential fog density (default 0.00035).                                                 |

## Terrain / Azgaar world

| Var                              | Value             | Effect                                                                                                                                |
| -------------------------------- | ----------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| `ENGINE_HEIGHTMAP_TEST`          | any value         | Run the heightmap streaming CPU self-test at load.                                                                                    |
| `ENGINE_TERRAIN_DEBUG`           | `ramp` \| `biome` | Validation view: periodic hue per 256 m of height, or the raw biome-colour texture.                                                   |
| `ENGINE_HITCH_DEBUG`             | any value         | Log a `HITCH:` line per frame while Jolt heightfield bodies are created.                                                              |
| `ENGINE_AZGAAR_HM_SIGMA`         | float (texels)    | Override the height-grid Gaussian-blur σ (default 0.35 × sample spacing; clamped to [1, 6]).                                          |
| `ENGINE_AZGAAR_CLIMATE_SIGMA`    | float (texels)    | Same override for the temperature / precipitation / coast grids.                                                                      |
| `ENGINE_AZGAAR_TINT_SIGMA`       | float (texels)    | Same override for the biome-colour blend.                                                                                             |
| `ENGINE_AZGAAR_SNOW_LO`          | float °C          | Snow-blend start temperature (default −1).                                                                                            |
| `ENGINE_AZGAAR_SNOW_HI`          | float °C          | Snow-blend end temperature (default 3).                                                                                               |
| `ENGINE_AZGAAR_BEACH_H`          | float m           | Beach-blend height (default 2.5).                                                                                                     |
| `ENGINE_AZGAAR_CLIMATE_DISABLED` | any value         | Zero all the above thresholds and disable climate in the terrain look.                                                                |
| `ENGINE_AZGAAR_DIAG`             | any value         | Dump the biome-id histogram + table at world build.                                                                                   |
| `ENGINE_AZGAAR_DUMP_TEXTURES`    | dir               | Dump the packed per-world textures (exactly what the terrain pass uploads) as `biome_color.ppm` / `climate_temp.ppm` (R = temp + 64). |

## Props (Azgaar scatter + render pass)

| Var                             | Value     | Effect                                                                                                           |
| ------------------------------- | --------- | ---------------------------------------------------------------------------------------------------------------- |
| `ENGINE_AZGAAR_PROPS_DISABLED`  | any value | Disable the whole Azgaar props system.                                                                           |
| `ENGINE_AZGAAR_PROPS_DEBUG`     | any value | One-shot scatter/build debug line.                                                                               |
| `ENGINE_AZGAAR_PROPS_MESH_DUMP` | any value | Dump each built prop mesh as `/tmp/azgaar_props_<species>.obj` (grass cards: `/tmp/azgaar_props_grass_<n>.obj`). |
| `ENGINE_AZGAAR_SETTLE_DISABLED` | any value | Disable the settlement/plateau system.                                                                           |
| `ENGINE_NO_PROPS`               | `"1"`     | Disable the props render pass (scatter may still run).                                                           |
| `ENGINE_PROPS_DEBUG`            | any value | One-shot props-pass debug line (tiles, instancing, culling).                                                     |
| `ENGINE_PROPS_PERF`             | any value | Periodic props perf line (game/pass ms, instances, draws, memory).                                               |
| `ENGINE_PROPS_PLAYER_PUSH`      | float     | Enable/disable props being pushed by the player; **enabled by default**, `≤ 0` turns it off.                     |

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
