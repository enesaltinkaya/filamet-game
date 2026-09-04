# plan

## Strategy

Port the sound subsystem from the old engine (/home/enes/Projects/c/game-001-cpp/c-engine/ecs/system/sound/SoundSystem.{h,cpp}) into the new engine. The new engine already has: the ECS System base class (c-engine/ecs/Ecs.h), the SoLoud prebuilt lib (/home/enes/Projects/c/cpp-thirdparty/soloud/git/build-linux/libsoloud.a — same pattern as the old engine's `${thirdparty}/soloud/git/${platform_dir}/libsoloud.a`), the ogg assets already packed in c-engine/data/pak_0_engine/sound/ (click.ogg, error.ogg, whipstick.ogg), and utils::dataManagerRead in c-utils.

Approach:
1. Add c-engine/ecs/system/sound/SoundSystem.{h,cpp} mirroring the old SoundSystem (SoLoud C API: Soloud_create/init, Wav via dataManagerRead, soundPlay/soundStop, hover/click/error helpers). Adapt to new-engine idioms: use the new `System` base, `systemAdd(3, &soundSystem)` in Ecs.cpp (old engine used order 3), and whatever the new engine uses for delayed tasks/signals (or drop `settingsSaved`/`futureTaskAdd` hooks if they don't exist here — keep it minimal but functionally equivalent).
2. Link libsoloud.a (check c-engine/c-game CMakeLists; old engine linked it in c-game's CMakeLists with ${thirdparty} + ${platform_dir}) and add its include path.
3. Wire at least one real use site (e.g. UI hover/click in the new engine's gui, or a debug hotkey in c-game) so the sound path is exercised end to end.
4. Build; if a headless audio run is possible verify no crash on startup/teardown.

Scope guard: do NOT port old-engine lua, music-level hooks, or device enumeration logging — keep it lean. Do not touch diligent/ or unrelated files.

Verification: bash /media/extra/Projects/c/filament-game/scripts/build.sh
Baseline commit: 4f386199cce818d3c3256049078dcb29a40aa615 (dirty)
