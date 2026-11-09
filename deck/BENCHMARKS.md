# Benchmarks

Measured numbers so a build can be chosen on evidence, not on hope. Every row is a run of
`test/ab_harness.py` with the same save, the same route and the same 240 s (the `*.json` files in
this folder and `legacy/` are the raw results).

**How to read this**

- Headless (`HOS_HEADLESS=1`): no window, no compositor, frame generation off. It measures the
  renderer and the replay, not the compositor. It cannot reproduce a Steam-UI restart.
- `fps` is the last 5-second rate at the end of the run; `gpu` the GPU time per frame; `vram` the
  peak against the driver's allowance. `world` is the second the draw count first passed 400.
- Scenes differ. The starting room is ~555 draws; Kokiri Forest is ~2800. Do not compare across
  scenes — compare rows with the same `scene`.

## Legacy base (branch `deck/legacy`, `ad1b641`)

24 runs, 3 per config, starting room into Kokiri Forest (~2800 draws).

| config | fps (median) | GPU ms | VRAM peak / allowed | textures | early ends |
|---|---|---|---|---|---|
| `stable` (1200/128/1500) | 11.6 | 66.2 | 6099 / 6144 | 2752 | 0 |
| `fps` (1500/128/800) | 12.0 | 54.6 | 6227 / 6144 (+83) | 2818 | 0 |
| `evict3000` (1500/128/3000) | 10.8 | 56.1 | 6227 / 6144 (+83) | 2695 | 0 |
| `nocull0` (culling on) | 10.2 | 51.7 | 6227 / 6144 (+83) | 2864 | 1 memory kill |
| `keepret` (giveback off) | 10.8 | 60.5 | 6099 / 6144 | 2892 | 1 exit |
| `tex96` | 11.1 | 64.3 | 6227 / 6144 (+83) | 2928 | 0 |
| `tex256` | 12.0 | 56.9 | 6355 / 6144 (+211) | 3117 | 0 |

Reading: the texture budget is a weak lever (1200 vs 1500 moves peak VRAM ~128 MB, because the
buffer pools and imports fill it). `fps` is the fastest but sits 83 MB over; `stable` is the only
one that stayed under the allowance. None of them crashed.

## Current main base (branch `deck/steamdeck`, `58af2c0` + the eviction patch)

| run | scene | fps | GPU ms | VRAM peak / allowed | notes |
|---|---|---|---|---|---|
| `main-interior.json` | starting room, ~555 draws | ~50 | 15.5 | 3428 / 5836 | no OOM, no device lost |
| `main-door-headless.json` | never left the title | — | 15.1 | 874 / 5845 | boot only, killed by the memory guard |
| `main-boot.json` | never left the title | — | 13.1 | 866 / 5784 | first boot, program cache cold |

**Windowed, this build exits when walking out of the room through the door** (no device lost, exit
code 0, log ends in the finalized-texture upload path). Reported upstream as
<https://github.com/NotRexed/Hookshot/issues/862>. The same save is stable on the legacy base.


## Manual (windowed) observations

Headless cannot show the compositor, the Steam-UI restart or the door exit, so the windowed runs are
recorded here by hand. They are observations, not a controlled benchmark; the measurable columns are
read from the same `hookshot_log.txt` afterwards.

| date | build | config | what happened | fps seen | log facts |
|---|---|---|---|---|---|
| 2026-11-09 01:45 | udmabuf base (PR #448) | 50 %, Minimum, 6x | world rendered, walked, **OOM at the save** | ~7 | 1843 draws, GPU 43.5 ms, VRAM 5991/6037, `Not enough memory for command submission` |
| 2026-11-09 02:15 | udmabuf + giveback | 50 %, Minimum, 6x | walked ~3 min, then **Steam UI restarted** under it | 7–15 | VRAM 6099/6144, textures 4005 MB peak, GPU 26 → 69 ms |
| 2026-11-09 07:47 | legacy + eviction (`stable`) | 1200/128/1500 | **stable**, walked, lighting and texture flicker, dips | 6–15 | max submit 208 ms, max upload 340 ms (288 MB texture), VRAM 7023/6144 |
| 2026-11-09 07:55 | legacy + eviction (`fps`) | 1500/128/800 | stable, no visible difference from `stable` | 6–15 | same class |
| 2026-11-09 09:35 | main `58af2c0` + eviction | 1200/128/1500 | looked nicer, **exited at the room door** | ~15 before | 856 draws, VRAM 3840/6027, no device lost, exit code 0 |
| 2026-11-09 09:41 | main + eviction, `KEEP_FINALIZED_MB=0` | 1200/128/1500 | same exit at the door | — | log ends in the finalized-texture upload path |
| 2026-11-09 10:45 | latest main `15ec28d` (vanilla) | 1200/128/1500 | **crashes in the RADV driver at startup**, headless | — | `CRASH: exception 0xc0000005 ... at libvulkan_radeon.so+0x19cc52`, right after the mesh programs |

The two verdicts that only a person can give: the legacy base **does not crash** but flickers; the
main base **looks better** but exits at the door. That is the trade-off this branch is trying to
close, and it is why both rows stay in the table.

Add a row after every windowed test: date, build, config, what happened, the fps range you saw, and
the one or two log facts (`VRAM`, `GPU time`, `slow submit`, `device lost`).


### 2026-11-09, stable vs evict3000 on the legacy build (2 runs each, 240 s)

| run | scene | fps | GPU ms | max submit | VRAM peak / allowed | textures |
|---|---|---|---|---|---|---|
| stable 1 | room, 717 draws | 24.7 | 61.5 | 83.0 | 5451 / 6124 | 1049 |
| evict3000 1 | room, 625 draws | 42.0 | 51.5 | 62.2 | 5835 / 6144 | 1038 |
| stable 2 | **forest**, 2568 draws | 12.1 | 69.9 | 63.5 | 6099 / 6144 | 2828 |
| evict3000 2 | room, 689 draws | 57.1 | 42.9 | 50.5 | 5331 / 6138 | 897 |

The steering does not always get out of the room, so three runs are the room and one is the forest —
the fps column is not comparable across them. In the room runs `evict3000` is clearly better on the
jerk metric (max submit 50–62 ms against 83 ms, GPU 43–52 against 62). The forest sample is stable 2
only, so the forest comparison is still open.

## Reproducing a row

```sh
HOS_GAME=/path/to/game HOS_LAUNCHER=/path/to/game/hookshot-play.sh \
HOS_CACHE_SEED= python3 deck/test/ab_harness.py single --name myrun --seconds 240
python3 deck/test/ab_report.py
```

Add the result to this table (and the `.json` here) whenever a build or a config changes.
