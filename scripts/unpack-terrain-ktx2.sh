#!/bin/bash
# Unpack the ETC1S KTX2 terrain default textures to plain PNGs.
#
# The terrain textures ship as recomposited PNGs beside the .ktx2
# originals (the renderer reads the PNGs; the PNG is the exact decode of
# the same ETC1S data, kept for provenance).
#
# basisu -unpack writes per-format planes; the auto-composited *_rgba_* PNG
# has FLAT alpha (documented in docs/lessons.md), so RGB and A planes are
# recomposited here per mip, keeping only mip 0 (the runtime regenerates the
# mip chain via DiligentTools TextureLoader GenerateMips).
#
# Usage: scripts/unpack-terrain-ktx2.sh   (run from the repo root)
set -e

ROOT="$(dirname "$(realpath "$0")")/.."
BASISU=/home/enes/Projects/c/cpp-thirdparty/filament/git/build-linux/third_party/basisu/tnt/basisu
SRC="$ROOT/c-game/data/pak_1/images/terrain"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

TEXTURES=(
    grass_default/albedo
    grass_default/normal
    cliff_side_default/albedo
    cliff_side_default/normal
    snow_default/albedo
    sand_default/albedo
)

for t in "${TEXTURES[@]}"; do
    set="$SRC/$(dirname "$t")"
    name="$(basename "$t")"
    ktx2="$set/$name.ktx2"
    out="$set/$name.png"
    if [ -f "$out" ]; then
        echo "skip $out (exists)"
        continue
    fi
    echo "unpacking $ktx2"
    (cd "$WORK" && "$BASISU" -unpack "$ktx2" >/dev/null 2>&1)
    python3 - "$WORK" "$name" "$out" <<'PY'
import sys, os, glob
from PIL import Image
work, name, out = sys.argv[1], sys.argv[2], sys.argv[3]
rgb0 = sorted(glob.glob(f'{work}/{name}_unpacked_rgb_RGBA32_level_0_*'))
a0   = sorted(glob.glob(f'{work}/{name}_unpacked_a_RGBA32_0_0_*.png'))
assert rgb0 and a0, (rgb0, a0)
rgb = Image.open(rgb0[0]).convert('RGB')
a   = Image.open(a0[0]).convert('L')
rgba = rgb.convert('RGBA')
rgba.putalpha(a)
rgba.save(out)
print('wrote', out, rgba.size)
PY
done
