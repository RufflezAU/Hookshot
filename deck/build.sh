#!/bin/sh
# Build this branch with the Hookshot AppImage's toolchain, reusing an object cache if one is there.
#
#   APP=/path/to/squashfs-root NUP=/path/to/game.nup OUT=/path/to/out ./deck/build.sh
#
# HOOKSHOT_CXX_INCLUDE / HOOKSHOT_CXX_LIB: the AppImage's glibc 2.35 sysroot has no C++ standard
# library headers, so gpu/vk is compiled against libstdc++ 14's. Point them at the unpacked
# libstdc++-14-dev tree (Debian/Ubuntu) or set HOOKSHOT_VK_PREBUILT to the AppImage's
# hookshot_vk.so to skip building the back end (slower at runtime on the Deck: the prebuilt .so
# does not carry the udmabuf path).
set -e
: "${APP:?set APP to the extracted AppImage (squashfs-root)}"
: "${NUP:?set NUP to your .nup}"
OUT=${OUT:-$PWD/out}
JOBS=${JOBS:-3}

LIB="$APP/usr/lib/hookshot"
PY="$APP/usr/python/bin/python3"
[ -x "$PY" ] || PY=python3

export EMSDK="$LIB/emsdk"
export HOOKSHOT_SYSROOT="$LIB/sysroot"
export HOOKSHOT_FFMPEG="$LIB/ffmpeg"
export HOOKSHOT_SDL2="$LIB/libSDL2-2.0.so.0"
export RX_CPM="$LIB/cpm"
export WGPU_NATIVE="$LIB/wgpu-native"
export HOOKSHOT_JOBS="$JOBS"

cd "$(dirname "$0")/.."
exec "$PY" -I nx2/make_game.py \
  --nup "$NUP" \
  --out "$OUT" \
  --language en-US \
  --jobs "$JOBS"
