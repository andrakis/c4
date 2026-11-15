// accel.c -- the accelerators' loader (libjs/accel.js): INFO says so, a game
// nobody ships is found as 0, and each game the build lists as a number of
// its own. Each game tests its own accelerators against its loops
// (games/duke3d/c4/acceltest.c, games/quake/c4/acceltest.c). test-libjs runs
// this with them and without (cli.js --no-accel).
#include <stdio.h>
#include "accel.h"

int main() {
    int d, q;
    if (!accel_present()) { printf("accel: not fitted\n"); return 0; }
    if (accel_find("no-such-game") != 0 || accel_find("../libjs") != 0) { printf("accel: found what is not there\n"); return 1; }
    d = accel_find("duke3d"); q = accel_find("quake");
    if (d == 0 || q == 0 || d == q || ((d | q) & 0xff) != 0 || d < 0x1100 || q < 0x1100 || d >= 0x2000 || q >= 0x2000) {
        printf("accel: duke3d at %x, quake at %x\n", d, q);
        return 1;
    }
    printf("accel: the loader finds each game's\n");
    return 0;
}
