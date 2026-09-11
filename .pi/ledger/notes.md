# notes

## brainstorm

## Core difficulty

The rock's collision shape is restored at runtime from a pre-baked blob whose space (primitive-local vs node-space) and shape type (full mesh vs convex hull) are not known, so "player walks through the rock" has three mutually exclusive causes — scale missing/misapplied, body mispositioned or absent (name lookup), or shape-space mismatch — and only a measurement of the rock's world AABB vs the visual bounds can discriminate.

## Reductions / key lemmas

1. **Equivalence invariant.** `joltCreateBodyFromShapeBlob` creates a body at node pos/rot and decorates the blob with `ScaledShape(nodeScale)` (jolt_c_api.cpp:564-580, correct order: scale in local space under T·R). If the blob was baked in primitive-local space, the collision world AABB must *exactly* equal the visual AABB, because the same `gltfPropsNodeGlobalTRS` decomposition is used for the visual transform. So if JOLT_DEBUG shows world AABB ≠ visual bounds, the blob is not in the space we assume, or the body isn't the rock's (name mismatch → `utils::warn("...no node transform")` and body skipped, leaving no collision at all).
2. **Hull direction of error.** A convex-hull approximation of a big rock is a *larger* envelope — it over-blocks, it does not let the player walk in. The observed symptom (walk-through) points at collision too small or mispositioned, i.e. scale-not-applied / stale sidecar / name miss, not hull concavity. Hulls matter only if the rock was baked from a *subset* of its geometry.
3. **Layer/filter unlikely.** The character's step queries use `GetDefaultLayerFilter(Layers::MOVING)` (jolt_c_api.cpp ~1195-1251) and the MOVING layer collides with NON_MOVING; props static bodies are NON_MOVING. A filter miss is a one-grep check, low probability.
4. **Measurement chain already instrumented.** `JSB_DEBUG` at bake prints `rawAABB` (raw glTF verts) vs `shapeLocalAABB` (Jolt local bounds) — proves what space the blob lives in. `JOLT_DEBUG` at restore prints shape type, pos, scl, `innerLocalSize`, world AABB per static body. Verdict rule: `world extent ≈ scl × innerLocalSize` (sanity), `world AABB` covers visual bounds, and no "no node transform" warn for the rock.
5. **Staleness is a first-class suspect.** The JBVH sidecar is a baked pak in the data dir; the runtime scale fix does nothing if the pak was baked before/without the relevant glTF state, or if it was never re-baked after the scale fix landed.

## Candidate approaches

- **A. Measure first, then branch.** Run the parked scene with `JOLT_DEBUG=1`; capture the rock's line, the props load count, and any "no node transform" warn; compare world AABB to the visual (scaled glTF) bounds; re-run bake with `JSB_DEBUG=1` if needed. Risk: visual bounds are not printed anywhere, so someone must compute them from the glTF + node TRS by hand (small script or RenderDoc). Effort: 1-2 runs + a bounds computation, ~30 min.
- **B. Bake the scale in.** Change `tools/jolt-shape-builder` to read the node TRS and multiply vertices by scale before building the shape (blob = node-space, runtime scale becomes 1 or is skipped for props). Risk: double-scaling if the runtime ScaledShape path stays active for the same entry; every sidecar pak must be re-baked; changes the sidecar contract for terrain/other props. Effort: tool change + re-bake + re-verify, 1-2 sessions.
- **C. Character path fix.** If measurement shows the rock's world AABB *already* covers the visual and the player still passes through, the bug is in the CharacterVirtual: query filter, capsule layer, or contact/depenetration offsets. Risk: chasing the wrong layer if A shows a geometry mismatch. Effort: mostly greps once A rules geometry out; fix 1-2 h if real.
- **D. Runtime restore hardening.** Make `propsSidecarLoad` log pos/rot/scl per entry and assert the node lookup hit (turn the silent-skip into a loud failure with the entry name). Risk: none, but it's a diagnostic, not a fix. Effort: 30 min.

## Recommended approach

