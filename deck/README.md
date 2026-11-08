# Deck branch — Steam Deck OLED

This branch is the Steam Deck OLED path for Hookshot. It is a normal build of the project, made
from **your own game dump**, with three changes that matter on a 14 GB unified-memory APU:

1. **Guest memory as udmabuf dma-bufs** — `linux/udmabuf-guest-memory` (PR #448, AzleKayn).
   Instead of handing RADV a host pointer and letting amdgpu re-check every page at each
   `DRM_IOCTL_AMDGPU_CS`, the guest window is a memfd and its pages are imported as
   `/dev/udmabuf` dma-bufs. On the Deck this takes the world's `vkQueueSubmit` from 80+ ms to
   a few ms and is what makes the world reachable at all.
2. **Retired imports given back** — PR #446 (AzleKayn, merged upstream). Retired pools used to
   keep their imports for the whole session; on a 16 GB machine the import set only grew.
3. **Two local fixes**:
   - the giveback frees a pool's extra dma-buf pieces too (`Pool::pieces`), which upstream's
     version does not need because it only has the host-pointer path;
   - the texture eviction's age is wall-clock milliseconds, not presents. 60 presents is one
     second at 60 fps but **fifteen seconds at 4 fps**, and on a slow device VRAM ran past the
     driver's budget between sweeps (textures peaked at 4005 MB against a 2200 MB budget), the
     driver paged VRAM to system memory, and the compositor starved.

## What is not in this branch

No game files, no built binaries, no caches. The build embeds the translated game and is made
locally from a `.nup` you produce from your own dump with Hookshot Builder. Nothing here
redistributes any part of the game.

## Build

You need:

- the Hookshot AppImage, extracted (`squashfs-root/usr/lib/hookshot` holds the toolchain:
  clang, the glibc 2.35 sysroot, FFmpeg, SDL2);
- a `.nup` of your own dump;
- libstdc++ 14 headers (the AppImage's sysroot has no C++ standard library headers). On Debian
  or Ubuntu they come from `libstdc++-14-dev`; point `HOOKSHOT_CXX_INCLUDE` and
  `HOOKSHOT_CXX_LIB` at the unpacked tree. The build links libstdc++ statically, so the game
  folder does not depend on the host's version.

```sh
APP=/path/to/squashfs-root \
NUP=/path/to/game.nup \
OUT=/path/to/output \
HOOKSHOT_CXX_INCLUDE=/path/to/libstdc++-14/usr/include \
HOOKSHOT_CXX_LIB=/path/to/libstdc++-14/usr/lib/gcc/x86_64-linux-gnu/14 \
./deck/build.sh
```

The first build translates the game (5–20 minutes depending on the machine); later builds reuse
the object cache and take a minute or two.

## Run

Copy `deck/hookshot-play.sh`, `deck/hookshot_env.txt` and `deck/hookshot_options.ini` next to the
built `hookshot` binary, then run the script (or point a non-Steam shortcut at it).

`/dev/udmabuf` must exist. The kernel option is `CONFIG_UDMABUF=y`; the device node is usually
created at boot. If it is missing:

```sh
sudo mknod /dev/udmabuf c 10 258
sudo chmod 660 /dev/udmabuf
```

The launcher only forces `HOS_GUEST_MEMFD=1` when the sysfs parameter is hidden *and*
`/dev/udmabuf` is writable — a forced memfd without a working udmabuf has no import at all.

## Settings on the Deck

`hookshot_options.ini`: 50 % resolution, Custom preset (so the budget below is the one used),
6x frame generation, handheld off.
`hookshot_env.txt`: the texture budget and eviction knobs. They are a starting point measured on
one Deck; tune the budget to your own tolerance. The two that matter:

- `RX_VK_VRAM_MB` — the texture budget. 1200 MB was the only value that stayed under the
  driver's allowance in an overnight A/B (peak 6099 of 6144 MB); 1500 MB was 83 MB over.
- `RX_VK_EVICT_MIN_MS` — how long a texture may go unused before it can be evicted (default
  1000 ms). Lower means less VRAM and more re-uploads.

## Known issues

- The Steam UI can restart under the game when the GPU runs out of budget (open upstream
  reports: #581, #604). Keeping the texture budget below the driver's allowance is the
  mitigation.
- Movies are audio-desynced below 60 fps in the upstream clock; not addressed here.
- Frame generation has no movie gate upstream; 6x also multiplies movie frames.

## Credit

The udmabuf path and the retired-import giveback are AzleKayn's (PR #448 and PR #446). This
branch only adds the Deck toolchain fixes and the two changes above.
