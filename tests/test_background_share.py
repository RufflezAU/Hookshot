"""A background game thread is held to its share of a processor, and a thread waiting for its lock ends its rest.

host/hos_thread.c holds a game thread that would use a whole processor for work no frame waits on (OoT's
OptimizeChangeBank) to a duty, a percentage of one processor: at its loop-header polls and SVCs (hos_bg_gate) the thread
compares the processor time it used with the time that passed and rests when it is over. The fixture runs the real code
cut from the source (the HosThread struct, and from the constants to hos_bg_hurry) over a clock the test moves: running
adds to the clock and to the thread's processor time, blocking and resting to the clock alone.

- a thread that never blocks ends at its duty, and gives its virtual core up for every rest;
- a thread that uses less than its duty by itself never rests;
- a second spent idle does not pay for a burst after it;
- a waiter for a lock the thread holds ends the rest at once, and the thread then runs unheld for BG_HURRY_NS, whether
  the waiter names the holder by its pointer (nn::os mutexes) or by its Horizon handle (svcArbitrateLock);
- a waiter that comes just before the rest keeps it from starting;
- a holder that no longer exists is not touched.

Two broken variants (no rest at all; no time unheld after a waiter came) must each fail.

python -I -B tests/test_background_share.py [--cc gcc]
"""
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
FIRST = "enum { BG_SLICE_NS"
AFTER = "static DWORD WINAPI bg_ticker("


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + (";" if source[end:end + 1] == ";" else "")


