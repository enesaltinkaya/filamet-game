#!/bin/bash
set -e

TOOLS_DIR="$(dirname "$(realpath "$0")")"
TOOL_DIR="$TOOLS_DIR"
THIRDPARTY="/home/enes/Projects/c/cpp-thirdparty"
SRC="$TOOL_DIR/main.cpp"
OUT="$TOOL_DIR/terrain-chunker"

if [ "$OUT" -nt "$SRC" ] 2>/dev/null; then
    exit 0
fi

echo "building terrain-chunker..."
clang++ -std=c++17 -O2 -DNDEBUG \
    -I"$THIRDPARTY/cgltf/git" \
    -I"$THIRDPARTY/meshoptimizer/git/src" \
    -o "$OUT" \
    "$SRC" \
    "$THIRDPARTY/meshoptimizer/git/build-linux/libmeshoptimizer.a" \
    -lm

echo "terrain-chunker built: $OUT"
