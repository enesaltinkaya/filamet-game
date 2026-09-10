# Plan

Strategy: the task is an investigation ("terrain seems to be taking too much gpu time"), so we first measure, then fix if a clear culprit emerges. Approach: (1) profile a running frame — use the RenderDoc capture flow from docs/renderdoc-capture.md (run.sh renderdoc + scripts/rdc.py) or the built-in GPU timers, and identify the terrain pass and its share of the frame; (2) inspect the terrain render path in c-engine/c-game (draw calls, mesh size, overdraw, blend state, splat shader cost in splat_terrain_ps.hlsl, sampling of large maps) to find concrete inefficiencies; (3) implement the cheapest high-impact fix found (e.g. cheaper splat shader, reduce overdraw/blending, cull, fewer passes); (4) re-capture and compare GPU time before/after to prove the win. Workers append findings to notes.md; keep each round to one concrete experiment or fix.

Verification: cmake --build build -j$(nproc)
Baseline commit: 3e22b1fcc4508abb36f7337500d885f5706f489e (dirty)