A + D together: instrument the restore path to print the node TRS and any lookup misses, run once with `JOLT_DEBUG=1`, and compare against the visual bounds — the equivalence invariant in lemma 1 means a single run discriminates between "scale not effective/stale pak", "name miss/body absent", and "character query". It's cheapest and can't commit us to the wrong fix; B and C are chosen by what the numbers say. Must be true: the JOLT_DEBUG line's world AABB is computed through the outer (scaled) shape (it is — jolt_c_api.cpp:605 uses the outer `shape`), and the visual bounds are computed from the same glTF + `gltfPropsNodeGlobalTRS` the engine uses, not a different transform source.

## Proposed tasks

1. **Add restore-side logging (D).** In `propsSidecarLoad` (PhysicsSystem.cpp:159), print per entry: name, found pos/rot/scl, body ptr; make a node-lookup miss a loud warn with the entry name (it already warns — verify it fires). Back up the file to /tmp first (no git).
2. **Ground-truth run.** `TERM=xterm-256color ENGINE_HIDDEN_WINDOW=1 ENGINE_AUTOTEST=enter JOLT_DEBUG=1 timeout 12 ./scripts/run.sh` in the parked scene; capture the rock's JOLT_DEBUG line, the `N props bodies` count, and any transform-miss warn. Separately compute the rock's expected visual AABB from its glTF node TRS × primitive raw bounds (small standalone script or RenderDoc) and state the verdict: scale missing / mispositioned / body absent / geometry fine (→ task 4 becomes character path).
3. **Sidecar freshness check.** Identify which glTF and when `build/c-game/data`'s props JBVH pak was last generated vs the current glTF; if stale, re-bake with `JSB_DEBUG=1`, compare `rawAABB`/`shapeLocalAABB`, and re-run task 2.
4. **Fix + verify.** Apply the fix indicated by task 2/3 (re-bake, restore fix, or character-query fix), then verify: rock world AABB covers the visual in JOLT_DEBUG, and a screenshot run (`ENGINE_SCREENSHOT=/tmp/verify.jpg`, per plan's verification command) shows the capsule stopped at the rock face with the parked player untouched.

## round 1

### Ground truth run (task 1)

Built, ran parked scene with `JOLT_DEBUG=1` (logs: `/tmp/groundtruth.log` no-names, `/tmp/groundtruth2.log` with temp entry-name log). 16 terrain + **61 props bodies**, **zero** `no node transform` warns — every sidecar name resolves. Temp log added in `propsSidecarLoad` (name/pos/rot/scl per entry, JOLT_DEBUG-guarded), file backed to `/tmp/PhysicsSystem.cpp.bak` and then **restored** (working tree clean again).

Key lines (name | shape, pos, scl, world AABB):
- `Cube.001` (the scaled rock; node scale **3.35**): `Scaled(Mesh) pos=(-424.8,511.1,1651.1) scl=(3.35,3.35,3.35) innerLocalSize=(2,2,2) world=(-428.1,507.8,1647.7)-(-421.4,514.5,1654.4)`
- `vjrmfb1ab_..._0` (big rock 264x153x140, scale 1): `Mesh pos=(-799.8,494.9,1802.5) scl=1 world=(-941.5,486.2,1710.3)-(-658.2,638.8,1891.5)`
- `vjrmfb1ab_..._0.002` (scale **0.334**): `Scaled(Mesh) pos=(-227.0,508.1,1523.6) scl=(0.334) world=(-298.2,503.8,1473.8)-(-155.1,578.7,1574.8)`

Visual AABBs computed from `/tmp/test2.glb` (unzstd `c-game/data/pak_1/models/test2.zstd`) node TRS x raw POSITION min/max, per node hierarchy. All scaled props near the player (Cube.001, cubeNoMaterial 0.5, vjrmfb1ab.002) match body world AABB **exactly** (<=0.1) — the runtime ScaledShape fix is effective; scale is NOT missing, rocks are NOT mispositioned, no body absent.

**Verdict: geometry fine for the rocks -> suspect is the character path (task 4).**

Caveat + extra findings for later rounds:
- Parked **camera** is at (-320,588,344) looking at a terrain hill (`/tmp/rock.jpg` shows terrain only); the player is parked at (-420.4,508.0,1628.8) facing roughly +z, where the props cluster is. So the screenshot does not frame the rock; the "rock in front" at ~22 units +z is the scaled cube `Cube.001` (7.7^3). If the user meant a different object, re-park and re-verify.
- **Stale blobs (body mispositioned) for the small cubes in the same cluster** — baked geometry no longer matches the current glTF (sidecar `test2.jolt.zstd` baked from older vertex data):
  - `Cube.005`: body shifted **-2.0 in Y** vs visual (vis top 514.2, body top 512.2)
  - `Cube.012/013/014`: body shifted **-4.7..-5.1 in Y** (vis tops 519.6-520.1, body tops 514.9-515.4)
  - `Cube.015`: body shifted **-10.8 X, -8.6 Z** (blob centered at node origin; visual mesh local bounds y -1.4..22.9, z 0.4..16.7)
  - `SM_HP_Tree*` (all 33): blob = **prim0 trunk only** (innerLocalSize 1049x2464x1354 scaled ~0.01); canopy prim1 has no collision.
  These are exactly the "stale sidecar / re-bake" items for task 2; the sidecar (14:56) is newer than the glTF (14:41) yet disagrees, so the bake source file differs from the current pak copy.
- `deer_001` minor 0.9-unit diff (hull noise). `vjrmfb1ab.001` (scale 1, at (1136.9,362.2,-822.7)) matches in extent; its node has multiple parents in the glb, engine picks one.
- JOLT_DEBUG `world=` AABB is tight (verified against rotated local extent for the 170-deg-rotated vjrmfb1ab.0: x-extent 283.3 = 263.9*cos+139.9*sin). Sanity `world extent == scl x innerLocalSize` holds for all scaled bodies.
- db.db has tables `player`/`camera` (not `transform`): player=(-420.39,507.95,1628.76,...), camera=(-401.7,515.9,1626.8) + stored orientation.


## round 2

### Character controller path (task 4) — diagnosis

**Verdict: the capsule passes through the rock because the rock body is a closed 2x2x2 box mesh
(Cube.001 blob, innerLocalSize exactly (2,2,2), ScaledShape 3.35) and Jolt's character queries run in
`IgnoreBackFaces` mode — a capsule strictly INSIDE the box registers ZERO contacts with it. The world
AABB covering the visual is irrelevant: AABB is only the broadphase envelope; narrow phase is
triangle-based and backfaces never collide.**

Measured (all with the wrapper's JOLT_DEBUG hooks; rock = body 17, Scaled(Mesh), center
(-424.8,511.1,1651.1), world AABB (-428.1,507.8,1647.7)-(-421.4,514.5,1654.4); terrain = body 9,
big heightfield whose surface at the rock footprint is y~510.0 and dips to ~508.4 at the parked spot):

1. Layers/filters are fine. Character queries use `GetDefaultBroadPhaseLayerFilter(MOVING)` +
   `GetDefaultLayerFilter(MOVING)` (jolt_c_api.cpp ExtendedUpdate call); layer table lets MOVING collide
   with NON_MOVING; props bodies are `Layers::NON_MOVING`. A cast from outside the rock HITS the rock
   (t=0.537 from 0.3 in front) with the character's EXACT cast settings (backface ignore, shrunk
   shape, CollideOnlyWithActive — probed via new JOLT_TEST_CASTC hook). So the rock is visible from
   outside and blocks a cast.
