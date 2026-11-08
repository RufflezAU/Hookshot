# Configs

Every `.env` here is a complete `hookshot_env.txt`; the matching `.ini` is a complete
`hookshot_options.ini` (resolution, upscaling, sharpness, frame generation). The launcher copies
both over the game folder's files before it starts, so one build can be run with any of them:

```sh
./hookshot-play.sh low          # configs/low.env + configs/low.ini
HOS_CONFIG=fps ./hookshot-play.sh
```

The argument and `HOS_CONFIG` do the same thing; a Steam shortcut can use LaunchOptions `low`.

| name | budget | tex cap | eviction | keep finalized | what it is |
|---|---|---|---|---|---|
| `stable` | 1200 MB | 128 MB | 1500 ms | default | the only budget that stayed under the driver's allowance (peak 6099 of 6144 MB); 3/3 runs completed |
| `fps` | 1500 MB | 128 MB | 800 ms | default | best frame rate in the A/B (12.0 fps median), 83 MB over the allowance |
| `keepoff` | 1200 MB | 128 MB | 1500 ms | off | `stable` with #593's finalized-texture keep off, for bisecting the door crash |
| `legacy` | 1200 MB | 128 MB | 1500 ms | n/a | for the `deck/legacy` build (same numbers as `stable`; that branch predates this folder) |
| `low` | 1200 MB | 128 MB, 1024 px | 3000 ms | default | opt-in Deck look: Catmull-Rom upscaling and 20 % sharpness in `low.ini`, less eviction churn. The aggressive 512 px / 64 MB caps were tried and made the intro movie run at 7-11 fps, so they are not in it |

`hookshot_options.ini` (the menu file) is separate and is not copied by `HOS_CONFIG`: 50 %
resolution, Custom preset, 6x frame generation, handheld off. Keep it identical across builds.

The measured numbers behind these choices are in `../STATE.md` and `../README.md`.
