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

### 2026-11-09 late: the best build so far

**`legacy + #477 + samples-passed 4096`** (`Hookshot-legacy477b`): the old base with the
udmabuf/giveback/eviction patches, #477 (push-descriptor cache + tree-shadow mask) and #487's
samples-passed 4096. It removed the icon/object cycling; the fog/lighting flicker in the first long
shot outside remains.

The renderer's flash counter (`RX_VK_FLASH=1`, `RX_VK_SHOTS`, `RX_VK_SHOT_EVERY=1`,
`RX_VK_FLASH_REGION=8`) over the same 150 s route, same save:

| build | flashes |
|---|---|
| new base + SKIP_CODE + #955 (DECK) | 572 |
| legacy + #477 | 32 |
| legacy + #477 + samples-passed 4096 | 22 |

Note: the flash config only reaches the game if the launcher honours `HOS_CONFIG`; a launcher that
always copies `stable.env` silently measures nothing (that was a false "0 flashes" here).

Upstream by 2026-11-09 evening: `main` is 153 commits past `b234b43`, #957 (handle pointer,
fragment stages only) is merged, with #853/#840/#939 perf work and test switches for garbled
textures (`RX_VK_KEEP_UNFLUSHABLE=1`) and AMD stripes (`RX_VK_MARKERS_V205=1`). #477 is still closed
and unmerged; #798 (the icon flicker) is the open report for it.

### 2026-11-10: fog/lighting flicker — forks checked, our own path

Upstream and forks checked before writing anything (code search for `oot_visual_workaround.h`
returns 8 forks):

| source | what it has |
|---|---|
| upstream #1101 (white fog/overexposure, RADV) | open, no fix; the only tip is `SDL_VIDEODRIVER=x11`, reported useless under gamescope |
| #1004 (lighting tracking), #1011 (Hyrule Field lighting switching) | open, no fix |
| #1117 (NEAREST samplers → halos, checkerboard smoke) | open, a sampler-filtering lead |
| #914 (Deku Tree white haze) | closed into #1004 |
| Lichii / Vendichii / Bananza / Triforce / Elysium / Hookshot | same workaround as ours (shape-matched, fog 0.08 or 0.25, exposure 1.0 or 0.65) |
| selevet-eglantine | the only fork with an extra knob: the bloom exponent (c3+0x34) — ported here as `RX_VK_OOT_BLOOM` |

What the matrix on 477b showed (flash counter, same save):

| variant | flashes, same 4 s room window |
|---|---|
| `RX_VK_OOT_FOG=0.08` (default) | 68 |
| `RX_VK_OOT_FOG=0.25` | **14** |
| `0.5` / `1.0` | worse (26 / 38 in the room runs) |

The fog integration scale is the mechanism: at 0.08 the accumulation sits near the noise floor and
the haze flips per frame. `0.25` is set in the 477b config; the user confirms the flicker is "a lot
slower". It is our own value, not a borrowed fix.

Traps found while measuring:

- The flash counter **requires `RX_VK_SHOTS` + `RX_VK_SHOT_EVERY=1`**: it compares the read-back
  frame with its neighbours. Without the shots it logs nothing (a run with `RX_VK_FLASH=1` alone
  reports 0 flashes — not a fix).
- The per-draw dumps those settings cause were **4 GB** and slowed the runs to 6.8 fps (a room run
  is 27.6 fps). Use `RX_VK_FLASH_DUMPS=0` and clean `shots/` between runs.
