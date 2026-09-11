# plan

Context: the props physics bodies are restored at runtime from pre-baked JBVH blobs
(`PhysicsSystem::propsSidecarLoad`) with the node's global TRS from
`gltfPropsNodeGlobalTRS`; the working-tree fix already passes the node scale into
`joltCreateBodyFromShapeBlob`, which wraps the blob in a `ScaledShape` when scale != 1
(wrapper at cpp-thirdparty/jolt/wrapper/src/jolt_c_api.cpp). The player capsule still
passes through the rock, so the effective collision shape is smaller than (or mispositioned
relative to) the scaled visual.

Strategy: (1) get ground truth by running the parked scene with `JOLT_DEBUG=1` — the
wrapper prints each static body's shape type, pos, scl, and world AABB — and compare the
rock's world AABB against the rock's visual (scaled glTF) bounds; this tells us whether the
scale is missing, wrong, or the baked blob's local space simply doesn't match the node space.
(2) check the bake side (`tools/jolt-shape-builder`): what shape type the rock got
(Mesh vs ConvexHull vs primitive-subset) and in which space the vertices were baked —
a convex hull or a single-primitive mesh on a big rock is a classic "walk into the rock"
signature. (3) rule out the character-controller path (layer/filter on the capsule's
collision queries vs props static bodies) only after the AABB comparison points at it.
(4) fix wherever the mismatch is (re-bake, apply scale at bake time, or fix the runtime
restore) and re-verify the rock's world AABB covers the visual.

Constraints: AGENTS.md says no git — record the baseline commit below for reference, but
do NOT run git restore/checkout; back up any file to a /tmp copy before editing. Do not
move the parked player/camera (db.db transforms). No code comments.

Verification: cd /media/extra/Projects/c/filament-game && TERM=xterm-256color ENGINE_HIDDEN_WINDOW=1 ENGINE_AUTOTEST=enter ENGINE_SCREENSHOT=/tmp/verify.jpg timeout 12 ./scripts/run.sh; echo "exit=$?"; ls -la /tmp/verify.jpg

Baseline commit: 05d1c14f366daacb297dc85c4925b0c340a03dc4 (dirty — PhysicsSystem.cpp props-scale fix, jolt-shape-builder main.cpp JSB_DEBUG hook + binary, SsaoDiligent.cpp bool fix; these working-tree changes are part of the task state and must be preserved, not reverted)
