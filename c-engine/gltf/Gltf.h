#pragma once

#include "Defines.h"

namespace engine::gltf {

bool gltfInit(void);
bool gltfLoad(const char* pakPath);
void gltfUpdate(double elapsedSeconds);
void gltfFrameCamera(void);
void gltfDestroy(void);

// Move the loaded instance so its LOCAL ORIGIN (the feet for character
// assets) lands at ABSOLUTE world (x, y, z). The local origin is the pivot,
// NOT the AABB min corner (eve's min corner sits 0.64 m to the origin's
// left). double precision: the placement is re-expressed relative to the
// world anchor internally so far-from-origin positions keep sub-mm precision.
// false if nothing is loaded.
bool gltfPlaceAt(double x, double y, double z);

// Same, plus a yaw (radians) around world +Y, pivoted on the local origin
// (the feet). The model's local +Z is its forward at yaw 0, so the character
// faces (sin yaw, 0, cos yaw) — the old engine's (moveDir.x, moveDir.z) ->
// atan2(x, z) convention.
bool gltfPlaceAtFacing(double x, double y, double z, f32 yaw);

// Local-space (unplaced) bounding box of the asset, true metres (compensated
// for the cm-authored wrapper scale). false if nothing is loaded.
bool gltfLocalBoundingBox(f32 min[3], f32 max[3]);

// World-space bounding box of the loaded asset; false if nothing is loaded.
bool gltfBoundingBox(f32 min[3], f32 max[3]);

// ── Animation ───────────────────────────────────────────────────────────────
// Clips live in a separate animation-source asset (old engine's
// models/animations.dat): a glb carrying the same skeleton + all clips but no
// textures. Load it with gltfLoadAnimations; gltfUpdate then plays the
// selected clip and syncs the joint transforms onto the loaded (visible)
// model. If no source asset is loaded, clips of the loaded model itself are
// used instead.
bool gltfLoadAnimations(const char* pakPath);

u32 gltfAnimationCount(void);
const char* gltfAnimationName(u32 index);  // "" if unnamed; valid until gltfDestroy
f32 gltfAnimationDuration(u32 index);      // seconds

// Start (re)playing a clip from t=0. false if not found or no animation source.
bool gltfPlayAnimation(const char* name, f32 speed, bool loop);
// Same, but crossfades in over blendSeconds (0 = instant switch).
bool gltfPlayAnimationBlended(const char* name, f32 speed, bool loop, f32 blendSeconds);
// Stop playback, keeping the last applied pose.
void gltfStopAnimation(void);


}  // namespace engine::gltf
