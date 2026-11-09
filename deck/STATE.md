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
- `'string.h' file not found` (third_party/zbic) or `'algorithm' file not found`
  (third_party/a64-kit/lift) on a fresh checkout: those two host helpers are compiled with the
  AppImage's clang, which cannot see the system's C/C++ headers here. Copy the prebuilt helpers from
  a tree that built before — `third_party/zbic/zbic.so` and `third_party/a64-kit/lift/liba64emit.so`
  plus its `.sha1` stamp (the stamp is the SHA-1 of `a64emit.cpp` + `suyu/arm64_to_c.h`, so a tree
  with the same sources accepts it) — or build them with the system compiler.

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
- **Latest main (`15ec28d`, vanilla): the prepare takes 166 s** (38,809 programs) and the RADV
  crash is intermittent, not tied to one commit. A bisect of `58af2c0..15ec28d` (two cold runs per
  commit) found no commit that reproduces it; the endpoint itself did not crash in three cold runs.
  Recorded on <https://github.com/NotRexed/Hookshot/issues/862>.
- **Latest main (`15ec28d`, vanilla) crashed once in the RADV driver at startup** on this Deck:
  `CRASH: exception 0xc0000005 ... at libvulkan_radeon.so+0x19cc52`, right after the mesh programs.
  So the upstream fixes (#705, #772) cannot be used here yet; tracked in
  <https://github.com/NotRexed/Hookshot/issues/862>.
- **The older base** (udmabuf branch + giveback, before this rebase) ran the forest at 10–12 fps
  with no crash but with lighting/texture flicker.
- **The VRAM mirror is off on integrated GPUs by design** (`mirror_on()`: on only for discrete
  cards). Forcing `RX_VK_MIRROR=1` on the Deck made it much worse: 5.6 fps, GPU 127 ms, VRAM
  6539/6144. Do not enable it.
- **PR #792's mesh fallback** (`RX_VK_FORCE_MESH_FALLBACK=1`) boots without the driver crash, but
  the first run compiles a 5.9 GB program cache and loading it takes the process past 11.5 GB RSS.
  Not practical for play as it stands.
- **With `RX_VK_PRECOMPILE=0` the latest main runs**: no prepare, no driver fault, the starting
  room at 40 fps headless (GPU 26.5 ms, VRAM 3116/5867). That is the current test build
  (`Hookshot-latest2`, `15ec28d` + the millisecond eviction). The cost is a 112 ms compile spike
  when a new shader first appears.

### Steam shortcut file

Editing `shortcuts.vdf` by hand: the file must end with a trailing **0x08** after the root object's
0x08. A 0x00 there makes Steam log `CSteamDoc::LoadShortcuts: failed to load shortcut file` and
delete the file on start — every non-Steam shortcut disappears. Keep a backup and only write it
while Steam is closed. It is kept as branch `deck/legacy`; build it the
  same way, with the same `deck/` settings (its `deck/` folder predates `configs/`, so copy
  `hookshot_env.txt` from this branch's `configs/stable.env`).

## Measure it the same way

The results table lives in `BENCHMARKS.md` (`benchmarks/*.json` are the raw runs). Add a row and the
JSON whenever a build or config changes, so the next person can choose on evidence.

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
