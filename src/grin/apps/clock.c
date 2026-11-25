// clock.c -- CLOCK.C4R, grin's Clock (docs/grin-plan.md 4): the window
// kit's Clock class (src/winkit/clock.c), linked with import.c. Every
// launch opens another Clock on the same image.

#include <stdio.h>
#include "winkit.h"
#include "grin.h"

int main(int argc, char **argv) {
    if (!grin_present()) { printf("Clock is a grin application: GRIN starts grin, and Clock is in its Accessories.\n"); return 1; }
    if (clock_open() < 0) return 1;
    return GRIN_RESIDENT;
}
