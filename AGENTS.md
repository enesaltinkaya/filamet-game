### File locations

Everything related to project is either here /media/extra/Projects/c/filament-game or in thirdparty directory /home/enes/Projects/c/cpp-thirdparty.

You are not allowed to use "find / ...".
You are not allowed to use git.

use `./scripts/run.sh` (or its variants) to launch the game.
`scripts/run.sh` runs clear under set -e and it fails when TERM env variable is unset.

### Screenshot feature

`ENGINE_SCREENSHOT=path` (env var) makes the engine capture one frame and save it as a JPEG (quality 90, via stb_image_write in c-engine/renderer/Renderer.cpp) a few frames after startup, for automated runs. One-shot — only the first capture is written.

Example: `ENGINE_SCREENSHOT=/tmp/shot.jpg ./build/c-game/c-game` — save screenshots to `/tmp/` to keep the project folder uncluttered.

### Old engine

We will porting our old engine /home/enes/Projects/c/game-001-cpp to this new engine.

### Render path

We are only using diligent (filament was removed 2026-09-05; its
implementation lives on in git history as the look/parity reference).

Sources are here with samples and docs;
/home/enes/Projects/c/cpp-thirdparty/diligent

### Lessons

After a multi-hour debugging session, record the rule + incident in `docs/lessons.md` (dated entries, rule first). Read it before fighting renderer/texture/buffer weirdness — known pitfalls (e.g. Diligent dynamic-buffer ring clobbering) are listed there.
