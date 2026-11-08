# State — what works, what does not, how to reproduce it

Last updated 2026-11-09. If you are an agent picking this up, read this file first, then
`README.md`, then `configs/README.md`.

## Source

- Branch `deck/steamdeck`, based on upstream `main` (commit `58af2c0` when this was written).
- Only two changes on top of main: the millisecond texture eviction (`gpu/vk/vk_texture.cpp`,
  `.h`) and the `nx2/build.py` libstdc++ include fix. See `README.md`.

## Build it exactly

```sh
APP=/path/to/squashfs-root \            # the extracted Hookshot AppImage
NUP=/path/to/game.nup \                 # your own dump, made with Hookshot Builder
OUT=/path/to/output \
HOOKSHOT_CXX_INCLUDE=/path/to/libstdc++-14/usr/include \
HOOKSHOT_CXX_LIB=/path/to/libstdc++-14/usr/lib/gcc/x86_64-linux-gnu/14 \
./deck/build.sh
```

Expected output: `hookshot` (~486 MB), `hookshot_vk.so` (~6 MB), `hookshot_shaders.bin`
(~490 MB), `romfs/`, `image.bin`. The first build translates the game (20–70 min on a Deck);
later builds reuse the object cache and take a few minutes.

The two failure modes seen here, and their fixes:

- `'algorithm' file not found` — `HOOKSHOT_CXX_INCLUDE` / `HOOKSHOT_CXX_LIB` not set (or
  `build.py` started before the fix was in the tree).
- `native NVN: memory pool without storage (size 0x98000000)` — `HOS_MEMORY_MB` set below the
  default. Do not set it; the game expects the full 9808 MB arrangement.

## Run it exactly

Copy `deck/hookshot-play.sh`, `deck/hookshot_env.txt`, `deck/hookshot_options.ini` and
`deck/configs/` next to the built `hookshot`, then:

```sh
HOS_CONFIG=stable ./hookshot-play.sh
```

`/dev/udmabuf` must exist (`sudo mknod /dev/udmabuf c 10 258` if not). The log is
`hookshot_log.txt` in the game folder; the previous run is `.old.txt`.

## Current state (2026-11-09)

- **This build (main + eviction) boots, loads a save, and runs the starting room at ~50 fps in
  headless runs** (GPU 15.5–16.9 ms), VRAM ~3.4 of 5.8 GB.
- **It crashes/hangs at the room-door transition in a windowed run.** The log ends in the texture
  upload-over-finalized path after very slow uploads (372–630 ms), with no OOM, no device lost and
  no `CRASH:` line; Steam reports exit code 0. The same save is stable on the older udmabuf-base
  build. `RX_VK_KEEP_FINALIZED_MB=0` (config `keepoff`) did not change it. Open: is it main's
  storage-buffer/finalized-texture path, or the program-cache load (5.4 GB), or our eviction?
- **The older base** (udmabuf branch + giveback, before this rebase) ran the forest at 10–12 fps
  with no crash but with lighting/texture flicker. It is not in this branch.

## Measure it the same way

`test/ab_harness.py` runs a build headless, steers it into the save with `HOS_INPUT_LIVE`, walks,
and records VRAM/textures/GPU/fps/faults per run:

```sh
HOS_GAME=/path/to/game HOS_LAUNCHER=/path/to/game/hookshot-play.sh \
HOS_CACHE_SEED= python3 deck/test/ab_harness.py single --name test --seconds 240
python3 deck/test/ab_report.py
```

A run is only comparable if it reached the world (`max_draws` > 400) and did not end early
(`mem_kill`, `rss_kill`, `exited_early`). The harness kills the game if `MemAvailable` falls below
900 MB or its RSS passes 11.5 GB, so the agent's machine survives; it also sets `oom_score_adj=1000`
on the game.

## Log lines worth grepping

```
a frame: N draws                16 = movie, 61 = title, 216 = file select, 400+ = world
presents, N fps                 presented rate
GPU time N ms a frame           GPU cost
VRAM: N MB used of N MB allowed peak against the driver's allowance
textures N MB (budget N MB)     the texture budget
imports: ... given back (N MB)  the retired-import giveback
upload over a drawn image ...   the finalized-texture path
slow upload N ms                a stall worth investigating
device lost / Not enough memory the two memory failures
```
