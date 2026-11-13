"""On Linux no core is kept from the game's threads unless the replay thread is there to use it.

host/hos_thread.c hos_reserved_cpus keeps the last physical core free of the game's threads for the renderer's replay
(hos_thread_pin pins every game thread off it). On Windows the synchronous replay, the default, moves the replaying
thread onto that core (gpu/vk/vk_replay.cpp replay_pin_mask); that move is Windows only, so on Linux only the replay
thread of RX_VK_ASYNC=1 ever ran there. With the default the core had nothing of the game's on it, and as a Linux thread
starts with its maker's processors, nearly the whole process ran on the others: three of a Steam Deck's four cores.

The fixture is hos_reserved_cpus cut from the source, built as the Linux build sees it (_WIN32 not defined), over a
processor list of four cores of two logical processors each, one process a case (the answer is kept after the first
call):

- nothing set: no core kept;
- RX_VK_ASYNC=1: the last core (logical processors 6 and 7);
- HOS_RESERVE_CORE=1: the last core, whatever the replay;
- HOS_RESERVE_CORE=0: none, even with the replay thread;
- RX_VK_ASYNC=0: none.

A variant without the Linux rule must fail the first case.

python -I -B tests/test_linux_reserved_core.py [--cc gcc]
"""
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
MARKER = "uint64_t hos_reserved_cpus(void) {"
RULE = "if (!(async && atoi(async) != 0) && !(keep && atoi(keep) != 0)) {"

FIXTURE = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef _WIN32   /* (the Linux build's branch, on every runner) */
typedef unsigned long DWORD;
typedef int BOOL;
enum { RelationProcessorCore = 0 };
typedef struct { uint64_t ProcessorMask; int Relationship; } SYSTEM_LOGICAL_PROCESSOR_INFORMATION;
static void hos_log(const char *fmt, ...) { (void)fmt; }
/* four cores, two logical processors each: a Steam Deck */
static BOOL GetLogicalProcessorInformation(SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info, DWORD *len) {
    for (int i = 0; i < 4; i++) info[i].ProcessorMask = 3ull << (2 * i), info[i].Relationship = RelationProcessorCore;
    *len = 4 * sizeof *info;
    return 1;
}
@CODE@
int main(int argc, char **argv) {
    for (int i = 2; i < argc; i++) putenv(argv[i]);
    const uint64_t want = strtoull(argv[1], NULL, 16), got = hos_reserved_cpus();
    if (got != want || hos_reserved_cpus() != want) { fprintf(stderr, "FAIL: kept %#llx, not %#llx\n", (unsigned long long)got, (unsigned long long)want); return 1; }
    return 0;
}
"""
CASES = [
    ("0", []),
    ("c0", ["RX_VK_ASYNC=1"]),
    ("c0", ["HOS_RESERVE_CORE=1"]),
    ("0", ["HOS_RESERVE_CORE=0", "RX_VK_ASYNC=1"]),
    ("0", ["RX_VK_ASYNC=0"]),
]


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    src = (ROOT / "host/hos_thread.c").read_text(encoding="utf-8")
    check(MARKER in src, "host/hos_thread.c has no hos_reserved_cpus")
    code = block(src, MARKER)
    check(RULE in code, "hos_reserved_cpus no longer has the Linux rule this test names")
    compiler = shutil.which(args.cc)
    check(compiler, "C compiler not found: " + args.cc)
    env = {k: v for k, v in os.environ.items() if k not in ("RX_VK_ASYNC", "HOS_RESERVE_CORE")}

    def build(folder, name, text):
        source = folder / (name + ".c")
        source.write_text(FIXTURE.replace("@CODE@", text), encoding="utf-8")
        exe = folder / (name + (".exe" if os.name == "nt" else ""))
        subprocess.run([compiler, "-std=gnu11", "-O1", "-w", str(source), "-o", str(exe)], check=True, timeout=120)
        return exe

    with tempfile.TemporaryDirectory(prefix="linux-reserved-core-") as tmp:
        folder = Path(tmp)
        exe = build(folder, "fixture", code)
        for want, settings in CASES:
            r = subprocess.run([str(exe), want] + settings, env=env, capture_output=True, text=True, timeout=30)
            check(r.returncode == 0, "%s: %s" % (" ".join(settings) or "nothing set", r.stderr.strip()))
        old = build(folder, "always", code.replace(RULE, "if (0) {"))
        r = subprocess.run([str(old), "0"], env=env, capture_output=True, text=True, timeout=30)
        check(r.returncode != 0 and "FAIL:" in r.stderr, "a core kept with nothing to run on it passes")
    print("ok")


if __name__ == "__main__":
    main()
