/* (hookshot) The toolchain's libstdc++.a (GCC 14) references glibc entry points the 2.35 sysroot the game links against
   does not export: __isoc23_strtoul (C23's strtoul) and arc4random (glibc 2.36's BSD random). Without them the module has
   unresolved symbols and would not load at all ("hookshot_vk.so has 2 unresolved symbol(s); it would not load"). Each is
   the older equivalent; nothing here changes a result the game can see. */
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

unsigned long __isoc23_strtoul(const char* s, char** end, int base) { return strtoul(s, end, base); }
unsigned long long __isoc23_strtoull(const char* s, char** end, int base) { return strtoull(s, end, base); }

/* (a xorshift, seeded once: the uses are ids and shuffles, not cryptography) */
static uint64_t arc_state;
static void arc_seed(void) {
    if (!arc_state) arc_state = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32) ^ 0x9E3779B97F4A7C15ull;
}
static uint64_t arc_next(void) {
    arc_seed();
    uint64_t x = arc_state;
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    arc_state = x;
    return x;
}
uint32_t arc4random(void) { return (uint32_t)(arc_next() >> 32); }
uint32_t arc4random_uniform(uint32_t bound) { return bound ? (uint32_t)(arc_next() % bound) : 0; }
void arc4random_buf(void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    while (n) {
        const uint64_t v = arc_next();
        const size_t k = n < 8 ? n : 8;
        __builtin_memcpy(p, &v, k);
        p += k, n -= k;
    }
}
