#!/bin/bash
# Model exporter (port of the old engine's 1-blender-scene.sh, minus jolt):
#   .blend -> glb (blender, scripts/blender-scene.py) -> packed glb (gltfpack)
#          -> zstd -10 -> c-game/data/pak_1/models/<name>.zstd
# The game reads models/<name>.zstd from pak_1.pak; the loader sniffs the
# zstd magic, so .zstd is purely a naming choice (no more misleading .dat).
# Repack after running: ./scripts/build.sh (data.sh rebuilds pak_1.pak when
# its content md5 changed).
set -e

ROOT="$(dirname "$(realpath "$0")")/.."
ASSETS_DIR="/home/enes/Projects/assets"
GLTFPACK="/home/enes/Projects/c/cpp-thirdparty/meshoptimizer/git/build-linux/gltfpack"
OUT_DIR="$ROOT/c-game/data/pak_1/models"
SCRIPTS_TMP="$ROOT/scripts/.tmp"
STAGE_DIR="$SCRIPTS_TMP/models"

# gltfpack flags: identical to the old engine's scene export so files match
# what the old pak shipped. -cc = meshopt buffer compression (not KTX2),
# float positions, 16-bit uv/normals, keep names/extras/materials, 30 Hz
# animation resample with the old engine's quantization.
GLTFPACK_FLAGS=(-vpf -cc -vt 16 -vn 16 -ke -kn -kv -km -af 30 -at 24 -as 24 -ar 16)

convertModel() {
    local blendFile="$1"
    local name
    name="$(basename "${blendFile}")"
    name="${name%.blend}"
    local stamp="$SCRIPTS_TMP/${name}.blend.stamp"
    local mtime
    mtime="$(date -r "$blendFile" "+%Y%m%d%H%M%S")"

    if [ -f "$OUT_DIR/${name}.zstd" ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$mtime" ]; then
        echo "up to date: ${name}.zstd"
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
    # bones) so standard glTF renderers (Filament/gltfio) place it correctly
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

    echo -n "singlekey fix... "
    local skf="$STAGE_DIR/${name}.skf.glb"
    if ! python3 "$ROOT/scripts/gltf-singlekey-fix.py" "$glb" "$skf" >> "$log" 2>&1; then
        echo "FAILED (log: $log)"
        tail -n 20 "$log"
        return 1
    fi
    echo ok
    mv "$skf" "$glb"

    echo -n "gltfpack... "
    export KTX_GEN_MIPMAP=1
    "$GLTFPACK" "${GLTFPACK_FLAGS[@]}" -i "$glb" -o "$STAGE_DIR/${name}.pack.glb"
    rm -f "$glb"
    mv "$STAGE_DIR/${name}.pack.glb" "$glb"
    echo "$(du -sh "$glb" | cut -f1)"

    echo -n "zstd... "
    zstd -q -10 --rm -f "$glb"
    mv "${glb}.zst" "$OUT_DIR/${name}.zstd"
    echo "$(du -sh "$OUT_DIR/${name}.zstd" | cut -f1)"

    echo "$mtime" > "$stamp"
}

mkdir -p "$SCRIPTS_TMP"

convertModel "$ASSETS_DIR/Scenes/Characters/eve.blend"
convertModel "$ASSETS_DIR/Scenes/Characters/animations.blend"
