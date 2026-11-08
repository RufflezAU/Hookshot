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

## Reproducing a row

```sh
HOS_GAME=/path/to/game HOS_LAUNCHER=/path/to/game/hookshot-play.sh \
HOS_CACHE_SEED= python3 deck/test/ab_harness.py single --name myrun --seconds 240
python3 deck/test/ab_report.py
```

Add the result to this table (and the `.json` here) whenever a build or a config changes.
