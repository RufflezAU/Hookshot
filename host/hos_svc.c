/* Horizon supervisor calls that lifted SDK code still makes (the game itself has none; the SDK has ~60 wrappers).
   The HLE above nn:: should keep most of them from ever running: what reaches here is either trivial
   (debug output, ticks, info queries) or a sign that an SDK function above it wants an HLE. Unknown ones stop the
   run with the svc number and the caller, so the log says what to do next. Numbers: switchbrew.org "SVC". */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define R_OK 0
/* (mkw-recomp) the region of GetInfo 29/30: 128 GB at 192 GB, between the heap (up from 64 GB) and the address
   regions (down from 512 GB); the SDK's VAMM backs pieces of it with MapPhysicalMemory */
#define HOS_NX2_REGION_BASE 0x3000000000ULL
#define HOS_NX2_REGION_SIZE 0x2000000000ULL
#define R_INVALID_ENUM 0xF001   /* svc::ResultInvalidEnumValue */

static uint64_t get_info(uint32_t id0, uint64_t id1, int *ok) {
    *ok = 1;
    switch (id0) {
    case 0: return (1u << hos_cores()) - 1;               /* CoreMask: cores 0 to hos_cores() - 1 */
    case 1: return 0xFFFFFFFFFFFFFFFFULL;                  /* PriorityMask */
    case 2: return HOS_HEAP_BASE;                          /* AliasRegionAddress */
    case 3: return HOS_SPACE_END - HOS_HEAP_BASE;          /* AliasRegionSize */
    case 4: return HOS_HEAP_BASE;                          /* HeapRegionAddress */
    case 5: return HOS_SPACE_END - HOS_HEAP_BASE;          /* HeapRegionSize */
    case 6: return hos_total_memory;                       /* TotalMemorySize */
    case 7: return hos_program_size + hos_heap_size;       /* UsedMemorySize */
    case 8: return 0;                                      /* DebuggerAttached */
    case 11: return 0x5A3C9E1F0B7D2468ULL ^ (id1 * 0x9E3779B97F4A7C15ULL);   /* RandomEntropy */
    case 12: return 0x08000000ULL;                         /* AslrRegionAddress */
    case 13: return HOS_SPACE_END - 0x08000000ULL;         /* AslrRegionSize */
    case 14: return HOS_SYSTEM_BASE;                       /* StackRegionAddress */
    case 15: return HOS_SYSTEM_END - HOS_SYSTEM_BASE;      /* StackRegionSize */
    case 16: case 17: return 0;                            /* SystemResourceSize total / used */
    case 18: return hos_program_id;                        /* ProgramId (main.npdm's, gen/hos_image.c) */
    case 20: {   /* UserExceptionContextAddress: (hookshot) OoT's nn::os::SetDyingMessageRegionPrivate writes its
                    dying-message region into it; a zeroed page, made once */
        static uint64_t ctx_page;
        if (!ctx_page) ctx_page = hos_sys_alloc(0x1000, 0x1000);
        return ctx_page;
    }
    case 21: return hos_total_memory;                      /* TotalNonSystemMemorySize */
    case 22: return hos_program_size + hos_heap_size;      /* UsedNonSystemMemorySize */
    case 23: return 1;                                     /* IsApplication */
    /* the Switch 2 SDK's (ids from Eglantine's svc_types.h / svc_info.cpp) */
    case 24: return 256;                                   /* FreeThreadCount */
    case 28: return 0;                                     /* AliasRegionExtraSize */
    case 29: return HOS_NX2_REGION_BASE;                   /* the VAMM region nnosInitialize asks for */
    case 30: return HOS_NX2_REGION_SIZE;
    case 31: case 32: return 0;                            /* remote memory (the graphics stack's): size, used */
    case 33: return 0x1000;                                /* the page granule VAMM maps in */
    case 34: return 0;                                     /* TransferMemoryHint */
    case 37: case 38: return 0;
    default: *ok = 0; return 0;
    }
}

