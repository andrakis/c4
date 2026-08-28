// C4IX userland: ps. The task table is kernel memory, so this asks
// for one fixed-shape record at a time rather than walking anything.
//
// Columns are padded explicitly: libc4ix's printf takes no width
// specifiers, which is a deliberate limit rather than an oversight.

#include "c4ix_user.h"

static char *ps_state(int st) {
    if (st == TS_READY) return "ready";
    if (st == TS_RUNNING) return "run";
    if (st == TS_ZOMBIE) return "zombie";
    if (st == TS_WAITING) return "wait";
    if (st == TS_BLOCKED) return "block";
    return "?";
}

int main(int argc, char **argv) {
    int info[TASKINFO_WORDS];
    int i, total, total_hi, stable;

    // -s omits the counters. They are exact and useful, but they
    // shift with every change to the kernel, so a test that pins
    // ps output needs a column set that does not.
    stable = 0;
    if (argc > 1) { if (argv[1][0] == '-') { if (argv[1][1] == 's') stable = 1; } }

    // Headings go through the same widths as the values below, so
    // the two cannot drift apart.
    upadhdr("ID", 4);
    upadhdr("PPID", 5);
    upadstr("STATE", 8);
    upadstr("PRIV", 8);
    if (!stable) {
        upadhdr("SYSCALLS", 8);
        upadhdr("TRAPS", 9);
        upadhdr("CYCLES", 11);
    }
    uprintf(" NAME\n");
    total = 0;
    total_hi = 0;
    i = 0;
    while (utaskinfo(i, info)) {
        upadnum(info[0], 4);
        upadnum(info[1], 5);
        upadstr(ps_state(info[2]), 8);
        upadstr(info[3] ? "user" : "kernel", 8);
        if (!stable) {
            upadnum(info[4], 8);
            upadnum(info[5], 9);
            upadcycles2(info[7], info[6], 11);
        }
        uprintf(" %s\n", (char *)(info + 8));
        total = total + info[6];
        total_hi = total_hi + info[7];
        while (total >= 1000000000) { total = total - 1000000000; ++total_hi; }
        ++i;
    }
    if (stable) uprintf("%d tasks\n", i);
    else {
        uprintf("%d tasks, ", i);
        upadcycles2(total_hi, total, 0);
        uprintf("cycles accounted\n");
    }
    return 0;
}
