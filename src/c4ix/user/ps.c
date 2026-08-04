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
    int i, total, stable;

    // -s omits the counters. They are exact and useful, but they
    // shift with every change to the kernel, so a test that pins
    // ps output needs a column set that does not.
    stable = 0;
    if (argc > 1) { if (argv[1][0] == '-') { if (argv[1][1] == 's') stable = 1; } }

    if (stable) uprintf("  ID  PPID STATE   PRIV     NAME\n");
    else uprintf("  ID  PPID STATE   PRIV     SYSCALLS     TRAPS      CYCLES  NAME\n");
    total = 0;
    i = 0;
    while (utaskinfo(i, info)) {
        upadnum(info[0], 4);
        upadnum(info[1], 5);
        upadstr(ps_state(info[2]), 8);
        upadstr(info[3] ? "user" : "kernel", 8);
        if (!stable) {
            upadnum(info[4], 8);
            upadnum(info[5], 9);
            upadcycles(info[6], 11);
        }
        uprintf(" %s\n", (char *)(info + 7));
        total = total + info[6];
        ++i;
    }
    if (stable) uprintf("%d tasks\n", i);
    else {
        uprintf("%d tasks, ", i);
        upadcycles(total, 0);
        uprintf("cycles accounted\n");
    }
    return 0;
}
