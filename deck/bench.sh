#!/bin/sh
# deck/bench.sh -- measure the same thing on both paths, in a session that has a display.
#
#   ./bench.sh              both, 180 s each
#   ./bench.sh hookshot     only the recompilation
#   ./bench.sh emulator     only the emulator
#   BENCH_SECS=240 ./bench.sh
#
# Writes bench-results/summary.txt and one log per run. Every number comes from the program's own log:
#   Hookshot:  "vi: N frames presented (X fps over the last 5.0 s)"
#   Emulator:  "PresentClientBuffer: nx2:vi2 last 600 presents: frame N us"   -> 1000000 / N fps
# Both are windowed: frame generation and the emulator need a real window, which a script over SSH does not have.

set -u
GAME=${GAME:-/home/deck/Games/Hookshot-legacy477b}
EMU=${EMU:-/home/deck/Applications/lichii}
SECS=${BENCH_SECS:-180}
OUT="$GAME/bench-results"
mkdir -p "$OUT" || exit 1
WHAT=${1:-both}

say() { printf '%s\n' "$*" | tee -a "$OUT/summary.txt"; }
: > "$OUT/summary.txt"
say "benchmark $(date '+%Y-%m-%d %H:%M')  (${SECS}s a run)"
say ""

# ---------------------------------------------------------------- Hookshot
bench_hookshot() {
    cfg=${1:-stable}
    log="$OUT/hookshot-$cfg.log"
    say "== Hookshot, config $cfg =="
    [ -f "$GAME/configs/$cfg.env" ] || { say "   no configs/$cfg.env, skipped"; return; }
    [ -f "$GAME/configs/$cfg.ini" ] || { say "   no configs/$cfg.ini, skipped"; return; }
    ( cd "$GAME" && HOS_CONFIG="$cfg" HOS_WORLD_TIMEOUT=300 HOS_RSS_KILL_MB=13000 \
        HOS_GAME="$GAME" HOS_LAUNCHER="$GAME/hookshot-legacy477b-play.sh" \
        python3 "/home/deck/Documents/Default Project/flicker_test.py" --name "bench-$cfg" --seconds "$SECS" --windowed >"$log" 2>&1 ) &
    pid=$!
    # the run stops itself; this is only the backstop
    n=0
    while kill -0 "$pid" 2>/dev/null && [ "$n" -lt $((SECS + 240)) ]; do sleep 5; n=$((n + 5)); done
    kill "$pid" 2>/dev/null
    res="$GAME/flicker-results/bench-$cfg.json"
    if [ -f "$res" ]; then
        python3 - "$res" "$GAME/hookshot_log.txt" <<'PY' | tee -a "$OUT/summary.txt"
import json, re, sys
d = json.load(open(sys.argv[1]))
try:
    log = open(sys.argv[2], errors="replace").read()
except OSError:
    log = ""
rep = [float(x) for x in re.findall(r"replay time \(ms\): submit ([0-9.]+)", log)]
draws = re.findall(r"a frame: (\d+) draws", log)
print("   fps %.1f   p50 %.2f ms   p99 %.2f ms   1%% low %.1f" % (
    d["fps_avg_steady"], d["frame_ms_p50"], d["frame_ms_p99"], d["fps_1pct_low"]))
print("   replay %.2f ms (last 10 reports)   draws %s   flashes %d" % (
    (sum(rep[-10:]) / len(rep[-10:])) if rep else float("nan"), draws[-1] if draws else "?", d.get("flashes", 0)))
PY
    else
        say "   no result (the run did not reach the world; see $log)"
    fi
    say ""
}

# ---------------------------------------------------------------- emulator
bench_emulator() {
    log="$OUT/emulator.log"
    elog="/home/deck/.local/share/eglantine/log/eden_log.txt"
    say "== emulator (Lichii/Eglantine, --fidelity) =="
    [ -x "$EMU/lichii-oot.sh" ] || { say "   no launcher at $EMU/lichii-oot.sh, skipped"; return; }
    rm -f "$elog"
    ( cd "$EMU" && ./lichii-oot.sh --auto --fidelity >"$log" 2>&1 ) &
    pid=$!
    n=0
    while [ "$n" -lt "$SECS" ] && kill -0 "$pid" 2>/dev/null; do sleep 5; n=$((n + 5)); done
    kill "$pid" 2>/dev/null
    sleep 2
    pkill -f eden-cli 2>/dev/null
    if [ ! -f "$elog" ]; then say "   no emulator log ($elog)"; say ""; return; fi
    python3 - "$elog" <<'PY' | tee -a "$OUT/summary.txt"
import re, sys
lines = open(sys.argv[1], errors="replace").read().splitlines()
per = []
for ln in lines:
    m = re.search(r"last (\d+) presents: frame (\d+) us", ln)
    if m:
        per.append((int(m.group(1)), int(m.group(2))))
if not per:
    print("   no 'last N presents' line: the emulator did not present (see %s)" % sys.argv[1])
    bad = [l for l in lines[-40:] if re.search(r"Critical|Error", l)]
    for l in bad[-3:]:
        print("   " + l[:150])
else:
    n, us = per[-1]
    fps = 1e6 / us if us else 0.0
    print("   fps %.1f   frame %.2f ms   (%d presents a report, %d reports)" % (fps, us / 1000.0, n, len(per)))
    if len(per) > 2:
        vals = sorted(1e6 / u for _, u in per[1:] if u)
        print("   over the run: median %.1f fps, worst %.1f" % (vals[len(vals) // 2], vals[0]))
PY
    say ""
}

case "$WHAT" in
    hookshot) bench_hookshot stable ;;
    emulator) bench_emulator ;;
    both)     bench_hookshot stable; bench_emulator ;;
    *)        echo "usage: $0 [both|hookshot|emulator]" >&2; exit 2 ;;
esac

say "results in $OUT/"