FIXTURE = r"""
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef void *HANDLE;
typedef int BOOL;
typedef long LONG;
typedef unsigned long DWORD;
typedef struct { void *p; } SRWLOCK, *PSRWLOCK;
typedef struct { void *p; } CONDITION_VARIABLE;
#define SRWLOCK_INIT {0}
#define HOS_TLS_SLOTS 32
typedef struct { uint64_t x[32]; volatile int poll; } Ctx;
@STRUCT@
typedef struct HosThread HosThread;
static HosThread *cur, *threads[8];
static int nthreads;
static SRWLOCK threads_lock = SRWLOCK_INIT;
static void AcquireSRWLockShared(PSRWLOCK l) { (void)l; }
static void ReleaseSRWLockShared(PSRWLOCK l) { (void)l; }
static void hos_log(const char *fmt, ...) { (void)fmt; }

/* the clock, and the processor time of the one thread under test */
static int64_t now_ns = 1000000000, used_ns;
static int64_t hos_now_ns(void) { return now_ns; }
static void bg_usage(int64_t *used, int64_t *wall) { *used = used_ns, *wall = now_ns; }
static void run(int64_t ns) { now_ns += ns, used_ns += ns; }
static void idle(int64_t ns) { now_ns += ns; }

static int aways, backs;
static int hos_vcore_away(void) { aways++; return 1; }
static void hos_vcore_back(void) { backs++; }

/* a rest: the event's wait. A waiter for a lock can come waiter_after_ms into it (by pointer, or by handle) */
static int rests, signaled, waiter_after_ms = -1, waiter_by_handle;
static int64_t last_rest_ns;
void hos_bg_hurry(HosThread *holder, uint32_t handle);
static BOOL SetEvent(HANDLE h) { (void)h; signaled = 1; return 1; }
static DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    (void)h;
    rests++;
    const int64_t t0 = now_ns;
    if (!signaled && waiter_after_ms >= 0 && (DWORD)waiter_after_ms < ms) {
        now_ns += (int64_t)waiter_after_ms * 1000000;
        if (waiter_by_handle) hos_bg_hurry(NULL, cur->kernel_handle);
        else hos_bg_hurry(cur, 0);
        waiter_after_ms = -1;
    }
    if (signaled) signaled = 0;
    else now_ns += (int64_t)ms * 1000000;
    last_rest_ns = now_ns - t0;
    return 0;
}
@CODE@

static HosThread t;
static void fresh(int duty) {
    memset(&t, 0, sizeof t);
    t.bg_duty = duty, t.bg_wake = (HANDLE)&t, t.kernel_handle = 0x10042;
    cur = &t, threads[0] = &t, nthreads = 1, bg_threads = 1;
    rests = signaled = aways = backs = 0, waiter_after_ms = -1, waiter_by_handle = 0;
}
/* the thread runs without a block for `ns` of its own processor time, a poll every 100 us */
static void spin(int64_t ns) {
    for (int64_t done = 0; done < ns; done += 100000) { run(100000); hos_bg_gate(); }
}
static void spin_until_rest(void) {
    const int before = rests;
    for (int i = 0; i < 100000 && rests == before; i++) { run(100000); hos_bg_gate(); }
    CHECK(rests == before + 1);
}
static void waiter_case(int by_handle) {
    fresh(25);
    spin(100000000);
    waiter_after_ms = 1, waiter_by_handle = by_handle;
    spin_until_rest();
    CHECK(last_rest_ns == 1000000);          /* the rest ended when the waiter came, not after its 6 ms */
    CHECK(t.bg_st_hurries == 1);
    const int before = rests;
    spin(BG_HURRY_NS - 200000);
    CHECK(rests == before);                  /* and the holder runs unheld for BG_HURRY_NS */
    spin(20000000);
    CHECK(rests > before);                   /* then it is held again */
}
int main(void) {
    /* a thread that never blocks: its duty, to within a tenth of it, each rest with its core given up */
    for (int duty = 20; duty <= 50; duty += 15) {
        fresh(duty);
        const int64_t t0 = now_ns, u0 = used_ns;
        spin(500000000);
        const double share = (double)(used_ns - u0) / (double)(now_ns - t0) * 100;
        CHECK(share > duty * 0.9 && share < duty * 1.1);
        CHECK(rests > 20 && aways == rests && backs == rests);
    }
    /* a thread that blocks by itself and uses a tenth of a processor: never rested at a duty of 25 */
    fresh(25);
    for (int i = 0; i < 2000; i++) { run(100000); hos_bg_gate(); idle(900000); hos_bg_gate(); }
    CHECK(rests == 0);
    /* an idle second, then a burst: the burst is held from its start (within 30 ms it has rested, and over 200 ms it
       had no more than its duty and what the first slices allow) */
    fresh(25);
    spin(10000000);
    idle(1000000000);
    hos_bg_gate();
    const int before = rests;
    const int64_t t0 = now_ns, u0 = used_ns;
    while (now_ns - t0 < 30000000) { run(100000); hos_bg_gate(); }
    CHECK(rests > before);
    while (now_ns - t0 < 200000000) { run(100000); hos_bg_gate(); }
    CHECK((double)(used_ns - u0) / (double)(now_ns - t0) < 0.30);
    /* a waiter for its lock, named by pointer and by handle */
    waiter_case(0);
    waiter_case(1);
    /* a waiter that came just before the rest: no rest starts while its time runs */
    fresh(25);
    spin(100000000);
    {
        const int r0 = rests;
        hos_bg_hurry(&t, 0);
        CHECK(!signaled);                    /* (it was not resting: nothing to end) */
        spin(BG_HURRY_NS - 200000);
        CHECK(rests == r0);
    }
    /* a holder read from a lock that is no thread any more is not touched (the pointer is never followed) */
    fresh(25);
    hos_bg_hurry((HosThread *)(uintptr_t)16, 0);
    hos_bg_hurry(NULL, 0x7777);
    CHECK(t.bg_hurry_until == 0);
    /* no thread held: a lock's waiter looks nothing up */
    bg_threads = 0;
    hos_bg_hurry(&t, 0);
    CHECK(t.bg_hurry_until == 0);
    /* the rest: none within the duty or for a duty of 0 or 100, BG_REST_MAX_NS at most */
    CHECK(bg_rest_ns(1000, 4000, 4000000, 25) == 0);
    CHECK(bg_rest_ns(2000, 2000, 2000000, 25) == 6000000);
    CHECK(bg_rest_ns(2000, 2000, 2000000, 0) == 0 && bg_rest_ns(2000, 2000, 2000000, 100) == 0);
    CHECK(bg_rest_ns(50000, 50000, 50000000, 10) == BG_REST_MAX_NS);
    /* a duty too low for the longest rest: a slice in every slice and longest rest, no less */
    fresh(5);
    {
        const int64_t t0 = now_ns, u0 = used_ns;
        spin(200000000);
        const double share = (double)(used_ns - u0) / (double)(now_ns - t0) * 100;
        CHECK(share > 15 && share < 19);
    }
    puts("ok");
    return 0;
}
"""


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--source", type=Path, default=ROOT / "host/hos_thread.c")
    args = parser.parse_args()
    src = args.source.read_text(encoding="utf-8")
    check(FIRST in src and AFTER in src, "host/hos_thread.c has no background-thread block (%s ... %s)" % (FIRST, AFTER))
    code = src[src.index(FIRST):src.index(AFTER)]
    struct = block(src, "struct HosThread {")
    compiler = shutil.which(args.cc)
    check(compiler, "C compiler not found: " + args.cc)

    def build(folder, name, text):
        source = folder / (name + ".c")
        source.write_text(FIXTURE.replace("@STRUCT@", struct).replace("@CODE@", text), encoding="utf-8")
        exe = folder / (name + (".exe" if os.name == "nt" else ""))
        subprocess.run([compiler, "-std=gnu11", "-O1", "-w", str(source), "-o", str(exe)], check=True, timeout=120)
        return exe

    def broken(folder, name, old, new, what):
        check(old in code, "the block no longer has the line the broken variant '%s' changes" % name)
        r = subprocess.run([str(build(folder, name, code.replace(old, new)))], capture_output=True, text=True, timeout=60)
        check(r.returncode != 0 and "FAIL:" in r.stderr, what)

    with tempfile.TemporaryDirectory(prefix="background-share-") as tmp:
        folder = Path(tmp)
        r = subprocess.run([str(build(folder, "fixture", code))], capture_output=True, text=True, timeout=60)
        check(r.returncode == 0 and "ok" in r.stdout, "a background thread is not held to its share\n" + r.stderr)
        broken(folder, "no-rest", "if (missing <= 0) return 0;", "return 0;",
               "a thread that never rests passes: the share is not checked")
        broken(folder, "no-hurry", "hos_now_ns() + BG_HURRY_NS, __ATOMIC_SEQ_CST);", "0, __ATOMIC_SEQ_CST);",
               "a holder held again at once after its waiter came passes")
    print("ok")


if __name__ == "__main__":
    main()
