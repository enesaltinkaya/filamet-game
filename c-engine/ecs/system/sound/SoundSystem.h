#pragma once

#include "ecs/Ecs.h"

namespace engine {
class SoundSystem final : public System {
public:
    SoundSystem();
    void added() override;
    void removed() override;
    void preUpdate() override;
};

extern SoundSystem soundSystem;

struct Sound;
struct Sound* soundLoad(const char* path);
void soundDestroy(struct Sound* sound);

void soundPlay(struct Sound* sound, float volume, bool loop);
void soundStop(struct Sound* sound);

void soundPlayHover(void);
void soundPlayClick(void);
void soundPlayError(void);

/* Click at the current MUSIC level (no effects scaling, unthrottled) — the
 * settings audio page plays it after a music-slider change so the new level
 * is heard immediately (old engine's soundPlayClickOnMusicLevel). */
void soundPlayClickOnMusicLevel(void);
}  // namespace engine
