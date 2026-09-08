#!/bin/bash
# Packs all data/pak_* directories into zip paks next to the game binary.
# Run with cwd = the subproject that owns the data/ dir (c-game, ...).
# Env: BIN_DIR     target binary directory (default: <root>/build/c-game)
#      RELEASE     1 = rebuild paks (default 0)
#      SCRIPTS_TMP md5 cache dir, avoids rezipping unchanged paks

set -e

BIN_DIR="${BIN_DIR:-$(realpath build/c-game)}"
DATA_DIR="${BIN_DIR}/data"
RELEASE="${RELEASE:-0}"
SCRIPTS_TMP="${SCRIPTS_TMP:-$(dirname "$(realpath "$0")")/.tmp}"

zipPak() {
    local dir="$1"
    local md5
    md5=$(tar -cf - "$dir" | md5sum | cut -d' ' -f1)
    local md5_file="${SCRIPTS_TMP}/$dir"

    local do_zip=0
    if [ "$RELEASE" == "1" ] || [ ! -f "$md5_file" ] || [ "$(cat "$md5_file" 2>/dev/null)" != "$md5" ]; then
        do_zip=1
        echo "$md5" > "$md5_file"
    fi
    [ -f "${DATA_DIR}/$dir.pak" ] || do_zip=1

    if [ $do_zip == 1 ]; then
        rm -f "${DATA_DIR}/$dir.pak"
        (cd "$dir" && zip -q -r -0 "${DATA_DIR}/$dir.pak" .) # store only: uncompressed chunk reads
        local size
        size=$(du -sh "${DATA_DIR}/$dir.pak" | cut -f1)
        echo "zipping $dir size:${size}"
    fi
}

[ -d data ] && [ -n "$(find data -type d -name 'pak_*' -print -quit)" ] || exit 0

mkdir -p "${DATA_DIR}" "${SCRIPTS_TMP}"

cd data
for dir in $(find ./ -type d -name "pak_*" | sed 's|^\./||'); do
    zipPak "$dir"
done