2. Static `CollideShape` of the real capsule shape (same filters) at interior points:
   (-424.8,509.0,1648.5) 0 hits; (-424.8,510.0,1651.0) 0; (-426.5,509,1651) 0; (-423.5,509,1651) 0.
   Partial penetration still collides: at z=1647.7 (0.25 past the front face) the rock IS hit; at
   z=1648.25 (fully inside) the rock disappears. Transition = capsule no longer intersecting the box
   surface.
3. Real character embedded (ENGINE_TELEPORT=-424.8,510.0,1651.0 + ENGINE_AUTO_RUN=1, which suppresses
   db saves so the parked state was NOT touched): first frame `ground=3 (StuckInFloor) hits=0` — the
   controller sees NOTHING inside the solid. It then walks freely inside the box (x -424.8 -> -427.9,
   y 510.0 -> 507.8) and stops only when it hits the TERRAIN surface that passes through the box
   interior (hits=2 body 9). Walk-through through a solid, reproduced end to end.
4. Drop tests around the rock (teleport y=515, settle): terrain surface 510.01 at z 1638-1643 and at
   z 1656 (behind); capsule lands ON the box top (feet 514.5) when dropped at z 1650/1653. The box is
   solid from outside on all sides; a normally-walking player on the terrain is blocked by the
   TERRAIN wall at z~1643.8 (3.9 short of the front face — auto-run from the parked spot crawls at
   0.4 m/s, ground=0 InAir permanently: the capsule is always slightly embedded in the terrain and
   slowly sinking 507.95->507.88).
