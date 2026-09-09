#!/bin/bash
set -e

TOOLS_DIR="$(dirname "$(realpath "$0")")"
TOOL_DIR="$TOOLS_DIR"
THIRDPARTY="/home/enes/Projects/c/cpp-thirdparty"
SRC="$TOOL_DIR/main.cpp"
OUT="$TOOL_DIR/jolt-shape-builder"

if [ "$OUT" -nt "$SRC" ] 2>/dev/null; then
    exit 0
fi

echo "building jolt-shape-builder..."
clang++ -std=c++17 -O2 -DNDEBUG \
    -I"$THIRDPARTY/cgltf/git" \
    -I"$THIRDPARTY/meshoptimizer/git/src" \
    -I"$THIRDPARTY/jolt/git" \
    -o "$OUT" \
    "$SRC" \
    "$THIRDPARTY/jolt/git/build-linux/libJolt.a" \
    "$THIRDPARTY/meshoptimizer/git/build-linux/libmeshoptimizer.a" \
    -lpthread -lm

echo "jolt-shape-builder built: $OUT"
