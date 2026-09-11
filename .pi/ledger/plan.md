# Plan

The symptom — a shadow cut-out/clipped on a long cliff — is the classic
CSM cascade-boundary or far-cliff-depth failure: the cliff top and face span
multiple cascades, so the shadow map depth range for that region is too coarse
(or the cliff pokes outside the camera frustum for the shadow pass / is clipped
by the near plane of the shadow camera), leaving the map "empty" there and the
shadow visibly missing on the ground below. Strategy: inspect the shadow/cascade
setup in the Diligent renderer (c-engine/renderer/DiligentRenderer.cpp, shadow
pass, cascade splits, depth bias, view matrix) via a RenderDoc capture of the
parked scene (docs/renderdoc-capture.md, scripts/rdc.py) to confirm which
cascade the cliff is in and what its depth range is; then fix by adjusting
cascade splits / expanding the far cascade to cover the cliff, applying
proper depth bias, or clamping shadow projection so the cliff is never
clipped. Verify before/after with a headless screenshot from the parked
player position (do NOT move player/camera — they are parked on the cliff by
the user) and compare visually.
Approach: screenshot baseline → rdc.py list/dump shadow pass → tune
cascade/bias parameters in code → rebuild → screenshot compare.

Verification: TERM=xterm ENGINE_HIDDEN_WINDOW=1 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json ENGINE_AUTOTEST=enter ENGINE_SCREENSHOT=/tmp/verify.jpg timeout 30 ./scripts/run.sh
Baseline commit: 93e31c7c2ee0c063d693ebe407663a23364e3507 (dirty: AGENTS.md modified, .pi/ untracked)
