#!/usr/bin/env python3
"""Overnight A/B harness for the Hookshot udmabuf build.

Runs headless (no window, so it cannot take the Deck's screen or restart Steam), steers the
game into the save with HOS_INPUT_LIVE, walks it, and records what the renderer did.

One run:
  python3 ab_harness.py single --name test --seconds 480
A matrix (detached):
  setsid nohup python3 ab_harness.py matrix > ab-matrix.log 2>&1 &
"""
import argparse, json, os, re, shutil, signal, subprocess, sys, time

GAME = os.environ.get("HOS_GAME", os.getcwd())
LAUNCHER = os.environ.get("HOS_LAUNCHER", os.path.join(GAME, "hookshot-play.sh"))
LOG = os.path.join(GAME, "hookshot_log.txt")
LIVE = os.environ.get("HOS_LIVE", "/tmp/hookshot_live_input.txt")
RESULTS = os.environ.get("HOS_RESULTS", os.path.join(os.getcwd(), "ab-results"))
SAVE = os.environ.get("HOS_SAVE", os.path.expanduser("~/.local/share/hookshot/save"))
SAVE_BACKUP = os.environ.get("HOS_SAVE_BACKUP", os.path.join(os.getcwd(), "ab-save-backup"))
CACHE_SEED = os.environ.get("HOS_CACHE_SEED", "")

BASE_ENV = {
    "RX_VK_TEX_MAX_DIM": "1024",
    "RX_VK_MIRROR_MB": "192",
    "RX_VK_CHECKPOINTS": "1",
    "RX_VK_NOCULL": "1",
    "RX_VK_EVICT_MIN_MS": "1500",
    "RX_VK_VRAM_MB": "1500",
    "RX_VK_TEX_MAX_MB": "128",
}

# the agent itself (OpenCode) shares this machine and gets killed when RAM runs out: the game must be the first to go
MEM_FLOOR_KB = 50_000         # (the boot spike really does reach this; oom_score_adj protects the agent)
MEM_START_KB = 7_000_000      # do not start a run below this
RSS_CAP_KB = 11_500_000       # kill the game itself if it grows past this (the agent must survive)


def mem_available_kb():
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def wait_for_memory(limit_kb, seconds=300):
    t0 = time.time()
    while time.time() - t0 < seconds:
        if mem_available_kb() >= limit_kb:
            return True
        time.sleep(5)
    return mem_available_kb() >= limit_kb


def rss_kb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def make_killable(pid):
    """The kernel should pick the game, not the agent, when memory runs out."""
    try:
        with open(f"/proc/{pid}/oom_score_adj", "w") as f:
            f.write("1000")
    except OSError:
        pass


BASE_INI = """options_version=2
quality_preset=0
fullscreen=1
frame_generation=6
frame_pacing=1
resolution=4
upscaling=2
sharpness=35
game_speed=1
handheld_profile=0
ultrawide=0
fps_counter=1
controller_in_background=0
text_speed=0
"""


def write_config(env_extra, ini_text):
    env = dict(BASE_ENV)
    env.update(env_extra)
    with open(os.path.join(GAME, "hookshot_env.txt"), "w") as f:
        for k, v in env.items():
            f.write(f"{k}={v}\n")
    with open(os.path.join(GAME, "hookshot_options.ini"), "w") as f:
        f.write(ini_text)


def restore_save():
    if os.path.isdir(SAVE_BACKUP):
        if os.path.isdir(SAVE):
            shutil.rmtree(SAVE)
        shutil.copytree(SAVE_BACKUP, SAVE)


def restore_cache():
    # (the program cache is kept warm across runs: recompiling the whole title every run was what
    # spiked memory during boot, and a warm cache is also closer to a real session)
    if not CACHE_SEED:
        return
    target = os.path.join(GAME, "hookshot_programs.bin")
    try:
        if os.path.getsize(target) >= 50 << 20:
            return
    except OSError:
        pass
    if os.path.isfile(CACHE_SEED):
        shutil.copyfile(CACHE_SEED, target)


def live(line):
    try:
        with open(LIVE, "a") as f:
            f.write(line + "\n")
    except OSError:
        pass


