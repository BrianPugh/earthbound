#!/usr/bin/env bash
# Asset setup for the self-hosted CI jobs (ci.yml, "Asset jobs").
#
# Needs EB_DONOR_ROM (the US retail ROM) and, for the regress jobs, EB_SAVES_DIR
# (a copy of playtest/saves/). Creates .venv with ebtools, extracts asm/bin and
# src/assets from the ROM, and links the saves into playtest/saves. The runner
# also needs uv, cmake, SDL2, clang, arm-none-eabi-gcc, and ca65/spcasm for `make`.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"

: "${EB_DONOR_ROM:?set EB_DONOR_ROM to the donor ROM path on the runner}"
ln -sf "$EB_DONOR_ROM" earthbound.sfc
if [[ -n "${EB_SAVES_DIR:-}" ]]; then
    ln -sfn "$EB_SAVES_DIR" playtest/saves
fi

uv venv -q --allow-existing .venv
uv pip install -q -e .
uv run --no-sync ebtools extract earthbound.yml earthbound.sfc
