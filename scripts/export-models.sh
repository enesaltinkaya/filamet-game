#!/bin/bash
# Model exporter (port of the old engine's 1-blender-scene.sh, minus jolt):
#   .blend -> glb (blender, scripts/blender-scene.py) -> packed glb (gltfpack)
#          -> zstd -10 (or raw, convertModel <blend> 1) -> c-game/data/pak_1/models/
# The loader sniffs the zstd magic, so raw .glb and .zstd both load. Raw is
# opt-in: for test2 it trades ~60 ms of load-critical-path decode for +84 MB
# of pak size — we kept the compression (plans/world-load-2.md phase B,
# reverted on user preference).
# Repack after running: ./scripts/build.sh (data.sh rebuilds pak_1.pak when
# its content md5 changed).
set -e

ROOT="$(dirname "$(realpath "$0")")/.."
ASSETS_DIR="/home/enes/Projects/assets"
GLTFPACK="/home/enes/Projects/c/cpp-thirdparty/meshoptimizer/git/build-linux/gltfpack"
OUT_DIR="$ROOT/c-game/data/pak_1/models"
SCRIPTS_TMP="$ROOT/scripts/.tmp"
STAGE_DIR="$SCRIPTS_TMP/models"

# gltfpack flags: -noq (no quantization — fp32 positions/uvs/normals AND
# animation TRS), keep names/extras/materials, 30 Hz animation resample.
# Animation data must be fp32: Diligent's GLTF loader only reads fp32 sampler
# outputs (its int16 path is a compiled-out VERIFY — gltfpack's default
# animation quantization loads as garbage floats and NaNs out the skeleton).
# Vertex quantization would be fine (Diligent converts normalized ints), but
# gltfpack offers no way to keep it while disabling animation quantization.
# NOTE: no -cc (meshopt buffer compression) — the old engine's cgltf decoded
# EXT_meshopt_compression, but Diligent's GLTF loader has no meshopt support
# and reads compressed buffer views as garbage (bounding boxes and vertices).
# The zstd pass below keeps the shipped files small instead.
GLTFPACK_FLAGS=(-noq -ke -kn -kv -km -af 30)

convertModel() {
    local blendFile="$1"
    local raw="${2:-0}"
    local ext="zstd"
    [ "$raw" = "1" ] && ext="glb"
    local name
    name="$(basename "${blendFile}")"
    name="${name%.blend}"
    local stamp="$SCRIPTS_TMP/${name}.blend.stamp"
    local mtime
    mtime="$(date -r "$blendFile" "+%Y%m%d%H%M%S")"

    if [ -f "$OUT_DIR/${name}.${ext}" ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$mtime" ]; then
        echo "up to date: ${name}.${ext}"
        return
    fi

    if [ ! -f "$blendFile" ]; then
        echo "missing blend file: $blendFile" >&2
        return 1
    fi

    mkdir -p "$OUT_DIR" "$STAGE_DIR"
    local glb="$STAGE_DIR/${name}.glb"

    echo "#############################################"
    echo -n "blend -> glb ${name}... "
    local log="$STAGE_DIR/${name}.blend.log"
    if ! blender "$blendFile" --background --python "$ROOT/scripts/blender-scene.py" -- "$glb" > "$log" 2>&1; then
        echo "FAILED (log: $log)" >&2
        tail -n 20 "$log" >&2
        return 1
    fi
    echo "$(du -sh "$glb" | cut -f1)"

    # Standardize the character hierarchy (identity armature, metre-space
    # bones) so standard glTF renderers place it correctly
    # — Blender's exporter leaves the cm-authored armature transform on the
    # node, which it then applies twice. No-op for assets without a
    # transformed armature. See scripts/gltf-standardize.py.
    echo -n "standardize... "
    local std="$STAGE_DIR/${name}.std.glb"
    if ! python3 "$ROOT/scripts/gltf-standardize.py" "$glb" "$std" >> "$log" 2>&1; then
        echo "FAILED (log: $log)" >&2
        tail -n 20 "$log" >&2
        return 1
    fi
    echo ok
    mv "$std" "$glb"

    echo -n "gltfpack... "
    export KTX_GEN_MIPMAP=1
    "$GLTFPACK" "${GLTFPACK_FLAGS[@]}" -i "$glb" -o "$STAGE_DIR/${name}.pack.glb"
    rm -f "$glb"
    mv "$STAGE_DIR/${name}.pack.glb" "$glb"
    echo "$(du -sh "$glb" | cut -f1)"

    # gltfpack hard-codes animation rotation keys as int16 normalized snorm
    # (no flag changes that); Diligent's loader only reads fp32 sampler
    # outputs — rewrite them (see scripts/gltf-rotation-f32.py)
    echo -n "rotation f32... "
    local f32="$STAGE_DIR/${name}.f32.glb"
    if ! python3 "$ROOT/scripts/gltf-rotation-f32.py" "$glb" "$f32" >> "$log" 2>&1; then
        echo "FAILED (log: $log)"
        tail -n 20 "$log"
        return 1
    fi
    echo ok
    mv "$f32" "$glb"

    # Embedded raster textures -> external toktx ktx2 (raw RGBA8 + KTX_SS_ZSTD,
    # the terrain recipe) under images/models/<name>/ + URI rewrite: libpng
    # decoding of the embedded PNGs was the bulk of the props/eve load time
    # (plans/world-load.md phase 2). See scripts/gltf-extract-textures.py.
    echo -n "textures ktx2... "
    local imgDir="$ROOT/c-game/data/pak_1/images/models/${name}"
    rm -rf "$imgDir"
    if ! python3 "$ROOT/scripts/gltf-extract-textures.py" "$name" "$imgDir" "$glb" "$glb" >> "$log" 2>&1; then
        echo "FAILED (log: $log)" >&2
        tail -n 20 "$log" >&2
        return 1
    fi
    echo "$(du -sh "$imgDir" 2>/dev/null | cut -f1 || echo none)"

    echo -n "jolt shapes... "
    JOLT_SHAPE_BUILDER="$ROOT/tools/jolt-shape-builder/jolt-shape-builder"
    local joltFile="$STAGE_DIR/${name}.jolt"
    rm -f "$joltFile"
    "$JOLT_SHAPE_BUILDER" "$glb" "$joltFile" >> "$log" 2>&1
    if [ -f "$joltFile" ]; then
        zstd -q -10 --rm -f "$joltFile"
        mv "${joltFile}.zst" "$OUT_DIR/${name}.jolt.zstd"
        echo "$(du -sh "$OUT_DIR/${name}.jolt.zstd" | cut -f1)"
    else
        echo "none"
    fi

    if [ "$raw" = "1" ]; then
        echo -n "raw glb... "
        rm -f "$OUT_DIR/${name}.zstd"
        cp "$glb" "$OUT_DIR/${name}.glb"
        echo "$(du -sh "$OUT_DIR/${name}.glb" | cut -f1)"
    else
        echo -n "zstd... "
        rm -f "$OUT_DIR/${name}.glb"
        zstd -q -10 --rm -f "$glb"
        mv "${glb}.zst" "$OUT_DIR/${name}.zstd"
        echo "$(du -sh "$OUT_DIR/${name}.zstd" | cut -f1)"
    fi

    echo "$mtime" > "$stamp"
}

mkdir -p "$SCRIPTS_TMP"

convertModel "$ASSETS_DIR/Scenes/Characters/eve.blend"
convertModel "$ASSETS_DIR/Scenes/Characters/animations.blend"
convertModel "$ASSETS_DIR/Scenes/test2.blend"
