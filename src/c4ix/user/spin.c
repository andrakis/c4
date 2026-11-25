// C4IX userland: spin. Burns cycles for a while, printing progress,
// so there is something to interrupt with Ctrl-C. Takes a count of
// ticks (default 20); each tick is a few million cycles.

#include "c4ix_user.h"

enum { TICK_CYCLES = 3000000 };

int main(int argc, char **argv) {
    int ticks, i, t0;

    ticks = 20;
    if (argc > 1) {
        ticks = 0;
        i = 0;
        while (argv[1][i] >= '0' && argv[1][i] <= '9') {
            ticks = ticks * 10 + (argv[1][i] - '0');
            ++i;
        }
        if (ticks < 1) ticks = 20;
    }

    uprintf("spin: %d ticks, Ctrl-C to cancel\n", ticks);
    i = 0;
    while (i < ticks) {
        t0 = ucycles();
        while (ucycles() - t0 < TICK_CYCLES) ;
        uprintf("spin: tick %d/%d\n", i + 1, ticks);
        ++i;
    }
    uprintf("spin: finished\n");
    return 0;
}
