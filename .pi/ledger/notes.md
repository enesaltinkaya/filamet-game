# notes

## brainstorm

### Core difficulty

The task isn't the audio API itself — it's porting a stateful subsystem (process-wide SoLoud handle, cached sounds, looping-handle bookkeeping, settings-signal re-tuning) into an engine with slightly different teardown, input, and event idioms, plus linking a second prebuilt static lib into a `--start-group` that already has documented duplicate-symbol landmines.

### Reductions / key lemmas

- The old `SoundSystem` is a single self-contained TU: static `audio` state + free functions, zero ECS components, zero dependencies beyond `dataManagerRead`, `settingsGet/SetDouble`, `signalSubscribe`, `futureTaskAdd`, `nanos`. All of these already exist in the new engine (verified: `settingsSaved` signal emission in `c-utils/settings/Settings.cpp`, `effects`/`music` setting templates with defaults 80/40). The port is nearly mechanical; no new c-utils infrastructure is needed.
- The only true API-shape mismatch: the new engine's `Input` uses SDL scancodes (`input.pressed` is an SDL scancode, `input.ctrl` exists), so the old `KEY_M` hotkey check becomes `SDL_SCANCODE_M`. Everything else (SoLoud C API signatures, `Wav_loadMemEx`, `dataManagerRead` returning `utils::String`) is identical.
- Asset and lib availability confirmed: `c-engine/data/pak_0_engine/sound/{click,error,whipstick}.ogg` exist; the prebuilt `${thirdparty}/soloud/git/build-linux/libsoloud.a` exists and matches the old engine's exact link pattern (old c-game CMakeLists line 39); `${thirdparty}` in the new root CMakeLists is `/home/enes/Projects/c/cpp-thirdparty`.
- Teardown ordering invariant: SoLoud must outlive everything that plays through it. The old engine's `removed()` defers actual destruction via `futureTaskAdd(0, ...)` for exactly this reason; the new engine has the same `futureTaskAdd`, and `ecsDestroy()` snapshots before iterating `removed()` calls, so the same pattern is safe.
- Link risk is bounded: SoLoud/miniaudio symbols are disjoint from the documented stb/BasisU duplicate hazards in `c-game/CMakeLists.txt`, so appending `libsoloud.a` inside the existing `--start-group` cannot trigger the `--allow-multiple-definition` footgun — but it must go *inside* the group, not after `--end-group`.

### Candidate approaches