/* ---- mutex arbitration and process-wide condition variables -------------------------------------------------
   Horizon's user-mode mutex: the word holds the owner's handle, bit 30 means "has waiters". A thread that loses the
   race sets bit 30 and calls ArbitrateLock; the owner's ArbitrateUnlock hands the word straight to the
   highest-priority waiter. A condition variable wait releases the mutex and sleeps on the key; a signal moves a
   waiter back to its mutex, or gives it the mutex if free (switchbrew "SVC", libnx mutex.c / condvar.c). */
#define HANDLE_WAIT_MASK 0x40000000u
#define R_TIMED_OUT 0xEA01
#define SVC_BUCKETS 256

typedef struct Waiter {
    HosThread *t;
    uint64_t mutex, key;
    uint32_t tag;
    int on_key;
    struct Waiter *prev, *next;
} Waiter;
typedef struct {
    /* (the lock a cache line of its own: the member's alignment pads the struct to 64 bytes, so no two
       buckets share one and two threads on unrelated mutexes do not write each other's line) */
    _Alignas(64) SRWLOCK lk;
    Waiter *head, *tail;
} SvcBucket;
static SvcBucket buckets[SVC_BUCKETS];

static unsigned bucket_idx(uint64_t addr) { return (unsigned)((addr >> 3) ^ (addr >> 11) ^ (addr >> 19)) & (SVC_BUCKETS - 1); }
static void bucket_lock2(unsigned a, unsigned b) {
    if (a == b) AcquireSRWLockExclusive(&buckets[a].lk);
    else if (a < b) { AcquireSRWLockExclusive(&buckets[a].lk); AcquireSRWLockExclusive(&buckets[b].lk); }
    else { AcquireSRWLockExclusive(&buckets[b].lk); AcquireSRWLockExclusive(&buckets[a].lk); }
}
static void bucket_unlock2(unsigned a, unsigned b) {
    if (a == b) ReleaseSRWLockExclusive(&buckets[a].lk);
    else { ReleaseSRWLockExclusive(&buckets[a].lk); ReleaseSRWLockExclusive(&buckets[b].lk); }
}
static void list_push(SvcBucket *bk, Waiter *w) {
    w->prev = bk->tail;
    w->next = NULL;
    if (bk->tail) bk->tail->next = w;
    else bk->head = w;
    bk->tail = w;
}
static void list_remove(SvcBucket *bk, Waiter *w) {
    if (w->prev) w->prev->next = w->next;
    else bk->head = w->next;
    if (w->next) w->next->prev = w->prev;
    else bk->tail = w->prev;
    w->prev = w->next = NULL;
}

static uint32_t word_get(uint64_t a) { return __atomic_load_n((uint32_t *)GP(a), __ATOMIC_SEQ_CST); }
static void word_set(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)GP(a), v, __ATOMIC_SEQ_CST); }
/* User mode changes a mutex word with exclusives (compare-and-swap here) while the kernel holds the bucket lock: a kernel
   read-modify-write of a word user mode may change must be a compare-and-swap too. */
