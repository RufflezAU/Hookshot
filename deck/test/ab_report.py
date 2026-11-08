#!/usr/bin/env python3
"""Rank the ab-results/*.json runs and print the morning table."""
import glob, json, os, statistics, sys

RESULTS = os.environ.get("HOS_RESULTS", os.path.join(os.getcwd(), "ab-results"))


def load():
    runs = []
    for p in sorted(glob.glob(os.path.join(RESULTS, "*.json"))):
        try:
            with open(p) as f:
                d = json.load(f)
        except Exception:
            continue
        d["_file"] = os.path.basename(p)
        runs.append(d)
    return runs


def valid(r):
    return r.get("world_at") is not None and r.get("max_draws", 0) > 400


def score(r):
    """Lower is better. A run that never reached the world sorts last."""
    if not valid(r):
        return (9, 0, 0, 0, 0)
    paging = 1 if (r["peak_vram_allowed"] and r["peak_vram_used"] > r["peak_vram_allowed"] * 0.97) else 0
    faults = r["oom"] + r["device_lost"] + r["failed_submit"]
    return (faults, paging, round(r["max_gpu_ms"], 1), round(r["max_submit_ms"], 1), -round(r["last_fps"] or 0, 1))


def main():
    runs = load()
    if not runs:
        print("no results in", RESULTS)
        return
    # group by config name (strip _rN)
    groups = {}
    for r in runs:
        name = r["_file"].rsplit("_r", 1)[0] if "_r" in r["_file"] else r["_file"][:-5]
        groups.setdefault(name, []).append(r)

    rows = []
    for name, rs in groups.items():
        ok = [r for r in rs if valid(r)]
        bad = [r for r in rs if not valid(r)]
        faults = sum(r["oom"] + r["device_lost"] + r["failed_submit"] for r in rs)
        peak_over = max((r["peak_vram_used"] - r["peak_vram_allowed"]) for r in rs if r["peak_vram_allowed"]) if rs else 0
        rows.append({
            "name": name,
            "runs": len(rs),
            "world": len(ok),
            "faults": faults,
            "over_mb": peak_over,
            "gpu": max((r["max_gpu_ms"] for r in rs), default=0),
            "submit": max((r["max_submit_ms"] for r in rs), default=0),
            "fps": min((r["last_fps"] or 0 for r in rs), default=0),
            "tex": max((r["peak_tex"] for r in rs), default=0),
            "budget": max((r["peak_budget"] for r in rs), default=0),
            "churn": max((r["frames_made_max"] for r in rs), default=0),
            "given": max((r["imports_given_mb"] for r in rs), default=0),
        })

    rows.sort(key=lambda r: (r["world"] == 0, r["faults"], r["over_mb"] > 0, r["gpu"], r["submit"], -r["fps"]))
    hdr = f"{'config':28} {'runs':>4} {'world':>5} {'faults':>6} {'overMB':>7} {'gpu_ms':>6} {'submit':>6} {'fps':>5} {'texMB':>6} {'budget':>6} {'churn':>5} {'given':>5}"
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        print(f"{r['name']:28} {r['runs']:4} {r['world']:5} {r['faults']:6} {r['over_mb']:7} "
              f"{r['gpu']:6.1f} {r['submit']:6.1f} {r['fps']:5.1f} {r['tex']:6} {r['budget']:6} "
              f"{r['churn']:5.0f} {r['given']:5}")

    best = rows[0] if rows and rows[0]["world"] else None
    print()
    if best:
        print("best:", best["name"], "|", best["world"], "of", best["runs"], "runs reached the world, faults:", best["faults"])
    else:
        print("no config reached the world — the steering script needs work, not the config")


if __name__ == "__main__":
    main()
