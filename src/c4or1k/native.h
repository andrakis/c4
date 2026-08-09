// native.h -- force-included (gcc -include) shim that lets the
// UNMODIFIED c4or1k sources compile as a native executable, skipping
// the C4 toolchain entirely (M14). Three jobs:
//
//  1. Real libc headers for the calls c4lc provides as builtins
//     (printf/malloc/memset/memcmp/open/read/close/exit).
//  2. `#define int long` -- the same trick c4.c itself uses (c4.c
//     line ~20): the c4lc dialect's `int` is host-pointer-width, and
//     the sources store pointers and full 64-bit intermediates in it.
//     Note the trap this does NOT fix: integer LITERALS stay 32-bit
//     C ints, so literal shifts that must produce 2^31/2^32 carry
//     explicit (int) casts at their definition sites (see cpu.c's
//     sext) -- grep '(int)1 <<' for the audited list. printf("%d",
//     long) reads the low 32 bits on x86-64 varargs, which is
//     exactly what c4m's own PRTF does with its 64-bit cells, so
//     output formatting matches the hosted builds.
//  3. The two non-libc builtins the emulator uses: __time()
//     (milliseconds, only feeds the instructions/sec report) and
//     __c4_cycles() (console.c's stdin poll rate gate: c4m's counter
//     advances per VM instruction, roughly 100x per guest
//     instruction; a simple +10000 per call keeps the gate opening
//     every ~10 calls, i.e. every ~640 guest instructions -- the
//     same order of polling cadence the hosted builds get).
//
// Only the DEFAULT module set builds natively (no jit.c: a JIT that
// emits c4m bytecode is meaningless without a c4m under it, and the
// hooks are compiled out without -D C4OR1K_JIT anyway).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

static long __time_ms_now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#define __time() __time_ms_now()

static long __c4_cycles_counter;
static long __c4_cycles_now() { __c4_cycles_counter += 10000; return __c4_cycles_counter; }
#define __c4_cycles() __c4_cycles_now()

#define int long