5. So the only realistic embed paths in actual play: fly-camera takeover (playerFollowFlyingCamera /
   fly-end parks the capsule at the camera pos via SetPosition with no collision check) or a player
   DB row that was saved while the capsule sat inside the rock (postUpdate saves p.pos every second,
   autoRun-suppressed only). After embed, backface-blindness makes the rock invisible to the whole
   controller (cast, ground, depenetrate) — nothing can push the capsule back out; that is the
   observed "runs into the rock".
6. Extra: a cast starting fully inside a closed mesh reports t=0 hits (front-face artifacts) under
   default ShapeCastSettings; under the character's own settings (shrunk shape) the embedded
   character still moved, so those artifacts do not block.

Notes on state:
- Wrapper `cpp-thirdparty/jolt/wrapper/src/jolt_c_api.cpp` was extended (env-gated, off by default):
  JOLT_TEST_CAST now prints per-hit fraction (`t=`) + backface flag (`bf=`); new JOLT_TEST_CASTC runs
  the cast with CharacterVirtual's exact settings. Backup of the original at /tmp/jolt_c_api.cpp.bak.
  build-linux/libcjolt.a was rebuilt (build-win NOT touched; wrapper build.sh would rebuild both).
- Parked player/camera untouched (all behavioral runs used ENGINE_AUTO_RUN=1 which suppresses
  playerDbSaveState; verify db rows unchanged if in doubt).
- Ground states seen: ground=2 (WalkOnStairs) at terrain ledges, ground=3 (StuckInFloor) when
  embedded; ground=0 (InAir) is the PERSISTENT state at the parked spot (capsule embedded in
  terrain, never OnGround) — a secondary defect worth fixing in task 5 (mPenetrationRecoverySpeed
  push-out is losing to per-tick gravity re-embed).

Fix candidates for task 5 (cheapest first):
- (a) In joltCharacterUpdate, add a backface-aware depenetrate: CollideShape with
  mBackFaceMode=EBackFaceMode::CollideWithBackFaces (and convex too), push the character out along
  the deepest contact each tick (rate-limited). Makes the rock solid from the inside too, fixes any
  embed path (fly takeover, stale db row).
- (b) Bake Cube.* props as convex hulls (task 2 re-bake) — but verify whether convex-inside contacts
  register for CharacterVirtual; risky, and does not fix fly-teleport embeds in ANY solid.
- (c) Prevent embeds: clamp fly-park / teleport to the nearest non-embedded position (CastRay up +
  collide test with backfaces).
Recommend (a) as the primary fix; it is the only one that covers every entry path.

## round 3

### Task 5 (fix implementation) — done

Wrapper `cpp-thirdparty/jolt/wrapper/src/jolt_c_api.cpp`, `joltCharacterUpdate` now has a
backface-aware depenetrate before the controller's `ExtendedUpdate` (backup of the
pre-round-3 file at `/tmp/jolt_c_api.cpp.round3.bak`; the round-2 JOLT_TEST_* env-gated hooks
are still in the tree, unchanged):

1. **Step 1 — collide push-out.** `CollideShape` of the capsule with
   `mBackFaceMode = EBackFaceMode::CollideWithBackFaces`, deepest hit wins, capsule moved by
   `-mPenetrationAxis * mPenetrationDepth` (Jolt contract: axis = direction to move the *body*
   to resolve; capsule moves opposite — verified empirically, sign correct).
   **Terrain bodies are excluded** (`BodyInterface::GetUserData == JOLT_TERRAIN_USER_DATA`,
   i.e. the 16 chunk bodies; props use userData 0). This exclusion is load-bearing: without it,
   the parked capsule (0.7 m embedded in terrain at the park spot) pops to the surface once and
   then slides ~0.2 m/s down the slope (broken ground state, ground=3/StuckInFloor, no friction),
   which both corrupted the saved player row and moved the parked player.
