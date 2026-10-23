#!/bin/sh
# Build the snes9x libretro core used by playtest/emu.py (the reference
# emulator for checking what the original game does).
#
#   playtest/emu/build.sh            # clone (once) + build
#   SNES9X_REV=<sha> playtest/emu/build.sh
#
# Output: playtest/emu/build/snes9x_libretro.{dylib,so} (gitignored).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
BUILD="$HERE/build"
SRC="$BUILD/snes9x"
REPO=https://github.com/snes9xgit/snes9x.git
# Tested revision (2026-09-24); override with SNES9X_REV
REV=${SNES9X_REV:-1bcc369e89f08243e0a462882fb1f3e42e51de3a}

mkdir -p "$BUILD"
if [ ! -d "$SRC/.git" ]; then
    git clone --filter=blob:none "$REPO" "$SRC"
fi
git -C "$SRC" fetch --quiet origin "$REV" 2>/dev/null || true
git -C "$SRC" checkout --quiet "$REV"

case "$(uname -s)" in
Darwin)
    PLATFORM=osx
    EXT=dylib
    ;;
*)
    PLATFORM=unix
    EXT=so
    ;;
esac
ARCH=$(uname -m)
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

make -C "$SRC/libretro" -j"$JOBS" platform="$PLATFORM" arch="$ARCH"
cp "$SRC/libretro/snes9x_libretro.$EXT" "$BUILD/snes9x_libretro.$EXT"
echo "built $BUILD/snes9x_libretro.$EXT"
