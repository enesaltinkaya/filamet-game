#!/bin/bash
set -e

ROOT="$(dirname "$(realpath "$0")")/.."
clear

export ENGINE_DEBUG=1

"$ROOT/scripts/build.sh"

if [[ $1 == "renderdoc" ]]; then
    # App-side API (TriggerCapture) via preload; hooks come from the implicit
    # Vulkan layer (required: Diligent's Vulkan backend loads Vulkan entry
    # points via volk, which plain symbol interposition cannot intercept).
    # Both map the same library file, so only one RenderDoc instance exists.
    export LD_PRELOAD=/home/enes/Apps/renderdoc/build/lib/librenderdoc.so
    export ENABLE_VULKAN_RENDERDOC_CAPTURE=1
    export SDL_VIDEO_BACKEND=x11
fi

export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
export ENGINE_HIDDEN_WINDOW=0

"$ROOT/build/c-game/c-game" "${@:2}"
