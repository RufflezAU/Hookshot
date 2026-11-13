"""On Linux a condition variable's wake makes no system call when no thread is asleep on it, and loses no wake-up.

host/posix/win32_posix.c gives the host Windows' condition variables on a futex word. Windows' own wake does nothing
when nobody waits; the futex one was a system call every time, and the game signals events, queues and semaphores
thousands of times a frame, mostly with their waiters spinning or running. The condition variable now counts the threads
asleep on it and a wake calls the kernel only when there is one.

The fixture is the real code cut from the source (the type from host/posix/windows.h, and from rel_ms to the two sleeps
of win32_posix.c) over a futex made of a mutex and a condition, which counts its calls:

- 1,000 wakes of each kind with nobody asleep: no futex call;
- a thread asleep is woken by a wake of either kind, and a timed sleep that nobody wakes times out; after each the count
  of sleepers is 0 again, so later wakes are free again;
- InitializeConditionVariable leaves the count (see its comment in windows.h);
- four producers and four consumers pass 20,000 items through a one-slot buffer with untimed sleeps: a lost wake-up
  would leave it hanging (a watchdog ends the test after 60 s).

Two broken variants must each fail: a wake that always calls the kernel, and a sleeper that does not count itself.

python -I -B tests/test_posix_cv_wake.py [--cc gcc]
"""
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
TYPE = "typedef struct { volatile uint32_t seq, waiters; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;"
INIT = "static inline void InitializeConditionVariable(CONDITION_VARIABLE *cv) {"
FIRST = "static struct timespec rel_ms(DWORD ms) {"
AFTER = "void InitializeCriticalSection("