2. **Step 2 — containment escape.** A capsule *fully* inside a closed mesh registers zero
   contacts even with backface collide (no face intersection — measured in round 2), so step 1
   alone does nothing for a center-of-rock embed. Six `CastShape` probes (±up, ±X, ±Z, 500 m,
   backface mode on, terrain excluded): contained iff ALL six first hits are backface hits
   (inside a closed solid ⇒ every ray exits a backface; standing in a cave/next to a wall gives
   frontface first hits). Escape = teleport the capsule just past the **farthest** exit face
   (travel = dist + capsule span along that axis + 0.05 m); the full-span push puts the whole
   capsule clear of the face in one tick, and any remaining straddle with other solids is
   resolved by step 1 next frame. "Farthest" (not "nearest") matters: with the nearest rule the
   capsule bounces on the interior floor it is standing on and never leaves the rock.

Measured results:
- Embed (`ENGINE_TELEPORT=-424.8,510.0,1651.0` inside Cube.001 + auto-run): escape fires on the
  2nd-3rd frame (dir=+up, dist 4.12, span 1.4 → feet end 515.2, above the rock top 514.5),
  capsule then lands on top, walks off the ledge, and is blocked by the rock's front faces from
  outside (x pinned exactly at face − radius). No wall tunneling in any run.
- Parked spot (no teleport): **zero** depenetrate/escape events, x drift = 0, capsule stays in
  its original terrain-embedded equilibrium (the pre-existing slow-sink is unchanged, no new
  motion).
- Pinned verification passes: `... ENGINE_SCREENSHOT=/tmp/verify.jpg ... timeout 12
  ./scripts/run.sh` → exit=0, `/tmp/verify.jpg` written, scene frames as before (terrain hill,
  player at park spot).

### IMPORTANT: parked DB rows were clobbered and have been restored

Two non-automated runs (no `ENGINE_AUTO_RUN`, which is the only gate on the 1 Hz `playerDbSaveState`)
saved the drifted capsule position before I realized saves were not suppressed. `db.db`
(`build/c-game/data/db/db.db`) rows were restored from log-recovered values:
- **player** pos = (-420.395721, 507.953308, 1628.768799) — exact f32 from the round-2
  JOLT_DEBUG first-frame log (`/tmp/walk2.log`); modelYaw/camYaw/camPitch/camDist/moveYaw from the
  pre-clobber row (angles are input-driven, unaffected by the drift).
- **camera** pos = (-401.7, 515.9, 1626.8) + yaw 96°/pitch −20.1° — position only known to
  0.1 m from the "flying camera: loaded saved state" log (exact f32 was lost); the eye is at
  most ~5 cm off the original. **Ask the user to re-park / confirm the framing if the 5 cm
  matters** — the player position itself is exact.
- Clobbered copy kept at `/tmp/db.db.clobbered.bak`.

Rule for all future rounds: any game run that may write the db (i.e. anything WITHOUT
`ENGINE_AUTO_RUN=1` — note `ENGINE_SCREENSHOT` does NOT suppress the player-row save, it only
skips the camera save via `p.active`) must be followed by a db check. The pinned verification
command in plan.md re-saves the player row (sink drift ~0.1 m/12 s); that is the pre-existing
behavior, but it means the pinned verification should be the last run before final db state is
needed, or run with `ENGINE_AUTO_RUN=1` added.

### Remaining / for later

- Pre-existing ground-state defect (NOT fixed, out of scope): capsule at the park spot has
  ground=3 (StuckInFloor) or ground=0 (InAir) instead of OnGround — it slowly sinks into the
  terrain (~0.07 m over 12 s) and has no friction (would slide on slopes if lifted). A proper
  controller fix (ground-state/predictive-contact tuning) is the follow-up; note that simply
  lifting the capsule to the terrain surface exposes the slide, which is why terrain is
  excluded from the new depenetrate.
- Round-2 item (task 2) still open: stale prop blobs (Cube.005/012-015 mispositioned,
  SM_HP_Tree* trunk-only) need a re-bake of `test2.jolt.zstd`.
