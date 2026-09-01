#!/bin/bash
set -e

ROOT="$(dirname "$(realpath "$0")")/.."
clear

export ENGINE_DEBUG=1

"$ROOT/scripts/build.sh"

export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
"$ROOT/build/c-game/c-game"
