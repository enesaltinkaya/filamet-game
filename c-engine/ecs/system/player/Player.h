#pragma once

#include "ecs/Ecs.h"

namespace engine {
// Third-person player (port of the old engine's player controls —
// game-001-cpp c-game/game/player/Player.cpp, movement + character physics +
// locomotion animation: no abilities, no DB yet).
//
//   C      toggle player mode (drag orbit + WASD); from a fly it takes over
//   F      fly camera (takes over from player mode); ending the fly (ESC or
//          F) hands control back to the player — while flying the player
//          sticks to the camera (parked just below the eye, old engine's
//          playerFollowFlyingCamera), so it lands where the fly ended
//   WASD   run (4 m/s; SHIFT walks at 2 m/s), camera-relative
//   SPACE  jump (4 m/s impulse)
//   wheel  orbit distance (1.5–20 m)
//   LMB    drag: orbit camera (cursor is shown normally, hidden only during
//          the drag — the old engine's show/hide-on-button behaviour)
//   RMB    drag: orbit camera AND rotate the character to face the camera
//
// Physics: a Jolt CharacterVirtual capsule (r 0.25 m, 0.45 m half-cylinder —
// the old engine's dimensions) walked on the streaming heightmap
// heightfields: 45° max climb, 0.25 m stair steps, stick-to-floor on small
// drops — the same behaviour the old engine's joltCharacterUpdate gave.
// The character position is the FEET position (the shape is offset up by
// half its height inside the wrapper); the model is placed there and the
// orbit camera targets the capsule centre (feet + 0.70 m).
class PlayerSystem final : public System {
public:
    PlayerSystem();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern PlayerSystem playerSystem;

// Feet position (world metres) the player spawns at — the game sets this on
// world load (the old engine's hardcoded spawn) before the system is added.
void playerSetSpawn(f32 x, f32 y, f32 z);

// Move the character (and its Jolt controller) to a world position.
// Returns 1 on success, 0 when the player is not ready (not spawned yet).
char playerTeleportTo(f32 x, f32 y, f32 z);

char playerMode(void);  // 1 while player mode owns input + camera

// Live feet position (world metres, double precision — Jolt is double
// internally). Returns false when no player body is in the world (not
// spawned yet / automated no-player runs); consumers then treat the
// player as absent (e.g. the props player-push falloff reads as zero).
bool playerGetFootPos(double out[3]);
}  // namespace engine