FIXTURE = r"""
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef int BOOL;
typedef unsigned long DWORD, ULONG;
#define TRUE 1
#define FALSE 0
#define INFINITE 0xFFFFFFFFu
#define ERROR_TIMEOUT 1460
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1
#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129
typedef pthread_rwlock_t SRWLOCK;
typedef pthread_mutex_t CRITICAL_SECTION;
static _Thread_local DWORD last_error;
@TYPE@
@INIT@

/* the futex: a wait sleeps while the word still has the value it was given, a wake wakes the sleepers. Counted. */
static pthread_mutex_t fmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fcv = PTHREAD_COND_INITIALIZER;
static unsigned long wake_gen;
static volatile int futex_wakes, futex_waits, asleep;
static long futex(volatile uint32_t *a, int op, uint32_t v, const struct timespec *ts) {
    pthread_mutex_lock(&fmu);
    if (op == FUTEX_WAKE_PRIVATE) {
        futex_wakes++, wake_gen++;
        pthread_cond_broadcast(&fcv);
        pthread_mutex_unlock(&fmu);
        return 0;
    }
    futex_waits++;
    if (*a != v) { pthread_mutex_unlock(&fmu); errno = EAGAIN; return -1; }
    struct timespec until;
    if (ts) {
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += ts->tv_sec, until.tv_nsec += ts->tv_nsec;
        if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
    }
    const unsigned long g = wake_gen;
    long r = 0;
    asleep++;
    while (wake_gen == g)
        if (!ts) pthread_cond_wait(&fcv, &fmu);
        else if (pthread_cond_timedwait(&fcv, &fmu, &until) == ETIMEDOUT) { errno = ETIMEDOUT; r = -1; break; }
    asleep--;
    pthread_mutex_unlock(&fmu);
    return r;
}
@CODE@

static void msleep(int ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
static void *watchdog(void *p) { (void)p; msleep(60000); fprintf(stderr, "FAIL: hung (a lost wake-up)\n"); exit(1); return NULL; }

/* one sleeper, woken by a wake of the kind asked for */
static SRWLOCK lk = PTHREAD_RWLOCK_INITIALIZER;
static CONDITION_VARIABLE cv = {0};
static volatile int ready, woke;
static void *sleeper(void *p) {
    (void)p;
    pthread_rwlock_wrlock(&lk);
    while (!ready) SleepConditionVariableSRW(&cv, &lk, 20000, 0);
    woke = 1;
    pthread_rwlock_unlock(&lk);
    return NULL;
}
static void wake_one_sleeper(int all) {
    pthread_t t;
    ready = woke = 0;
    pthread_create(&t, NULL, sleeper, NULL);
    while (!asleep) msleep(1);
    CHECK(cv.waiters == 1);
    pthread_rwlock_wrlock(&lk);
    ready = 1;
    pthread_rwlock_unlock(&lk);
    const int before = futex_wakes;
    if (all) WakeAllConditionVariable(&cv); else WakeConditionVariable(&cv);
    CHECK(futex_wakes == before + 1);
    pthread_join(t, NULL);
    CHECK(woke && cv.waiters == 0);
}

/* producers and consumers over one slot, each side with its own condition variable, as the game's queues are used */
enum { THREADS = 4, ITEMS = 5000 };
static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static CONDITION_VARIABLE not_empty = {0}, not_full = {0};
static int slot_full;
static long taken;
static void *producer(void *p) {
    (void)p;
    for (int i = 0; i < ITEMS; i++) {
        pthread_mutex_lock(&qmu);
        while (slot_full) SleepConditionVariableCS(&not_full, &qmu, INFINITE);
        slot_full = 1;
        pthread_mutex_unlock(&qmu);
        if (i & 1) WakeConditionVariable(&not_empty); else WakeAllConditionVariable(&not_empty);
    }
    return NULL;
}
static void *consumer(void *p) {
    (void)p;
    for (int i = 0; i < ITEMS; i++) {
        pthread_mutex_lock(&qmu);
        while (!slot_full) SleepConditionVariableCS(&not_empty, &qmu, INFINITE);
        slot_full = 0;
        taken++;
        pthread_mutex_unlock(&qmu);
        if (i & 1) WakeConditionVariable(&not_full); else WakeAllConditionVariable(&not_full);
    }
    return NULL;
}

int main(void) {
    pthread_t dog, th[2 * THREADS];
    pthread_create(&dog, NULL, watchdog, NULL);
    /* nobody asleep: no system call */
    for (int i = 0; i < 1000; i++) { WakeConditionVariable(&cv); WakeAllConditionVariable(&cv); }
    CHECK(futex_wakes == 0 && futex_waits == 0);
    /* a sleeper is woken by either kind of wake, and the wakes after it are free again */
    wake_one_sleeper(0);
    wake_one_sleeper(1);
    {
        const int before = futex_wakes;
        for (int i = 0; i < 1000; i++) { WakeConditionVariable(&cv); WakeAllConditionVariable(&cv); }
        CHECK(futex_wakes == before);
    }
    /* a timed sleep nobody wakes times out, and leaves no sleeper counted */
    pthread_rwlock_wrlock(&lk);
    CHECK(!SleepConditionVariableSRW(&cv, &lk, 30, 0) && last_error == ERROR_TIMEOUT && cv.waiters == 0);
    pthread_rwlock_unlock(&lk);
    /* initializing keeps the count of sleepers */
    cv.waiters = 3, cv.seq = 7;
    InitializeConditionVariable(&cv);
    CHECK(cv.waiters == 3 && cv.seq == 0);
    cv.waiters = 0;
    /* no wake-up lost under load */
    for (int i = 0; i < THREADS; i++) pthread_create(&th[i], NULL, producer, NULL);
    for (int i = 0; i < THREADS; i++) pthread_create(&th[THREADS + i], NULL, consumer, NULL);
    for (int i = 0; i < 2 * THREADS; i++) pthread_join(th[i], NULL);
    CHECK(taken == (long)THREADS * ITEMS && !slot_full && not_empty.waiters == 0 && not_full.waiters == 0);
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
    args = parser.parse_args()
    header = (ROOT / "host/posix/windows.h").read_text(encoding="utf-8")
    src = (ROOT / "host/posix/win32_posix.c").read_text(encoding="utf-8")
    check(TYPE in header, "host/posix/windows.h: the condition variable no longer counts its sleepers")
    check(INIT in header and FIRST in src and AFTER in src, "the condition variable code is not where this test cuts it")
    init = header[header.index(INIT):header.index("\n", header.index(INIT))]
    code = src[src.index(FIRST):src.index(AFTER)]
    compiler = shutil.which(args.cc)
    check(compiler, "C compiler not found: " + args.cc)

    def build(folder, name, text):
        source = folder / (name + ".c")
        source.write_text(FIXTURE.replace("@TYPE@", TYPE).replace("@INIT@", init).replace("@CODE@", text), encoding="utf-8")
        exe = folder / (name + (".exe" if os.name == "nt" else ""))
        subprocess.run([compiler, "-std=gnu11", "-O1", "-w", "-pthread", str(source), "-o", str(exe)], check=True, timeout=120)
        return exe

    def broken(folder, name, old, new, what):
        check(old in code, "the code no longer has the line the broken variant '%s' changes" % name)
        r = subprocess.run([str(build(folder, name, code.replace(old, new)))], capture_output=True, text=True, timeout=120)
        check(r.returncode != 0 and "FAIL:" in r.stderr, what)

    with tempfile.TemporaryDirectory(prefix="posix-cv-wake-") as tmp:
        folder = Path(tmp)
        r = subprocess.run([str(build(folder, "fixture", code))], capture_output=True, text=True, timeout=120)
        check(r.returncode == 0 and "ok" in r.stdout, "the condition variable's wakes are not as described\n" + r.stderr)
        broken(folder, "always", "if (__atomic_load_n(&cv->waiters, __ATOMIC_SEQ_CST)) futex(&cv->seq, FUTEX_WAKE_PRIVATE, 1, NULL);",
               "futex(&cv->seq, FUTEX_WAKE_PRIVATE, 1, NULL);", "a wake that always calls the kernel passes")
        broken(folder, "uncounted", "    __atomic_add_fetch(&cv->waiters, 1, __ATOMIC_SEQ_CST);\n", "",
               "a sleeper that does not count itself passes")
    print("ok")


if __name__ == "__main__":
    main()
