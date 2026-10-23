// C4IX userland: nice. A program's priority (SYS_NICE, sched.c): -10,
// the most favoured, to 10; 0 for most.
//
//   nice [-n N] COMMAND [ARGS...]   run COMMAND at N (10 if not given)
//   nice -N COMMAND, nice --N ...   the same, as Unix's nice spells it
//   nice -p PID [N]                 show, or change, a running task's
//
// A favoured task preempts a less favoured one when it wakes, and runs
// 1 - N slices a turn; the turns still go round, so nothing starves.
// Wolf3D, Doom and Tyrian ask for -5 themselves. COMMAND is found as the
// shell finds it, and waited for; its children start at its priority.

#include "c4ix_user.h"

static char path[96];

static char *cat(char *d, char *s) {
    while (*s) { *d = *s; ++d; ++s; }
    *d = 0;
    return d;
}

// a number, -10..10; 99 when it is not one
static int num(char *s) {
    int n, neg;
    neg = 0;
    if (*s == '-') { neg = 1; ++s; }
    else if (*s == '+') ++s;
    if (*s < '0' || *s > '9') return 99;
    n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); ++s; }
    if (*s || n > 10) return 99;
    return neg ? -n : n;
}

static int usage() {
    uprintf("usage: nice [-n N] COMMAND [ARGS...]   run COMMAND at priority N (10 if not given)\n");
    uprintf("       nice -p PID [N]                 show or change a task's\n");
    uprintf("N: -10, the most favoured, to 10; 0 for most\n");
    return 2;
}

// NAME as the shell finds it: /bin/NAME, c4ix-NAME.c4r, NAME, NAME.c4r
static int run(char *name, int argc, char **argv) {
    int i, form, pid, st[4];
    char *p;
    i = 0;
    while (name[i] && name[i] != '/' && name[i] != '.') ++i;
    if (name[i]) return spawn(name, argc, argv);
    form = 0;
    pid = 0 - 1;
    while (form < 4 && pid < 0) {
        if (form == 0) {
            p = cat(path, "/bin/");
            cat(p, name);
            if (ustat(path, st) < 0 || st[0] != 2) { ++form; continue; }
        } else if (form == 1) {
            p = cat(path, "c4ix-");
            p = cat(p, name);
            cat(p, ".c4r");
        } else if (form == 2) {
            cat(path, name);
        } else {
            p = cat(path, name);
            cat(p, ".c4r");
        }
        pid = spawn(path, argc, argv);
        ++form;
    }
    return pid;
}

int main(int argc, char **argv) {
    int n, pid, old, i, k;
    if (argc < 2) return usage();
    if (!ustrcmp(argv[1], "-p")) {
        if (argc < 3 || argc > 4) return usage();
        pid = 0;
        k = 0;
        while (argv[2][k] >= '0' && argv[2][k] <= '9') { pid = pid * 10 + (argv[2][k] - '0'); ++k; }
        if (argv[2][k] || pid <= 0) return usage();
        n = argc == 4 ? num(argv[3]) : UNICE_ASK;
        if (argc == 4 && n == 99) return usage();
        if ((old = unice(pid, n)) == UNICE_NONE) { uprintf("nice: no task %d of a program's\n", pid); return 1; }
        uprintf("nice: %d is at %d\n", pid, argc == 4 ? n : old);
        return 0;
    }
    n = 10;
    i = 1;
    if (!ustrcmp(argv[1], "-n")) {
        if (argc < 4 || (n = num(argv[2])) == 99) return usage();
        i = 3;
    } else if (argv[1][0] == '-') {
        // -N is N, --N is -N, as Unix's nice has them
        if ((n = num(argv[1] + 1)) == 99) return usage();
        i = 2;
    }
    if (i >= argc) return usage();
    unice(0, n);
    if ((pid = run(argv[i], argc - i, argv + i)) < 0) {
        uprintf("nice: %s: not found\n", argv[i]);
        return 127;
    }
    return uwait(pid);
}