class Log:
    def __init__(self, path):
        self.path = path
        self.pos = 0
        self.buf = ""

    def read(self):
        try:
            with open(self.path, "r", errors="replace") as f:
                f.seek(self.pos)
                data = f.read()
                self.pos = f.tell()
        except OSError:
            return ""
        self.buf += data
        return data


RE_DRAWS = re.compile(r"a frame: (\d+) draws")
RE_VRAM = re.compile(r"VRAM: (\d+) MB used of (\d+) MB allowed.*?textures (\d+) MB \(budget (\d+) MB\)")
RE_GPU = re.compile(r"GPU time ([\d.]+) ms a frame")
RE_SUBMIT = re.compile(r"submit ([\d.]+), present")
RE_FPS = re.compile(r"presents, ([\d.]+) fps")
RE_IMPORTS = re.compile(r"imports: (\d+) live \((\d+) MB\).*?(\d+) retired given back \((\d+) MB\)")


def kill_strays():
    # a run whose harness was killed leaves its game behind, holding gigabytes: kill it by exact name
    try:
        subprocess.run(["pkill", "-9", "-x", "hookshot"], timeout=10,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass


def run_one(name, env_extra, ini_text, seconds, log_extra=None):
    os.makedirs(RESULTS, exist_ok=True)
    kill_strays()
    wait_for_memory(MEM_START_KB)
    restore_save()
    restore_cache()
    write_config(env_extra, ini_text)
    open(LIVE, "w").close()
    try:
        os.remove(LOG)
    except OSError:
        pass

    env = dict(os.environ)
    env["HOS_HEADLESS"] = "1"
    env["HOS_INPUT_LIVE"] = LIVE
    env["XDG_DATA_HOME"] = "/home/deck/.local/share"
    env.update(BASE_ENV)
    env.update(env_extra)
    proc = subprocess.Popen([LAUNCHER], cwd=GAME, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            preexec_fn=os.setsid)
    make_killable(proc.pid)
    t0 = time.time()
    log = Log(LOG)
    stats = {"name": name, "env": env_extra, "seconds": seconds,
             "max_draws": 0, "world_at": None, "peak_tex": 0, "peak_budget": 0,
             "peak_vram_used": 0, "peak_vram_allowed": 0, "max_gpu_ms": 0.0,
             "max_submit_ms": 0.0, "last_fps": None, "imports_given_mb": 0,
             "oom": 0, "device_lost": 0, "failed_submit": 0, "mem_kill": 0, "rss_kill": 0, "ended_at": None, "returncode": None,
             "exited_early": False, "exit_at": None, "frames_made_max": 0}
    world = False
    last_press = 0.0
    walk = 0
    while True:
        time.sleep(0.5)
        log.read()
        elapsed = time.time() - t0
        for m in RE_DRAWS.finditer(log.buf):
            n = int(m.group(1))
            if n > stats["max_draws"]:
                stats["max_draws"] = n
            if n > 400 and stats["world_at"] is None:
                stats["world_at"] = round(elapsed, 1)
                world = True
        for m in RE_VRAM.finditer(log.buf):
            used, allowed, tex, budget = map(int, m.groups())
            stats["peak_vram_used"] = max(stats["peak_vram_used"], used)
            stats["peak_vram_allowed"] = max(stats["peak_vram_allowed"], allowed)
            stats["peak_tex"] = max(stats["peak_tex"], tex)
            stats["peak_budget"] = max(stats["peak_budget"], budget)
        for m in RE_GPU.finditer(log.buf):
            stats["max_gpu_ms"] = max(stats["max_gpu_ms"], float(m.group(1)))
        for m in RE_SUBMIT.finditer(log.buf):
            stats["max_submit_ms"] = max(stats["max_submit_ms"], float(m.group(1)))
        fps = RE_FPS.findall(log.buf)
        if fps:
            stats["last_fps"] = float(fps[-1])
        for m in RE_IMPORTS.finditer(log.buf):
            stats["imports_given_mb"] = max(stats["imports_given_mb"], int(m.group(4)))
        stats["oom"] = log.buf.count("Not enough memory")
        stats["device_lost"] = log.buf.count("device lost")
        stats["failed_submit"] = log.buf.count("failed (-4)")
        m = re.findall(r"([\d.]+) textures made a frame", log.buf)
        if m:
            stats["frames_made_max"] = max(stats["frames_made_max"], float(m[-1]))

        if proc.poll() is not None:
            stats["exited_early"] = True
            stats["exit_at"] = round(elapsed, 1)
            stats["returncode"] = proc.returncode
            break
        if elapsed >= seconds:
            break
        if mem_available_kb() < MEM_FLOOR_KB:
            stats["mem_kill"] = 1
            break
        if rss_kb(proc.pid) > RSS_CAP_KB:
            stats["rss_kill"] = 1
            break

        # steering
        if elapsed - last_press >= 5.0:
            last_press = elapsed
            if not world:
                # Plus only while the title screen is up (61 draws). A slower boot used to miss a
                # fixed time window, and once the save is loading Plus opens the pause menu.
                if stats["max_draws"] <= 100:
                    live("0:PLUS")
                    live("1:A")
                else:
                    live("0:A")
            else:
                walk += 1
                # walk forward, turn a little every so often, and press A now and then
                live("0:LUP/4200")
                if walk % 4 == 0:
                    live("0:RRIGHT/900")
                if walk % 7 == 0:
                    live("0:A")

    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    except ProcessLookupError:
        pass
    time.sleep(1)
    log.read()
    # final parse of anything that arrived during shutdown
    for m in RE_VRAM.finditer(log.buf):
        used, allowed, tex, budget = map(int, m.groups())
        stats["peak_vram_used"] = max(stats["peak_vram_used"], used)
        stats["peak_vram_allowed"] = max(stats["peak_vram_allowed"], allowed)
        stats["peak_tex"] = max(stats["peak_tex"], tex)
        stats["peak_budget"] = max(stats["peak_budget"], budget)
    stats["ended_at"] = round(time.time() - t0, 1)
    stats["log_lines"] = log.buf.count("\n")
    out = os.path.join(RESULTS, f"{name}.json")
    with open(out, "w") as f:
        json.dump(stats, f, indent=2)
    try:
        shutil.copyfile(LOG, os.path.join(RESULTS, f"{name}.log"))
    except OSError:
        pass
    print(json.dumps(stats), flush=True)
    return stats


MATRIX = [
    # name, env overrides, ini overrides
    # (1800 and 2200 are left out on purpose: they are the budgets that ran the machine out of RAM)
    ("b1200_t128_e1500", {"RX_VK_VRAM_MB": "1200"}, {}),
    ("b1500_t128_e1500", {}, {}),
    ("b1500_t256_e1500", {"RX_VK_TEX_MAX_MB": "256"}, {}),
    ("b1500_t128_e800", {"RX_VK_EVICT_MIN_MS": "800"}, {}),
    ("b1500_t128_e3000", {"RX_VK_EVICT_MIN_MS": "3000"}, {}),
    ("b1500_t128_e1500_nocull0", {"RX_VK_NOCULL": "0"}, {}),
    ("b1500_t128_e1500_keepret", {"RX_VK_KEEP_RETIRED": "1"}, {}),
    ("b1500_t96_e1500", {"RX_VK_TEX_MAX_MB": "96"}, {}),
]


def ini_for(over):
    ini = BASE_INI
    for k, v in over.items():
        ini = re.sub(rf"^{k}=.*$", f"{k}={v}", ini, flags=re.M)
    return ini


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["single", "matrix"])
    ap.add_argument("--name", default="single")
    ap.add_argument("--seconds", type=int, default=480)
    ap.add_argument("--repeats", type=int, default=3)
    args = ap.parse_args()

    if not os.path.isdir(SAVE_BACKUP) and os.path.isdir(SAVE):
        shutil.copytree(SAVE, SAVE_BACKUP)
    os.makedirs(RESULTS, exist_ok=True)

    if args.mode == "single":
        run_one(args.name, {}, BASE_INI, args.seconds)
        return

    order = list(MATRIX)
    # interleave repeats so a failure late in the night still leaves every config with one run
    for r in range(args.repeats):
        for name, env_over, ini_over in order:
            run_one(f"{name}_r{r+1}", env_over, ini_for(ini_over), args.seconds)
    with open(os.path.join(RESULTS, "MATRIX-DONE"), "w") as f:
        f.write(time.strftime("%Y-%m-%d %H:%M:%S\n"))


if __name__ == "__main__":
    main()
