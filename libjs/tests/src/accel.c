// accel.c -- the accelerators' loader (libjs/accel.js): INFO says so, a game
// nobody ships is found as 0, and each game the build lists as a number of
// its own. Each game tests its own accelerators against its loops
// (games/duke3d/c4/acceltest.c, games/quake/c4/acceltest.c). test-libjs runs
// this with them, without (cli.js --no-accel), and with C4IX's switched off
// (--no-osaccel), which it turns on and off again.
#include <stdio.h>
#include "accel.h"

enum { ACC_OS = 0x1001 };

int main() {
    int d, q, c, s;
    if (!accel_present()) { printf("accel: not fitted\n"); return 0; }
    if (accel_find("no-such-game") != 0 || accel_find("../libjs") != 0) { printf("accel: found what is not there\n"); return 1; }
    d = accel_find("duke3d"); q = accel_find("quake");
    if (d == 0 || q == 0 || d == q || ((d | q) & 0xff) != 0 || d < 0x1100 || q < 0x1100 || d >= 0x2000 || q >= 0x2000) {
        printf("accel: duke3d at %x, quake at %x\n", d, q);
        return 1;
    }
    printf("accel: the loader finds each game's\n");
    // an operating system's (C4IX's src/c4ix/accel.mjs, os/c4ix): its switch,
    // ACC_OS, 3 on and 2 off where the machine has one; while it is off
    // ACC_FIND gives 0 for it, and the games' as before
    s = __c4_opcode(0 - 1, ACC_OS);
    if (!(s & 2)) { printf("accel: no OS accelerators (%d)\n", s); return 1; }
    c = accel_find("c4ix");
    if ((s & 1) != (c != 0) || c == d || c == q) { printf("accel: the switch %d, c4ix at %x\n", s, c); return 1; }
    if (__c4_opcode(0, ACC_OS) != s || __c4_opcode(0 - 1, ACC_OS) != 2 || accel_find("c4ix") != 0 || accel_find("quake") != q) {
        printf("accel: turned off, c4ix still at %x\n", accel_find("c4ix"));
        return 1;
    }
    if (__c4_opcode(1, ACC_OS) != 2 || __c4_opcode(0 - 1, ACC_OS) != 3 || (c = accel_find("c4ix")) == 0 || (c & 0xff) != 0) {
        printf("accel: turned on, c4ix at %x\n", c);
        return 1;
    }
    printf("accel: the OS switch was %s and turns\n", (s & 1) ? "on" : "off");
    return 0;
}