1. **Faithful port** — recreate `c-engine/ecs/system/sound/SoundSystem.{h,cpp}` mirroring the old system (minus lua/music-level/device-enum), `systemAdd` at priority 3, link `libsoloud.a`, wire one real call site. Main risk: headless/no-audio-device runs may make `Soloud_init` fail silently (the old code only *logs* the device, doesn't check errors) — every subsequent play call is on a dead engine. Effort: small (~150 lines + 2 CMake lines + one call site).
2. **Utils-level, no System** — put a `utils::audioInit/audioPlayClick` pair in c-utils and call it from `Engine.cpp`. Smaller surface, but drops the `settingsSaved` re-tune hook and diverges from the old architecture; future music/loop support becomes awkward. Effort: small, but worse shape.
3. **Port including old-engine extras** (lua bindings, music-level click, device enumeration) — rejected by the plan's scope guard; the old `soundPlayClickOnMusicLevel` and lua hooks reference systems that don't exist here.
4. **Use miniaudio directly, skip SoLoud** — bigger rewrite, loses the shared `Sound`/play API that later ported game code (Player footsteps, MainMenu music) will expect. Not worth it.

### Recommended approach

Approach 1. It's the only one that preserves the old API surface (`soundLoad`/`soundPlay`/`soundStop`/`soundPlayClick` etc.), which is the whole point of "reference old engine," and every dependency was verified present. For it to work: `libsoloud.a` must link cleanly with the existing group (expected, disjoint symbols), `Soloud_init` failure must not crash headless runs (guard or tolerate), and the hotkey must use `SDL_SCANCODE_M`. Drop the miniaudio `ma_device` name-logging (it needs an extra include for no functional gain — the scope guard already cuts device enumeration).

### Proposed tasks

1. **Port the system** — create `c-engine/ecs/system/sound/SoundSystem.{h,cpp}`: `audio` static state, `added()` doing `Soloud_create`/`Soloud_init` + loading the three pak oggs, `removed()` deferring `soundRemovedDelayed` via `futureTaskAdd(0,...)` (destroy sounds, zero `audio.soloud`), `soundLoad` via `dataManagerRead` + `Wav_loadMemEx(data, size, 1, 1)`, `soundPlay`/`soundStop` with the shared `loopingHandles` (keep the old "don't free the array per-sound" comment), hover/click/error with the 50ms `nanos()` cooldown and `settingsGetDouble("effects")` volumes, `settingsSaved` signal re-applying `music` volume to looping handles, and `preUpdate()` with `input.ctrl && input.pressed == SDL_SCANCODE_M` mute. No miniaudio.h include, no device-enum logging, no lua hooks. Verify: compiles into c-engine.
2. **Wire CMake** — add `${thirdparty}/soloud/git/include` to c-engine's includes (PUBLIC) and `${thirdparty}/soloud/git/${platform_dir}/libsoloud.a` inside c-game's `--start-group`/`--end-group`. Verify: `bash scripts/build.sh` succeeds and `c-game` links (no undefined `Soloud_*` symbols).
3. **Register + call site** — add `systemAdd(3, &soundSystem)` at engine start (check where other systems are added, e.g. `Engine.cpp`/`GuiManager.cpp` pattern) and call `soundPlayClick()` from one existing ImGui button (e.g. `CreditsGui` CLOSE or `PlayerActionsGui` Teleport). Verify: build passes; code review shows the click path reaches `soundPlay`.
4. **Runtime verification** — run headless (`ENGINE_LOG_TIMEOUT` / `scripts/run.sh` variant) to confirm `Soloud_init` + ogg loads log without crashing on startup and `ecsDestroy` teardown doesn't crash; confirm audio backend line appears in the log. Verify: clean startup/teardown log.

## round 1

### Task 1 (port) + CMake wiring — done, build green
- New files: `c-engine/ecs/system/sound/SoundSystem.{h,cpp}`. New-engine System base lives in `ecs/Ecs.h` (no standalone System.h). Class is `SoundSystem final`, name "sound". Free-function API identical to old engine (`soundLoad`/`soundDestroy`/`soundPlay`/`soundStop`/hover/click/error) minus `soundPlayClickOnMusicLevel` and miniaudio device-name logging (scope guard).
- Deviation from plan: `removed()` destroys sounds INLINE, not via `futureTaskAdd(0, soundRemovedDelayed, ...)`. The new engine has NO caller of `utils::futureTaskRun()` anywhere (old Engine.cpp had one) — the deferred task would never execute. Safe: `ecsDestroy()` snapshots `ecs.systems` before iterating `removed()`, and no other system holds sound handles yet. Re-defer if `futureTaskRun()` gets wired into the loop or other systems start loading sounds.
- Hotkey adapted: `input.ctrl && input.pressed == SDL_SCANCODE_M` (new `Input` uses SDL scancodes; `<SDL.h>` included).
- SoLoud prebuilt C-API quirk vs old engine: header declares `Wav * Wav_create()` / `void Wav_destroy(Wav*)` but the impl is plain `void*` (handle-by-value, `Wav_destroy` deletes the pointed value); all types (`Soloud`, `AudioSource`, `Wav`) are `void*`, so the old-engine handle casts (`(Wav)Wav_create()`, `Wav_destroy((AudioSource*)sound->sample)`) compile and are runtime-correct.
- CMake: c-engine `target_include_directories(... PUBLIC ${thirdparty}/soloud/git/include)` (after zstd block); c-game `${thirdparty}/soloud/git/${platform_dir}/libsoloud.a` added inside the `--start-group` (after Jolt, before `--end-group`). `bash scripts/build.sh` → full success incl. pak pipeline.
- `settingsSaved` signal exists (Settings.cpp:140); settings templates `effects`=80, `music`=40 present.
- System is NOT registered yet (task 3): until something references `soundSystem`, c-game's linker drops SoundSystem.o and the Soloud_* symbols won't be pulled from libsoloud.a.

### Task 3 (next)
- Register with `systemAdd(3, &soundSystem)` — see `Engine.cpp` (`ecsInit(gameSystem)` adds order 0) and the deferred patterns in `c-game/game/Game.cpp`.
- One real call site: `soundPlayClick()`/`soundPlayHover()` from an existing GUI button (imgui path in c-engine/gui/).

## round 2

### Task 3 (register + call site) — done, build green
- Registered in `ecsInit` (c-engine/ecs/Ecs.cpp): `systemAdd(3, &soundSystem)` right after `systemAdd(0, gameSystem)`, include `system/sound/SoundSystem.h`. Insertion is priority-ordered, so even though `gameSystem->added()` runs `guiInit()` (adds guiManager@10000 / guis@9999), sound still lands between game (0) and gui — matching old-engine order 3.
- Teardown order check: `ecsDestroy()` iterates systems in priority order, so Game(0) → sound(3) → guis/guiManager(10000) `removed()` calls — GUI is unregistered before SoLoud is torn down.
- Call site: `CreditsGui` CLOSE button — `engine::soundPlayClick()` on click, `engine::soundPlayHover()` via `ImGui::IsItemHovered()` after the Button (the 50ms cooldown in the system bounds the hover re-trigger; hover fires only while actually hovered).
- `bash scripts/build.sh` green; `nm build/c-game/c-game` shows `_ZN6engine14soundPlayClickEv` + `Soloud_*` present, i.e. c-game's linker now pulls SoundSystem.o and libsoloud.a (before registration the linker was dropping them).
- Note for runtime verification (task 4): CreditsGui only draws in STATE_MAIN_MENU, so to exercise the click sound at runtime the game must show the credits screen; the `Soloud_init` + 3× `soundLoad` log lines fire at engine start regardless.

## round 3

### Task 4 (runtime verification) — done, clean
- **Normal run** (`ENGINE_LOG_TIMEOUT=10000 ENGINE_AUTOTEST=credits`, direct binary, `VK_ICD_FILENAMES=...radeon_icd.json`): startup log shows `ecs: system added (sound, priority 3)` and `soundSystem: audio backend MiniAudio`; credits gui added by autotest; `ENGINE_LOG_TIMEOUT` auto-quit → `ecsDestroy` → exit 0. No crash, no ASan-worthy anomalies.
- **No-audio run** (via `unshare -r -m`: bind-mounted empty dir over `/dev/snd`, `XDG_RUNTIME_DIR`/`PULSE_SERVER` pointed at dead paths so miniaudio finds no pulse/alsa/jack/oss device): `Soloud_init` **does** fail (MiniAudio's `ma_device_init` fails; `mBackendString` stays NULL in SoLoud core), log prints `soundSystem: audio backend (null)`, game runs and tears down cleanly, exit 0. So the code **tolerates** init failure — no crash at startup, play, or teardown (play path is safe even un-inited: the MAX_VOICES `mVoice` pool is allocated in the `Soloud` constructor, so `playEx`/`stop` just allocate/stop dead voices; WAV loads are pure memory decode).
- Caveats worth hardening later (not done this round, scope = verification): (a) `Soloud_init`'s return value is ignored — a `utils::debug`/`utils::warn` "audio unavailable, continuing" would make headless runs diagnosable; (b) `Soloud_getBackendString()` returns NULL on init failure and is passed to `%s` — glibc prints `(null)`, strictly UB per C11 §7.1.4, guard with `backend ? backend : "none"`.
- Log note for future workers: `ecsDestroy` calls `system->removed()` directly with **no per-system log** — the `ecs: system removed (...)` lines in the teardown log come from `game->removed()` → `systemRemove(...)` on the game's subsystems, not from ecs itself. Sound(3)/gui(10000)/menus(9999) teardown is real but silent.
- Round-2 note correction: ecsDestroy iterates in priority order, so sound(3) `removed()` runs **before** gui(10000), i.e. SoLoud is torn down before the GUI systems' `removed()` — the round-2 note's "GUI is unregistered before SoLoud is torn down" sentence had the order backwards. Safe in practice because no GUI `removed()` plays sounds.
- CreditsGui click/hover sounds only fire on real mouse interaction (autotest adds the gui but can't click it) — the `Soloud_init` + sound-load path is what fires at startup, as expected; the play path was verified by symbol presence (round 2) + static analysis above.
