// Two symbols the C++ runtime asks for that the AppImage's sysroot does not have.
//
// This build links libstdc++ statically (-static-libstdc++, so the game folder runs where the system's
// is older). The libstdc++.a here came from a gcc 14 built against glibc 2.39, and the sysroot this
// links against is 2.35, so its objects carry two undefined references nothing in that libc defines:
//
//   __isoc23_strtoul  glibc 2.38's C23 strtoul (it also takes a 0b prefix). The 2.35 parse is the
//                     same for every number we read; no caller here passes "0b".
//   arc4random        glibc 2.36's generator, which libstdc++'s <random> uses to seed random_device.
//
// Both are three lines of definition, and hidden: they bind the reference inside this object and are
// not exported, so nothing else loaded into the game can end up calling them.
//
// (A build with a matching toolchain -- the AppImage's own, or one whose libstdc++ was built against
// the sysroot in use -- has no reference to either, and these are simply unused.)
#include <stdlib.h>

#define LOCAL __attribute__((visibility("hidden")))

extern "C" LOCAL unsigned long __isoc23_strtoul(const char *s, char **end, int base) {
    return strtoul(s, end, base);
}

extern "C" LOCAL unsigned int arc4random(void) {
    // (xorshift32: enough to seed a generator, and it never blocks, which is all <random> wants it for)
    static unsigned state = 0x9e3779b9u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
