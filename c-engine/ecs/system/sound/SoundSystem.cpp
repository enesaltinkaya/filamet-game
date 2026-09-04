#include "SoundSystem.h"
#include "Utils.h"
#include "renderer/Window.h"

#include "soloud_c.h"
#include <SDL.h>

namespace engine {
struct Sound {
    Wav sample;
    int handle;
};

static struct {
    Soloud soloud;
    std::vector<int> loopingHandles;
    Sound* buttonHoverEffect;
    Sound* buttonClickEffect;
    Sound* errorEffect;
} audio;

static void settingsSaved(void* _);

SoundSystem soundSystem;

SoundSystem::SoundSystem() : System("sound") {}

void SoundSystem::added() {
    audio.soloud = (Soloud)Soloud_create();
    Soloud_init((Soloud*)audio.soloud);

    audio.buttonHoverEffect = soundLoad("sound/whipstick.ogg");
    audio.buttonClickEffect = soundLoad("sound/click.ogg");
    audio.errorEffect       = soundLoad("sound/error.ogg");

    utils::signalSubscribe("settingsSaved", settingsSaved);

    const char* backend = Soloud_getBackendString((Soloud*)audio.soloud);
    utils::debug("soundSystem: audio backend %s", backend);
}

void SoundSystem::removed() {
    // destroy inline: the new engine never calls utils::futureTaskRun(), so
    // the old engine's next-frame deferral (let other systems drop handles
    // first) is a no-op here.  ecsDestroy() snapshots ecs.systems before
    // iterating removed(), and no other system holds sound handles yet.
    // Re-defer (futureTaskAdd(0, ...)) if a futureTaskRun() is wired into
    // the loop or other systems start loading sounds.
    soundDestroy(audio.buttonClickEffect);
    soundDestroy(audio.buttonHoverEffect);
    soundDestroy(audio.errorEffect);

    audio.soloud = 0;
}

Sound* soundLoad(const char* path) {
    Sound* sound = new Sound{};
    sound->sample = (Wav)Wav_create();

    utils::String soundFileContents = utils::dataManagerRead(path);
    Wav_loadMemEx((AudioSource*)sound->sample, reinterpret_cast<const unsigned char*>(soundFileContents.data), soundFileContents.size, 1, 1);
    utils::stringDestroy(&soundFileContents);
    return sound;
}

void soundDestroy(Sound* sound) {
    if (!sound) return;
    soundStop(sound);
    Wav_destroy((AudioSource*)sound->sample);
    delete sound;
    // Note: the handle is unlinked from audio.loopingHandles inside
    // soundStop.  That array is process-wide (owned by the sound system,
    // shared by every looping sound), so it must NOT be freed here — freeing
    // it per-sound corrupts the heap for the next looping sound.
}

void soundPlay(Sound* sound, float volume, bool loop) {
    if (loop) {
        sound->handle = Soloud_playBackgroundEx((Soloud*)audio.soloud, (AudioSource*)sound->sample, volume, 0, 0);
        Soloud_setLooping((Soloud*)audio.soloud, sound->handle, 1);
        audio.loopingHandles.push_back(sound->handle);
    } else {
        Soloud_playEx((Soloud*)audio.soloud, (AudioSource*)sound->sample, volume, 0, 0, 0);
    }
}

void soundStop(Sound* sound) {
    if (!sound || !sound->handle) {
        return;
    }
    Soloud_stop((Soloud*)audio.soloud, sound->handle);

    for (i32 i = 0, s = static_cast<i32>(audio.loopingHandles.size()); i < s; i++) {
        if (audio.loopingHandles[i] == sound->handle) {
            audio.loopingHandles[i] = audio.loopingHandles.back();
            audio.loopingHandles.pop_back();
            break;
        }
    }
}

void settingsSaved(void* _) {
    for (i32 i = 0, si = static_cast<i32>(audio.loopingHandles.size()); i < si; i++) {
        int handle = audio.loopingHandles[i];
        Soloud_setVolume((Soloud*)audio.soloud, handle, utils::settingsGetDouble("music") / 100.);
    }
}

static double lastPlayed;

void soundPlayHover(void) {
    if (utils::nanos() > lastPlayed + MILLION * 50) {
        lastPlayed = utils::nanos();
        soundPlay(audio.buttonHoverEffect, utils::settingsGetDouble("effects") / 100 / 5.f, 0);
    }
}

void soundPlayClick(void) {
    if (utils::nanos() > lastPlayed + MILLION * 50) {
        lastPlayed = utils::nanos();
        soundPlay(audio.buttonClickEffect, utils::settingsGetDouble("effects") / 100., 0);
    }
}

void soundPlayError(void) {
    if (utils::nanos() > lastPlayed + MILLION * 50) {
        lastPlayed = utils::nanos();
        soundPlay(audio.errorEffect, utils::settingsGetDouble("effects") / 100., 0);
    }
}

void SoundSystem::preUpdate() {
    if (input.ctrl && input.pressed == SDL_SCANCODE_M) {
        utils::settingsSetDouble("music", 0);
        utils::settingsWrite();
    }
}
}  // namespace engine
