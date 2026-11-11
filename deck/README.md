# Hookshot — Steam Deck fork

This fork exists for one device: the **Steam Deck OLED** (RADV VANGOGH, RDNA2, 16 GB shared
LPDDR5, 4-core Zen 2). It is deliberately **not** a copy of upstream `main`. It tracks a Deck
branch that is several hundred commits behind, and that is a decision, not neglect.

## Why this fork is behind main

The newer `main` introduces a texture-handle corruption that this device shows constantly: random
heart, button and item icons flash over the picture, and surfaces take each other's contents
(upstream #798, folded into the open AMD tracking issue #997 — still unfixed). Every build made from
that line glitches in Link's house within seconds. It is a wrong-handle binding in the newer
handle-pointer code, not a setting, and the forks' partial fixes (Vendichii/Fritchii v4) do not
cover it.

The **legacy base + PR #477** is the only line that is clean on this hardware. #477 is the
push-descriptor cache marked stale after the emulator's own pushes plus the tree-shadow mask
stabilisation. Upstream closed #477 without merging it. It is the core of this fork.

## What is in this build

- Legacy Deck base: udmabuf guest memory, retired-import giveback, time-based texture eviction
- **PR #477** — push-descriptor cache invalidation + tree-shadow mask stabilisation (no icon cycling)
- Samples-passed reports read 4096 (#487's sunlight flicker)
- Deck memory tuning: `RX_VK_VRAM_MB=2000`, `RX_VK_TEX_MAX_MB=256`, `RX_VK_TEX_MAX_DIM=1024`,
  `RX_VK_EVICT_MIN_MS=3000`
- `RX_VK_OOT_VISUAL=1` + `RX_VK_OOT_FOG=0.2` — the effects work; the fog scale is our own value
  (0.08 thins the haze, 0.25+ makes the forest white; 0.2 measured lowest flicker)
- `RX_VK_NO_DESC_REUSE=1`, `RX_VK_NO_PASS_HAZARDS=1`
- Handheld 720p, 50 % render, FSR upscale; **frame generation off** — at 20 fps the made frames
  smear the HUD into red bars and make the icons blink

## How this fork is optimised (the reasoning)

The Deck's frame is **CPU-bound, not GPU-bound**: the replay profiler shows the emulator's own
replay at ~7–10 ms a frame (submit, memory sync, queue submit, draws) while the GPU sits at 8–12 ms.
The large majority of the frame is the game's recompiled code. So:

- **The GPU's idle time goes to data-parallel work, not to the replay.** The replay is serial and
  branch-heavy; a GPU cannot help it. The GPU *can* take texture swizzle/upload (the current
  `upload()` CPU staging shows stalls up to **785 ms**, 20 per run), the ASTC→BC3 decode, the
  write-back gathers and the post chain.
- **The game code is compiled at `-O2`** (the build defaults to `-O1` with only the hot file at
  `-O2`); PGO (`RX_PGO=gen` → profile → `RX_PGO=<prof>`) is the next step.
- **Multi-threading** only where the work is order-independent (the #939 uniform-list family).

## How this fork is measured

Everything here is A/B-tested, not guessed:

- **Flash counter** (`RX_VK_FLASH=1`, `RX_VK_FLASH_REGION=8`, with `RX_VK_SHOTS`): counts one-frame
  flashes in the renderer's own frames. A build is only "clean" at 0 in-game flashes.
- **Static-view harness** (`flicker_test.py --static`): the save's own camera, no input after the
  world loads. Repeatable to ±7 %; camera sweeps are not comparable.
- **Smoothness**: `fps_avg_steady`, `frame_ms_p50`, `frame_ms_p99`, `fps_1pct_low` over the steady
  half of the run, plus a thermal cool-down between runs (the Deck throttles; uncooled A/B is noise).

## Traps learned on this device

- The flash counter needs `RX_VK_SHOTS`; without it a run silently reports 0 flashes.
- Frame dumps are 4 GB and halve the frame rate: use `RX_VK_FLASH_DUMPS=0` and clean `shots/`.
- `HOS_RES_SCALE` is the VI output scale, not the render resolution — it buys no frames.
- The harness's "world" detection reads draw count > 400; the items menu also passes it. Press
  **A only** at the title (Plus opens the menu and the run then measures the menu).
- The launcher must honour `HOS_CONFIG`, or a config silently measures nothing.
- Steam shortcuts must end with `0x08`, not `0x00`.

## Builds

Built on white-dragon (WSL2, 12 cores) from the same tree with the AppImage toolchain. One change
per build, measured before the next. The known-good binary is kept beside each build.