static int word_cas(uint64_t a, uint32_t expect, uint32_t v) {
    return __atomic_compare_exchange_n((uint32_t *)GP(a), &expect, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* The best waiter (lowest priority value, then oldest) for a mutex (on_key 0) or a key (on_key 1) or address (2). */
static Waiter *best_waiter(SvcBucket *bk, uint64_t addr, int on_key, int *others) {
    Waiter *best = NULL;
    *others = 0;
    for (Waiter *w = bk->head; w; w = w->next) {
        if (w->on_key != on_key || (on_key ? w->key : w->mutex) != addr) continue;
        if (!best) best = w;
        else {
            (*others)++;
            if (hos_thread_prio(w->t) < hos_thread_prio(best->t)) best = w;
        }
    }
    return best;
}

/* Hand a held mutex to its next waiter, or free it. Caller holds buckets[bucket_idx(mutex)].lk. */
static void release_locked(SvcBucket *bk, uint64_t mutex) {
    int others;
    Waiter *w = best_waiter(bk, mutex, 0, &others);
    if (!w) { word_set(mutex, 0); return; }
    HosThread *t = w->t;
    uint32_t tag = w->tag;
    list_remove(bk, w);
    word_set(mutex, tag | (others ? HANDLE_WAIT_MASK : 0));
    hos_thread_wake(t);
}

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__EMSCRIPTEN__)
#include <emmintrin.h>
static int spin_for_release(uint64_t mutex, uint32_t held) {
    /* a counted spin, not a timed one: a clock call on every miss cost more than the wait it saved.
       HOS_SPIN_US still says how long to spin, in microseconds' worth of pauses (~64 a pause cycle); 0: no spin.
       The default is the 20 us this build has always spun: at 1 us (64 pauses) the Deck re-entered the kernel
       for every contended mutex and the frame went 36 -> 45 ms (measured, both halves of the frame slower). */
    static int iters = -1;
    if (iters < 0) {
        const char *e = getenv("HOS_SPIN_US");
        const int us = e ? atoi(e) : 20;
        iters = us <= 0 ? 0 : us > 1000 ? 64000 : us * 64;
    }
    if (!iters) return 0;
    static int misses[256], tries[256];
    const unsigned slot = (unsigned)(mutex >> 3 ^ mutex >> 11) & 255;
    const int m = __atomic_load_n(&misses[slot], __ATOMIC_RELAXED);
    if (m >= 4 && (__atomic_add_fetch(&tries[slot], 1, __ATOMIC_RELAXED) & 31)) return 0;
    for (int i = 0; i < 64; i++) {
        if (word_get(mutex) != held) {
            __atomic_store_n(&misses[slot], 0, __ATOMIC_RELAXED);
            return 1;
        }
        _mm_pause();
    }
    if (m < 4) __atomic_store_n(&misses[slot], m + 1, __ATOMIC_RELAXED);
    return 0;
}
#else
static int spin_for_release(uint64_t mutex, uint32_t held) { (void)mutex; (void)held; return 0; }
#endif

/* svcArbitrateLock(owner handle, mutex address, own tag) */
static uint32_t arbitrate_lock(uint32_t owner, uint64_t mutex, uint32_t tag) {
    HosThread *self = hos_self();
    if (spin_for_release(mutex, owner | HANDLE_WAIT_MASK)) return 0;
    SvcBucket *bk = &buckets[bucket_idx(mutex)];
    AcquireSRWLockExclusive(&bk->lk);
    if (word_get(mutex) != (owner | HANDLE_WAIT_MASK)) {   /* released meanwhile: let user mode retry */
        ReleaseSRWLockExclusive(&bk->lk);
        return 0;
    }
    Waiter w = {self, mutex, 0, tag, 0, NULL, NULL};
    list_push(bk, &w);
    ReleaseSRWLockExclusive(&bk->lk);
    int to;
    hos_thread_wait(self, -1, &to);   /* woken once release_locked made us the owner */
    return 0;
}

/* svcArbitrateUnlock(mutex address) */
static uint32_t arbitrate_unlock(uint64_t mutex) {
    SvcBucket *bk = &buckets[bucket_idx(mutex)];
    AcquireSRWLockExclusive(&bk->lk);
    release_locked(bk, mutex);
    ReleaseSRWLockExclusive(&bk->lk);
    return 0;
}

/* svcWaitProcessWideKeyAtomic(mutex address, key address, own tag, timeout ns) */
static uint32_t wait_key(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout) {
    HosThread *self = hos_self();
    const unsigned mb = bucket_idx(mutex), kb = bucket_idx(key);
    bucket_lock2(mb, kb);
    word_set(key, 1);
    Waiter w = {self, mutex, key, tag, 1, NULL, NULL};
    list_push(&buckets[kb], &w);
    release_locked(&buckets[mb], mutex);
    bucket_unlock2(mb, kb);
    int to;
    hos_thread_wait(self, timeout, &to);
    if (!to) return 0;
    AcquireSRWLockExclusive(&buckets[kb].lk);
    if (w.on_key == 1 && (w.prev || w.next || buckets[kb].head == &w)) {   /* still waiting on the key: real timeout */
        list_remove(&buckets[kb], &w);
        ReleaseSRWLockExclusive(&buckets[kb].lk);
        return R_TIMED_OUT;
    }
    ReleaseSRWLockExclusive(&buckets[kb].lk);
    hos_thread_wait(self, -1, &to);
    return 0;
}

/* svcSignalProcessWideKey(key address, count): count <= 0 wakes all */
static void signal_key(uint64_t key, int32_t count) {
    const unsigned kb = bucket_idx(key);
    SvcBucket *kbk = &buckets[kb];
    for (int n = 0; count <= 0 || n < count; ) {
        AcquireSRWLockExclusive(&kbk->lk);
        int others;
        Waiter *w = best_waiter(kbk, key, 1, &others);
        if (!w) { ReleaseSRWLockExclusive(&kbk->lk); break; }
        const uint64_t mutex = w->mutex;
        const unsigned mb = bucket_idx(mutex);
        if (mb != kb) {
            ReleaseSRWLockExclusive(&kbk->lk);
            bucket_lock2(kb, mb);
            w = best_waiter(kbk, key, 1, &others);
            if (!w || w->mutex != mutex) { bucket_unlock2(kb, mb); continue; }
        }
        n++;
        SvcBucket *mbk = &buckets[mb];
        for (;;) {
            const uint32_t cur = word_get(mutex);
            if (cur == 0) {                           /* mutex free: it is theirs */
                if (!word_cas(mutex, 0, w->tag)) continue;
                HosThread *t = w->t;
                list_remove(kbk, w);
                hos_thread_wake(t);
            } else {                                  /* queue on the mutex */
                if (!(cur & HANDLE_WAIT_MASK) && !word_cas(mutex, cur, cur | HANDLE_WAIT_MASK)) continue;
                list_remove(kbk, w);
                w->on_key = 0;
                list_push(mbk, w);
            }
            break;
        }
        bucket_unlock2(kb, mb);
    }
    AcquireSRWLockExclusive(&kbk->lk);
    int others;
    if (!best_waiter(kbk, key, 1, &others)) word_set(key, 0);
    ReleaseSRWLockExclusive(&kbk->lk);
}

#define R_INVALID_STATE 0xFA01   /* svc::ResultInvalidState: the value did not match */
static int addr_waiters(SvcBucket *bk, uint64_t addr) {
    int n = 0;
    for (Waiter *w = bk->head; w; w = w->next) if (w->on_key == 2 && w->key == addr) n++;
    return n;
}
static void wake_address_locked(SvcBucket *bk, uint64_t addr, int32_t count) {
    for (int n = 0; count <= 0 || n < count; n++) {
        int others;
        Waiter *w = best_waiter(bk, addr, 2, &others);
        if (!w) break;
        HosThread *t = w->t;
        list_remove(bk, w);
        hos_thread_wake(t);
    }
}
static uint32_t wait_address(uint64_t addr, uint32_t type, uint64_t value, int64_t timeout) {
    HosThread *self = hos_self();
    SvcBucket *bk = &buckets[bucket_idx(addr)];
    AcquireSRWLockExclusive(&bk->lk);
    int wait;
    switch (type) {
    case 0: wait = (int32_t)word_get(addr) < (int32_t)value; break;
    case 1: {
        uint32_t cur;
        for (;;) {
            cur = word_get(addr);
            if ((int32_t)cur >= (int32_t)value) { wait = 0; break; }
            if (word_cas(addr, cur, cur - 1)) { wait = 1; break; }
        }
        break;
    }
    case 2: wait = (int32_t)word_get(addr) == (int32_t)value; break;
    case 3: wait = __atomic_load_n((uint64_t *)GP(addr), __ATOMIC_SEQ_CST) == value; break;
    default:
        ReleaseSRWLockExclusive(&bk->lk);
        hos_log("svcWaitForAddress: arbitration type %u", type);
        return R_INVALID_ENUM;
    }
    if (!wait) { ReleaseSRWLockExclusive(&bk->lk); return R_INVALID_STATE; }
    if (timeout == 0) { ReleaseSRWLockExclusive(&bk->lk); return R_TIMED_OUT; }
    Waiter w = {self, 0, addr, 0, 2, NULL, NULL};
    list_push(bk, &w);
    ReleaseSRWLockExclusive(&bk->lk);
    int to;
    hos_thread_wait(self, timeout, &to);
    if (!to) return 0;
    AcquireSRWLockExclusive(&bk->lk);
    if (w.prev || w.next || bk->head == &w) {   /* still queued: a real timeout */
        list_remove(bk, &w);
        ReleaseSRWLockExclusive(&bk->lk);
        return R_TIMED_OUT;
    }
    ReleaseSRWLockExclusive(&bk->lk);
    hos_thread_wait(self, -1, &to);   /* signaled meanwhile: take the pending wake-up */
    return 0;
}
static uint32_t signal_address(uint64_t addr, uint32_t type, int32_t value, int32_t count) {
    SvcBucket *bk = &buckets[bucket_idx(addr)];
    AcquireSRWLockExclusive(&bk->lk);
    uint32_t r = 0;
    if (type == 1) {
        if (!word_cas(addr, (uint32_t)value, (uint32_t)value + 1)) r = R_INVALID_STATE;
    } else if (type == 2) {
        const int n = addr_waiters(bk, addr);
        int32_t nv;
        if (count <= 0) nv = n > 0 ? value - 2 : value + 1;
        else if (n > 0) {
            const int more = n - 1 < count + 1 ? n - 1 : count + 1;
            nv = more == 0 ? value + 1 : more <= count ? value - 1 : value;
        } else nv = value + 1;
        if (!word_cas(addr, (uint32_t)value, (uint32_t)nv)) r = R_INVALID_STATE;
    } else if (type != 0) {
        ReleaseSRWLockExclusive(&bk->lk);
        hos_log("svcSignalToAddress: signal type %u", type);
        return R_INVALID_ENUM;
    }
    if (!r) wake_address_locked(bk, addr, count);
    ReleaseSRWLockExclusive(&bk->lk);
    return r;
}

void hos_kernel_dump(void) {
    int total = 0;
    for (int b = 0; b < SVC_BUCKETS; b++) {
        AcquireSRWLockExclusive(&buckets[b].lk);
        for (Waiter *w = buckets[b].head; w; w = w->next) total++;
        ReleaseSRWLockExclusive(&buckets[b].lk);
    }
    hos_log("kernel: %d threads waiting", total);
    for (int b = 0; b < SVC_BUCKETS; b++) {
        AcquireSRWLockExclusive(&buckets[b].lk);
        for (Waiter *w = buckets[b].head; w; w = w->next) {
            hos_log("kernel:   %s (tag %#x) on %s %#llx: mutex %#llx = %#x, key %#llx = %#x", hos_thread_name(w->t), w->tag,
                    w->on_key ? "key" : "mutex", (unsigned long long)(w->on_key ? w->key : w->mutex),
                    (unsigned long long)w->mutex, w->mutex ? word_get(w->mutex) : 0, (unsigned long long)w->key,
                    w->key ? word_get(w->key) : 0);
        }
        ReleaseSRWLockExclusive(&buckets[b].lk);
    }
    void hos_thread_log_handles(void);
    hos_thread_log_handles();
}

/* svcQueryMemory: MemoryInfo { u64 addr, size; u32 state, attr, perm, ipc_refs, device_refs, pad } */
extern const uint64_t hos_image_end;
static void query_memory(uint64_t out, uint64_t addr) {
    uint64_t base = 0, size = 0;
    uint32_t state = 0, perm = 0;   /* Free */
    uint64_t stack, stack_size;
    hos_thread_stack(hos_self(), &stack, &stack_size);
    if (addr >= stack && addr < stack + stack_size) { base = stack; size = stack_size; state = 0x0B; perm = 3; }   /* Stack */
    else if (addr >= hos_image_base && addr < hos_image_end) { base = hos_image_base; size = hos_image_end - base; state = 0x03; perm = 5; }   /* Code */
    else if (addr >= HOS_HEAP_BASE && addr < HOS_HEAP_BASE + hos_heap_size) { base = HOS_HEAP_BASE; size = hos_heap_size; state = 0x05; perm = 3; }   /* Normal */
    else if (addr >= HOS_SYSTEM_BASE && addr < HOS_SYSTEM_END) { base = HOS_SYSTEM_BASE; size = HOS_SYSTEM_END - HOS_SYSTEM_BASE; state = 0x05; perm = 3; }
    else { base = addr & ~0xFFFULL; size = 0x1000; }   /* Free: one page, enough for callers probing an address */
    memset(GP(out), 0, 0x28);
    G64(out) = base;
    G64(out + 8) = size;
    G32(out + 16) = state;
    G32(out + 24) = perm;
}

/* A thread handle, with Horizon's pseudo-handle for the current thread. */
#define CUR_THREAD 0xFFFF8000u
static HosThread *thread_handle(uint32_t h) { return h == CUR_THREAD ? hos_self() : hos_thread_by_handle(h); }

void a64_svc(Ctx *c, uint32_t imm) {
    hos_vcore_checkpoint();
    switch (imm) {
    case 0x01: { /* SetHeapSize(-, size) -> (result, heap address) */
        int hos_heap_size_fits(uint64_t size);   /* (hookshot, hos_mem.c: the heap stays in the guest window) */
        if (!hos_heap_size_fits(c->x[1])) {
            c->x[0] = 0xCA01;   /* svc::ResultOutOfMemory */
            return;
        }
        hos_heap_size = c->x[1];
        c->x[0] = R_OK;
        c->x[1] = HOS_HEAP_BASE;
        return;
    }
    case 0x02:   /* SetMemoryPermission */
    case 0x03:   /* SetMemoryAttribute */
    case 0x16:   /* CloseHandle */
    case 0x2A:   /* FlushEntireDataCache */
    case 0x2B:   /* FlushDataCache */
        c->x[0] = R_OK;
        return;
    case 0x0A:   /* ExitThread */
        hos_vcore_away();
        ExitThread(0);
        return;
    case 0x07:   /* ExitProcess */
        hos_log("svcExitProcess");
        hos_log_flush();
        ExitProcess(0);
        return;
    case 0x0C: { /* GetThreadPriority(-, handle) -> (result, priority) */
        HosThread *t = thread_handle((uint32_t)c->x[1]);
        c->x[1] = t ? (uint32_t)hos_thread_prio(t) : 44;
        c->x[0] = t ? R_OK : 0xE401;   /* svc::ResultInvalidHandle */
        return;
    }
    case 0x0D: { /* SetThreadPriority(handle, priority) */
        HosThread *t = thread_handle((uint32_t)c->x[0]);
        if (t) hos_thread_priority(t, 1, (int)c->x[1]);
        c->x[0] = t ? R_OK : 0xE401;
        return;
    }
    case 0x0E: { /* GetThreadCoreMask(-, -, handle) -> (result, ideal core, affinity mask) */
        HosThread *t = thread_handle((uint32_t)c->x[2]);
        c->x[1] = t ? (uint32_t)hos_thread_core(t) : 0;
        c->x[2] = t && hos_thread_mask(t) ? hos_thread_mask(t) : 0x7;
        c->x[0] = t ? R_OK : 0xE401;
        return;
    }
    case 0x0F: { /* SetThreadCoreMask(handle, ideal core, mask): -1 keeps the ideal core, -2/-3 defaults */
        HosThread *t = thread_handle((uint32_t)c->x[0]);
        if (t) hos_thread_affinity(t, (int)c->x[1], c->x[2]);
        c->x[0] = t ? R_OK : 0xE401;
        return;
    }
    case 0x24:   /* GetProcessId(-, handle) -> (result, id) */
        c->x[1] = 0x51;
        c->x[0] = R_OK;
        return;
    case 0x06:   /* QueryMemory(MemoryInfo* out, -, address) -> (result, page info) */
        query_memory(c->x[0], c->x[2]);
        c->x[0] = R_OK;
        c->x[1] = 0;
        return;
    case 0x1A:   /* ArbitrateLock(owner handle, mutex, tag) */
        c->x[0] = arbitrate_lock((uint32_t)c->x[0], c->x[1], (uint32_t)c->x[2]);
        return;
    case 0x1B:   /* ArbitrateUnlock(mutex) */
        c->x[0] = arbitrate_unlock(c->x[0]);
        return;
    case 0x1C:   /* WaitProcessWideKeyAtomic(mutex, key, tag, timeout) */
        c->x[0] = wait_key(c->x[0], c->x[1], (uint32_t)c->x[2], (int64_t)c->x[3]);
        return;
    case 0x34:   /* WaitForAddress(address, type, value, timeout) (hookshot) */
        c->x[0] = wait_address(c->x[0], (uint32_t)c->x[1], c->x[2], (int64_t)c->x[3]);
        return;
    case 0x35:   /* SignalToAddress(address, type, value, count) (hookshot) */
        c->x[0] = signal_address(c->x[0], (uint32_t)c->x[1], (int32_t)c->x[2], (int32_t)c->x[3]);
        return;
    case 0x1D:   /* SignalProcessWideKey(key, count) */
        signal_key(c->x[0], (int32_t)c->x[1]);
        return;
    case 0x0B: { /* SleepThread(ns) */
        void hos_sleep_logged(int64_t ns, uint64_t lr);
        extern _Thread_local int64_t hos_slept_ns[2];   /* (hos_thread.c: the FPS counter's Breakdown, while it shows) */
        extern volatile int hos_fps_timing;
        const int64_t t0 = hos_fps_timing ? hos_now_ns() : 0;
        hos_sleep_logged((int64_t)c->x[0], c->x[30] | 1);
        if (t0) hos_slept_ns[1] += hos_now_ns() - t0;
        return;
    }
    case 0x10:   /* GetCurrentProcessorNumber */
        c->x[0] = (uint32_t)hos_thread_core(hos_self());
        return;
    case 0x1E:   /* GetSystemTick */
        c->x[0] = hos_ticks();
        return;
    case 0x25: { /* GetThreadId(-, handle) -> (result, id) */
        HosThread *t = thread_handle((uint32_t)c->x[1]);
        c->x[1] = hos_thread_id(t ? t : hos_self());
        c->x[0] = R_OK;
        return;
    }
    case 0x26: { /* Break(reason, info, size) */
        hos_fatal(c, "svcBreak(reason %#llx, info %#llx, size %#llx)", (unsigned long long)c->x[0],
                  (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
        return;
    }
    case 0x27: { /* OutputDebugString(str, size) */
        uint64_t n = c->x[1] > 4096 ? 4096 : c->x[1];
        char buf[4097];
        memcpy(buf, GP(c->x[0]), n);
        buf[n] = 0;
        hos_log("debug: %s", buf);
        c->x[0] = R_OK;
        return;
    }
    case 0x2C:   /* MapPhysicalMemory(address, size) */
        hos_commit(c->x[0], c->x[1]);
        if (hos_trace) hos_log("MapPhysicalMemory(%#llx, %#llx)", (unsigned long long)c->x[0], (unsigned long long)c->x[1]);
        c->x[0] = R_OK;
        return;
    case 0x2D:   /* UnmapPhysicalMemory(address, size): the pages read as zero if mapped again */
        if (!hos_guest_range(c->x[0], c->x[1])) {   /* (hookshot: guest pages only) */
            c->x[0] = 0xCA01;
            return;
        }
        {   /* (pages under a Vulkan import stay committed, zeroed: hos_mem.c FreeMemoryPages) */
            extern int rx_guest_imported(uint64_t lo, uint64_t hi);
            if (rx_guest_imported(c->x[0], c->x[0] + c->x[1])) memset(GP(c->x[0]), 0, (size_t)c->x[1]);
            else VirtualFree(GP(c->x[0]), (SIZE_T)c->x[1], MEM_DECOMMIT);
        }
        c->x[0] = R_OK;
        return;
    case 0x1F: { /* ConnectToNamedPort(-, name) -> (result, session handle): (hookshot) SDK code that resolves a
                    service through "sm:" (nn::sm::Initialize, from nn::nsd's HIPC client in OoT) gets a session
                    whose requests svc 0x21 answers empty, as for any service the runtime does not emulate. */
        static volatile LONG next = 0;
        const char *name = c->x[1] ? (const char *)GP(c->x[1]) : "";
        uint32_t h = 0x7E000000u | (uint32_t)InterlockedIncrement(&next);
        hos_log("ipc: ConnectToNamedPort(\"%.11s\") -> session %#x (requests answered empty) (lr %#llx)", name, h,
                (unsigned long long)c->x[30]);
        c->x[0] = R_OK;
        c->x[1] = h;
        return;
    }
    case 0x21: { /* SendSyncRequest(session handle): (mkw-recomp) a service the runtime does not emulate. The request
                    is in the thread's IPC buffer (TLS + 0); the answer is a CMIF success with zeroed outputs, as
                    Eglantine's auto-stub does, so a lifted service client sees an empty, successful reply. */
        uint8_t *tls = (uint8_t *)GP(c->tpidrro_el0);
        const uint32_t w0 = *(uint32_t *)tls, w1 = *(uint32_t *)(tls + 4);
        /* the request's CMIF header ('SFCI', version, command id) after the HIPC header, 16-byte aligned */
        uint32_t cmd = 0;
        for (int off = 8; off < 0x40; off += 4)
            if (*(uint32_t *)(tls + off) == 0x49434653u) { cmd = *(uint32_t *)(tls + off + 8); break; }
        static volatile LONG logged;
        if (InterlockedIncrement(&logged) <= 64)
            hos_log("ipc: handle %#x command %u (hipc %08x %08x, lr %#llx): answered empty", (uint32_t)c->x[0], cmd,
                    w0, w1, (unsigned long long)c->x[30]);
        memset(tls, 0, 0x100);
        *(uint32_t *)(tls + 0) = 0;                 /* HIPC: type 0, no descriptors */
        *(uint32_t *)(tls + 4) = 16;                /* 16 raw data words, no handle descriptor */
        *(uint32_t *)(tls + 16) = 0x4F434653u;      /* CMIF response 'SFCO', version 0, result 0 */
        c->x[0] = R_OK;
        return;
    }
    case 0x29: { /* GetInfo(id0, handle, id1) -> (result, value) */
        int ok;
        uint64_t v = get_info((uint32_t)c->x[1], c->x[3], &ok);
        if (!ok) hos_log("svcGetInfo: unknown id %#x/%#llx (lr %#llx)", (uint32_t)c->x[1],
                         (unsigned long long)c->x[3], (unsigned long long)c->x[30]);
        c->x[1] = v;
        c->x[0] = ok ? R_OK : R_INVALID_ENUM;
        return;
    }
    default:
        hos_fatal(c, "svc %#x not implemented (x0 %#llx x1 %#llx x2 %#llx)", imm, (unsigned long long)c->x[0],
                  (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
    }
}
