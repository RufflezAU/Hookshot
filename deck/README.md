# Deck branch — Steam Deck OLED

This branch is upstream `main` with the Steam Deck OLED changes on top. It is a normal build of the
project, made from **your own game dump**; nothing here redistributes any part of the game.

## What is different from main

Only two things, both small:

1. **The texture eviction's age is wall-clock milliseconds, not presents** (`gpu/vk/vk_texture.cpp`,
   `.h`). `texture_sweep` evicts what has been unused for 60 presents over budget and 120 under it.
   That is 1–2 s at 60 fps, but **10–30 s at 4–6 fps** — and a Deck runs this title's world at
   4–12 fps. Between sweeps the texture set grew far past the budget (4.0 GB against a 1.2 GB
   budget), the driver paged VRAM to system memory, GPU time went 26 → 69 ms and submissions to
   100–183 ms, and the compositor starved. `Texture::last_ms`, the sweep ordered by it, and
   `RX_VK_EVICT_MIN_MS` (default 1000; sweep 2000; the rendered keep 10 s / 60 s). At 60 fps the
   numbers are the same as before.
2. **`nx2/build.py` compiles `gpu/vk` against libstdc++ 14's headers** (`HOOKSHOT_CXX_INCLUDE` /
   `HOOKSHOT_CXX_LIB`). The AppImage's glibc 2.35 sysroot carries no C++ standard library headers;
   without this the Linux build stops at `'algorithm' file not found`. The link stays
   `-static-libstdc++`, so the game folder does not depend on the host's version.

Everything else that matters on this machine is already in main: the udmabuf guest-memory path
(PR #448), the retired-import giveback (PR #446/#479), the finalized-texture keep that fixes the
terrain streaks (#593), and the storage-buffer/lighting fixes.

## Build

You need:

- the Hookshot AppImage, extracted (`squashfs-root/usr/lib/hookshot` holds the toolchain: clang,
  the glibc 2.35 sysroot, FFmpeg, SDL2);
- a `.nup` of your own dump;
- libstdc++ 14 headers. On Debian/Ubuntu they come from `libstdc++-14-dev`; point
  `HOOKSHOT_CXX_INCLUDE` and `HOOKSHOT_CXX_LIB` at the unpacked tree.

```sh
APP=/path/to/squashfs-root \
NUP=/path/to/game.nup \
OUT=/path/to/output \
HOOKSHOT_CXX_INCLUDE=/path/to/libstdc++-14/usr/include \
HOOKSHOT_CXX_LIB=/path/to/libstdc++-14/usr/lib/gcc/x86_64-linux-gnu/14 \
./deck/build.sh
```

The first build translates the game (20–70 minutes on a Deck); later builds reuse the object cache.

## Run

Copy `deck/hookshot-play.sh`, `deck/hookshot_env.txt`, `deck/hookshot_options.ini` and
`deck/configs/` next to the built `hookshot` binary, then run the script (or point a non-Steam
shortcut at it):

```sh
HOS_CONFIG=stable ./hookshot-play.sh    # or fps, or keepoff
```

`STATE.md` is the current known-good/known-bad state and the exact build/run steps; `configs/`
lists every measured setting; `BENCHMARKS.md` has the measured fps/GPU/VRAM per build and config,
with the raw `ab_harness.py` JSON in `benchmarks/`. Read `STATE.md` before changing anything.

`/dev/udmabuf` must exist. The kernel option is `CONFIG_UDMABUF=y`; the node is usually created at
boot. If it is missing:

```sh
sudo mknod /dev/udmabuf c 10 258
sudo chmod 660 /dev/udmabuf
```

The launcher only forces `HOS_GUEST_MEMFD=1` when the sysfs parameter is hidden *and* `/dev/udmabuf`
is writable — a forced memfd without a working udmabuf has no import at all.

## Settings on the Deck

`hookshot_options.ini`: 50 % resolution, Custom preset (so the budget below is the one used), 6x
frame generation, handheld off.

`hookshot_env.txt`, measured over 24 four-minute runs on one Deck:

- `RX_VK_VRAM_MB=1200` — the only texture budget that stayed under the driver's allowance
  (peak 6099 of 6144 MB). 1500 MB was 83 MB over; 2200 MB went over badly.
- `RX_VK_TEX_MAX_MB=128` — 256 MB was clearly worse (+211 MB over).
- `RX_VK_EVICT_MIN_MS=1500` — a little less churn than 1000 at 4–12 fps.
- `RX_VK_TEX_MAX_DIM=1024`, `RX_VK_MIRROR_MB=192`, `RX_VK_CHECKPOINTS=1`.

The texture budget is a weak lever: 1200 vs 1500 moves peak VRAM by only ~128 MB, because the
game's buffer pools and imports fill it, not textures. No setting gets comfortably under the
allowance; the rest has to come from the buffer side or the resolution.

## Known issues

- The Steam UI can restart under the game when the GPU runs out of budget (upstream #581, #604).
- Movies are audio-desynced below 60 fps in the upstream clock.
- Frame generation has no movie gate upstream; 6x also multiplies movie frames.
- Visual flicker on AMD: #593 and the storage-buffer fixes are in main; #763/#774 (crushed
  shadows, dark models) are still open upstream.

## Credit

The udmabuf path, the import giveback and the finalized-texture keep are upstream's (AzleKayn and
the contributors). This branch only adds the two changes above and the Deck's settings.
