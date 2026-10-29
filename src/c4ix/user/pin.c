// C4IX userland: pin. Which CPUs a program may run on (SYS_AFFINITY,
// sched.c), for a machine with several (?cpus=N on the page,
// docs/c4ix-smp.md):
//
//   pin CPUS COMMAND [ARGS...]   run COMMAND on those CPUs only
//   pin -p PID [CPUS]            show, or change, a running task's
//
// CPUS is a list of numbers and ranges, 0,2 or 1-3, counting from 0 as
// top does, or "any". COMMAND is found as the shell finds it, and is
// waited for: pin 2 wolf3d --window & runs one copy of Wolf3D on CPU 2
// in the background, and the programs it starts stay there too, since a
// child starts with its parent's CPUs.

#include "c4ix_user.h"

static char path[96];

static char *cat(char *d, char *s) {
    while (*s) { *d = *s; ++d; ++s; }
    *d = 0;
    return d;
}

// CPUS as a mask; -1 when it is not a list of CPUs
static int cpus(char *s) {
    int mask, a, b, k;
    if (!ustrcmp(s, "any")) return 0;
    mask = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0 - 1;
        a = 0;
        while (*s >= '0' && *s <= '9') { a = a * 10 + (*s - '0'); ++s; }
        b = a;
        if (*s == '-') {
            ++s;
            if (*s < '0' || *s > '9') return 0 - 1;
            b = 0;
            while (*s >= '0' && *s <= '9') { b = b * 10 + (*s - '0'); ++s; }
        }
        if (a > 30 || b > 30 || b < a) return 0 - 1;
        k = a;
        while (k <= b) { mask = mask | (1 << k); ++k; }
        if (*s == ',') ++s;
        else if (*s) return 0 - 1;
    }
    return mask ? mask : 0 - 1;
}

static void show(int pid, int mask) {
    int k, first;
    uprintf("pin: %d runs on ", pid);
    if (!mask) { uprintf("any CPU\n"); return; }
    uprintf("CPU");
    first = 1;
    k = 0;
    while (k < 31) {
        if (mask & (1 << k)) { uprintf(first ? " %d" : ",%d", k); first = 0; }
        ++k;
    }
    uprintf("\n");
}

static int usage() {
    uprintf("usage: pin CPUS COMMAND [ARGS...]   run COMMAND on those CPUs\n");
    uprintf("       pin -p PID [CPUS]            show or change a task's CPUs\n");
    uprintf("CPUS: 0,2 or 1-3, as top numbers them, or any\n");
    return 2;
}

// NAME as the shell finds it, with its PATH as it starts (sh.c):
// /bin/NAME, /usr/bin/NAME, /usr/games/bin/NAME, c4ix-NAME.c4r, NAME,
// NAME.c4r
static int run(char *name, int argc, char **argv) {
    int i, form, pid, st[4];
    char *p;
    i = 0;
    while (name[i] && name[i] != '/' && name[i] != '.') ++i;
    if (name[i]) return spawn(name, argc, argv);
    if (i > 64) return 0 - 1;
    form = 0 - 2;
    pid = 0 - 1;
    while (form < 4 && pid < 0) {
        if (form <= 0) {
            if (form == 0 - 2) p = cat(path, "/bin/");
            else if (form == 0 - 1) p = cat(path, "/usr/bin/");
            else p = cat(path, "/usr/games/bin/");
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
    int mask, pid, old, code;
    if (argc < 2) return usage();
    if (!ustrcmp(argv[1], "-p")) {
        if (argc < 3 || argc > 4) return usage();
        pid = 0;
        old = 0;
        while (argv[2][old] >= '0' && argv[2][old] <= '9') { pid = pid * 10 + (argv[2][old] - '0'); ++old; }
        if (argv[2][old] || pid <= 0) return usage();
        mask = argc == 4 ? cpus(argv[3]) : 0 - 1;
        if (argc == 4 && mask < 0) return usage();
        if ((old = uaffinity(pid, mask)) < 0) { uprintf("pin: no task %d of a program's\n", pid); return 1; }
        show(pid, argc == 4 ? mask : old);
        return 0;
    }
    if (argc < 3 || (mask = cpus(argv[1])) < 0) return usage();
    uaffinity(0, mask);
    if ((pid = run(argv[2], argc - 2, argv + 2)) < 0) {
        uprintf("pin: %s: not found\n", argv[2]);
        return 127;
    }
    code = uwait(pid);
    return code;
}
