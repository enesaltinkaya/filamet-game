#!/bin/bash
set -e

ROOT="$(dirname "$(realpath "$0")")/.."
BUILD_DIR="$ROOT/build"
SCRIPTS="$ROOT/scripts"

if [ ! -d "$BUILD_DIR" ]; then
    cmake -GNinja -DCMAKE_BUILD_TYPE=Debug -S "$ROOT" -B "$BUILD_DIR"
fi
cmake --build "$BUILD_DIR"

# Blender terrain pipeline (oghuzlands .blend -> chunked glb -> gltfpack ->
# jolt shapes -> splat/detail ktx2). Mtime-stamped: a no-op when the .blend,
# its UDIM tiles and detail textures are unchanged.
python3 "$SCRIPTS/blender-terrain.py"

# The game binary lives in build/c-game/; paks must land next to it.
export BIN_DIR="$BUILD_DIR/c-game"
export RELEASE=0
export SCRIPTS_TMP="$SCRIPTS/.tmp"
mkdir -p "$SCRIPTS_TMP"

cd "$ROOT/c-game"
. "$SCRIPTS/data.sh"

# engine pak (pak_0_engine: fonts, images, sound) ships next to the game paks
cd "$ROOT/c-engine"
. "$SCRIPTS/data.sh"
