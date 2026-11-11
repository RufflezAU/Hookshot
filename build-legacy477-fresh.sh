#!/bin/sh
set -e
APP=/home/deck/Applications/hookshot/squashfs-root
LIB=$APP/usr/lib/hookshot
C=/home/deck/opencode-agents/hookshot-legacy
TOOLS=/home/deck/opencode-agents/hookshot-udmabuf/tools
PY=$APP/usr/python/bin/python3
export EMSDK="$LIB/emsdk"
export HOOKSHOT_SYSROOT="$LIB/sysroot"
export HOOKSHOT_FFMPEG="$LIB/ffmpeg"
export HOOKSHOT_SDL2="$LIB/libSDL2-2.0.so.0"
unset HOOKSHOT_VK_PREBUILT
export RX_CPM="$TOOLS/cpm"
export WGPU_NATIVE="$TOOLS/wgpu-native"
export HOOKSHOT_CXX_INCLUDE="$TOOLS/cxx/include"
export HOOKSHOT_CXX_LIB="$TOOLS/cxx/lib"
export HOOKSHOT_JOBS="${HOOKSHOT_JOBS:-3}"
cd "$C"
exec "$PY" -I nx2/make_game.py --nup /home/deck/Emulation/roms/switch2/oot.nup --out /home/deck/Games/Hookshot-legacy477 --language en-US --work /home/deck/.cache/hookshot-builder-legacy --jobs "$HOOKSHOT_JOBS"
