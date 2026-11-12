# Steam Deck build — what is set up here and why

This folder holds the Deck-specific pieces that live beside the game, so they survive the game folder being
rebuilt. Everything in the renderer itself is in the commits on `deck/rebase-main`.

## What is in here

| file | what it is |
|---|---|
| `launcher.sh` | the launcher the Steam shortcut runs. Copies the config in, sets `ulimit -c 0`, starts the game. |
| `stable.env` | the settings the measurements were taken with (`hookshot_env.txt` beside the executable) |
| `stable.ini` | the in-game options (`hookshot_options.ini` beside the executable) |

Install: drop the three into the game folder, keeping the names the launcher uses (`configs/stable.env`,
`configs/stable.ini`), or point `HOS_CONFIG` at them.

## The two things that must not be lost

**`ulimit -c 0` in the launcher.** The emulator aborts on the heap while closing (`malloc(): unsorted double linked
list corrupted`). That is harmless on its own, but SteamOS pipes cores to a Python submitter and this process is
495 MB with ~2.5 GB mapped, so the kernel was writing a multi-GB core and the desktop was unusable for seconds
afterwards. The corruption is still there and still un-found; it is exit-only.

**The shader preparation.** Run once, after any change to the shader-affecting settings:

```sh
./hookshot --prepare-shaders
```

It builds every program and the pipelines the notes predict — 17,645 of them, about 35 minutes on the Deck — and
writes the caches beside the executable. Measured effect: waits for shaders or pipelines **9.4 ms a frame → 0.0**,
translation over a session **4,608 ms → 202 ms**, at no cost in steady state (21.8 vs 21.6 fps back to back).

## Settings worth knowing

- **75 % internal resolution** (`resolution=2` in the ini). The sharpness the player chose; about 2.5 fps against
  50 %.
- `RX_VK_STAGING_IDLE_S=0` and `RX_VK_STAGING_KEEP_MB=256` — keep the upload staging arenas instead of letting them
  go stale. Making one under memory pressure cost 12–25 ms and it happened 28 times a session; 1 time after.
- `RX_VK_FRAMEGEN=1` — frame generation allowed (the in-game row decides). **Unmeasured on this hardware**: it needs
  a real window, and the test harness runs offscreen, so it cannot be A/B'd from a script. Set to `0` to rule it out.
- `HOS_REALTIME_FLOOR=10` — keeps the game's time step sane below 60 fps.

## Measuring on this device

Frame rate swings **40 %** between identical runs (12.4 vs 17.3 fps on the same config and scene) because six
emulated cores share four physical ones with the rest of the system. The **replay time** in the log is stable to
about ±8 % and is the number to compare:

```
grep "a frame's replay time" hookshot_log.txt | tail -1
```