- `vk: view of texture … not made` (16×, the same line as #914) is **our own dump path**: it makes
  per-layer views of a BC4 texture as a linear format for the snapshots. It is not the game's
  sampling and not the flicker.
- The sweep A/B cannot compare across runs: the camera sweep leaves different views on screen
  (594–1878 draws, 7.5–21 fps), so flash counts differ by the view, not the knob. Compare a fixed
  camera only (the static mode of `flicker_test.py`).
- `HOS_RES_SCALE` is the **VI fixed-path scale** (`host/nn_vi.c`), the final output scale — it does
  not change the 3D render targets, which is why it bought no frames.

`Hookshot-legacy477c` (built 2026-11-10) = 477b + the bloom exponent knob (`RX_VK_OOT_BLOOM`,
default 1 = the game's). The bloom is the other candidate for the white/overexposure flashes; the
A/B runs are in `flicker-results/` beside the build.

### 2026-11-10: the main base, static-view testing (flicker + fps)

The static mode of `flicker_test.py` (`--static`: the save's own camera, no input after the world
loads) repeats within 7 % (26 vs 28 flashes for the same config), so it is the fair A/B. The sweep
runs are not comparable (the camera leaves a different view on screen each time).

Main base (`Hookshot-latest5`, upstream `8b2e1d3` + SKIP_CODE, LOW_SPEC/MESH_OPT), same save:

| config | in-game flashes | fps |
|---|---|---|
| `OOT_VISUAL=1`, fog 0.2, `NO_DESC_REUSE=1` | **0** (only the boot/title light changes) | 22.0 |
| + `NO_PASS_HAZARDS=1` | **0** | 23.7 |
| + fog 0.08 / exposure 0.65 | 0 | 24.4 / 24.5 |
| `NOCULL=0` (culling on) | 800 (capped storm) | 14.0 |
| deep forest walk-out (2237 draws) | 800 (capped storm) | 9.4 |

- The room and the forest edge (216–685 draws) have **no in-game flashes** on the main base with
  `NO_DESC_REUSE=1` + `OOT_FOG=0.2`; the 26–38 the counter reports are the boot/title light changes
  (present < 750, before the world loads).
- The deep forest still storms: the white-fog overexposure of upstream #1101/#1004 (open, no fix).
  The game's TAAU network — the console's temporal average that hides this — cannot run on the Deck:
  RADV has no cooperative matrices ("matrix multiplies in software"), and the network would cost most
  of a frame. The stand-in (`taau_upscale.comp`) upscales without the temporal part.
- `RX_VK_TAAU_CLAMP=1` (the stand-in's per-pixel history bound, off by default) only dims flashes and
  smears motion; not a fix.
- FPS: the room/edge 22 → 23.7–24.5 with `NO_PASS_HAZARDS=1`. The frame is CPU-bound (GPU 8–12 ms);
  the replay costs ~0.03 ms/draw plus ~45 ms of fixed per-pass work (129 passes, 77 barriers), so the
  deep forest (2237 draws) is 9.4 fps no matter the GPU.
- Memory: textures sit at 1983 MB of the 2000 MB budget; no `Not enough memory`/`device lost` in any
  run. The flash counter's frame dumps are 170 MB+ per run — `RX_VK_FLASH_DUMPS=0` and cleaning
  `shots/` keeps them from piling up (they were 4 GB before).

`Hookshot-latest5/configs/stable.env` now carries the tested config: `OOT_VISUAL=1`,
`NO_DESC_REUSE=1`, `OOT_FOG=0.2`, `NO_PASS_HAZARDS=1`, LOW_SPEC/MESH_OPT.

## 2026-11-11 — the flicker was the frame-generation pacer, not a texture bug

The 477b build ships frame generation (`frame_generation=2`) and it was **on**. Its pacer laid the
made frames out on an estimate of the game's frame time with two limits too low for this frame
rate: frames 100 ms or more apart were ignored, and the estimate was clamped to 50 ms. At 9–15 fps
(64–110 ms frames) it kept a ~60 fps estimate, so a set of two went out **9–12 ms apart in bursts**
(the log's own histogram: `shown apart: 9-12 ms 348`, `353 late` of 365).

That is both the judder and the "icon flicker": the HUD was composited from stale and fresh frames
alternately. After the fix (`100 ms → 500 ms`, `50 ms → 250 ms` in `gpu/vk/vk_framegen.cpp`):

```
144 frames shown on time, 0 late;  shown apart: over 25 ms 143
```

Branch `deck/pacing-fix` on `RufflezAU/Hookshot`.

### Also ported: #1993 (RADV batched submits go out at an idle GPU)

Upstream `fab8cb37` — 3 lines plus a helper in `gpu/vk/vk_replay.cpp`. Upstream measured 33–41 →
46–61 fps in Kokiri on a 9070 XT with RADV; `RX_VK_BATCH_IDLE=0` reverts it.

### The build now uses the profile

The 477b build was compiled **without** `hookshot_pieces.prof`, `hot_funcs.txt` or the hot chunk.
Those are copied into `work-legacy` from the v2.1.0 builder and the tree rebuilt, so the hot
functions get the profile's flags and the link order.

### v2.1.0 measured on this Deck: do not ship it

Same house, same save: 6.5 fps against 15.7, GPU pinned at 200 MHz (power starvation from the CPU),
600–800 k vcore switches per 5 s against 33 k, and three `VK_ERROR_DEVICE_LOST` during world load.
Ruled out by measurement: memory limits, mesh native/fallback, `HOS_SPIN_US=0`, `HOS_NO_HANDOFF`,
`HOS_OCB_NORMAL`, `HOS_MW_WAKE=all`, `RX_VK_PRESENT_AHEAD=0`, `HOS_CORES=4`, 3-core pinning.

### The wall

The APU budget: CPU at 3.5 GHz leaves the GPU at ~1040 MHz of 1600. Fewer host cores clock the GPU
up (its work drops 50 → 18 ms) but starve the CPU; the frame rate falls either way. 30 real frames
would need ~2× on both at once. "Underwater" play is the game's Real time mode, which only keeps
normal speed down to **15 fps**.
