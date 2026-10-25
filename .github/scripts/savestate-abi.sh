#!/usr/bin/env bash
# Compile src/core/state_dump.c for the ILP32 embedded targets so its ABI
# _Static_asserts (the savestate section sizes must match the 64-bit desktop)
# are checked: the Pico (cortex-m0plus, 240x240) and the Game & Watch (cortex-m7,
# 320x240). Needs arm-none-eabi-gcc and the tamp submodule, no donor ROM:
# state_dump.c uses no item ids, so an empty items_generated.h stands in for the
# ROM-derived one.
#
#   .github/scripts/savestate-abi.sh
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
stub="$(mktemp -d)"
trap 'rm -rf "$stub"' EXIT
: >"$stub/items_generated.h"

for spec in "cortex-m0plus 240 240" "cortex-m7 320 240"; do
    read -r cpu w h <<<"$spec"
    arm-none-eabi-gcc -mthumb -mcpu="$cpu" -std=c11 -fsyntax-only -DEB_EMBEDDED=1 \
        -DEB_VIEWPORT_WIDTH="$w" -DEB_VIEWPORT_HEIGHT="$h" \
        -I"$root/src" -I"$root/src/include" -I"$root/src/vendor/tamp/tamp/_c_src" -I"$stub" \
        "$root/src/core/state_dump.c"
    echo "state_dump.c ABI asserts hold on $cpu (${w}x${h})"
done
