// C4IX userland: ps. The task list lives in kernel memory, so this
// asks the kernel for one fixed-shape record at a time rather than
// walking anything itself.
//
// Columns are padded by hand: libc4ix's printf takes %d %x %s %c %%
// and no width specifiers, which is a deliberate limit rather than
// something to work around inside the library.

#include "c4ix_user.h"

static char *ps_state(int st) {
    if (st == TS_READY) return "ready";
    if (st == TS_RUNNING) return "run";
    if (st == TS_ZOMBIE) return "zombie";
    if (st == TS_WAITING) return "wait";
    if (st == TS_BLOCKED) return "block";
    return "?";
}

static void ps_pad(char *s, int width) {
    int n;
    n = ustrlen(s);
    write(STDOUT, s, n);
    while (n < width) { write(STDOUT, " ", 1); ++n; }
}

static void ps_padnum(int v, int width) {
    char buf[16];
    int i, n;

    n = 0;
    if (v == 0) { buf[0] = '0'; n = 1; }
    while (v) { buf[n] = '0' + v - (v / 10) * 10; ++n; v = v / 10; }
    i = n;
    while (i) { --i; write(STDOUT, buf + i, 1); }
    while (n < width) { write(STDOUT, " ", 1); ++n; }
}

int main(int argc, char **argv) {
    int info[TASKINFO_WORDS];
    int i;

    uprintf("ID   STATE   PRIV SYSCALLS NAME\n");
    i = 0;
    while (utaskinfo(i, info)) {
        ps_padnum(info[0], 5);
        ps_pad(ps_state(info[1]), 8);
        ps_padnum(info[2], 5);
        ps_padnum(info[3], 9);
        uprintf("%s\n", (char *)(info + 4));
        ++i;
    }
    return 0;
}
