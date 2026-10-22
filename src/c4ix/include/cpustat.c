// cpustat.c -- cpustat.h's functions. Included by cpustat.h, or built
// into C4IX's shared libc4ix.

#include "c4ix_user.h"
#include "cpustat.h"

int cpu_sample(struct cpusample *s) {
    return ucpustat((int *)&s->h, (int *)s->t, CPU_MAXT);
}

// the record for task id in s, or -1
int cpu_find(struct cpusample *s, int id) {
    int i;
    i = 0;
    while (i < s->h.ntasks && s->t[i].id != id) ++i;
    return i < s->h.ntasks ? i : -1;
}

// What b's record k ran since sample a, in thousands of cycles: its own
// cycles (user), or the kernel's on its behalf (kernel); all of them if
// it is new since a.
int cpu_kdelta(struct cpusample *a, struct cpusample *b, int k, int kernel) {
    long long d;
    int i;
    i = cpu_find(a, b->t[k].id);
    if (kernel) { d = b->t[k].kernel; if (i >= 0) d = d - a->t[i].kernel; }
    else { d = b->t[k].user; if (i >= 0) d = d - a->t[i].user; }
    return d > 0 ? (int)(d / 1000) : 0;
}

// c thousand cycles as a percentage of the interval, rounded: the machine
// was busy for `busy` of its `ms` milliseconds, and in that time ran `all`
// thousand cycles, so c had c/all of the busy part.
int cpu_pct(int c, int all, int busy, int ms) {
    long long den;
    if (all <= 0) return 0;
    den = (long long)all * ms;
    return (int)(((long long)c * busy * 100 + den / 2) / den);
}

// The shares of the machine between samples a and b, in whole percent.
// pct[k], for b's record k, is that task's own cycles -- for the idle
// task, the idle time -- and *sys is System, the kernel's work for all
// of them. Weighed by time: what ran shares the part of the interval the
// machine was awake, in proportion to its cycles; the naps are idle.
//
// A task that ended between the samples has its last cycles in the total
// but no row, as in NT. The idle row is what the others leave, so the
// rows, with those, add up to exactly 100. Returns the CPU usage, which
// is 100 less idle.
int cpu_shares(struct cpusample *a, struct cpusample *b, int *pct, int *sys) {
    long long g;
    int k, ms, busy, all, gone, ks, used, idle;

    k = 0;
    while (k < b->h.ntasks) { pct[k] = 0; ++k; }
    *sys = 0;
    ms = b->h.ms - a->h.ms;
    if (ms <= 0) return 0;
    busy = ms - (b->h.napms - a->h.napms);
    if (busy < 0) busy = 0;

    // everything run in the interval: the tasks in b, and the ones that
    // left -- the reaped total's growth, less what they had run by a
    g = b->h.gone_user + b->h.gone_kernel - a->h.gone_user - a->h.gone_kernel;
    k = 0;
    while (k < a->h.ntasks) {
        if (cpu_find(b, a->t[k].id) < 0) g = g - a->t[k].user - a->t[k].kernel;
        ++k;
    }
    gone = g > 0 ? (int)(g / 1000) : 0;
    all = gone;
    k = 0;
    while (k < b->h.ntasks) {
        all = all + cpu_kdelta(a, b, k, 0) + cpu_kdelta(a, b, k, 1);
        ++k;
    }
    used = cpu_pct(gone, all, busy, ms);
    ks = 0;
    idle = -1;
    k = 0;
    while (k < b->h.ntasks) {
        if (b->t[k].id == b->h.idle) idle = k;
        else {
            pct[k] = cpu_pct(cpu_kdelta(a, b, k, 0), all, busy, ms);
            ks = ks + cpu_kdelta(a, b, k, 1);
            used = used + pct[k];
        }
        ++k;
    }
    *sys = cpu_pct(ks, all, busy, ms);
    used = used + *sys;
    if (used > 100) used = 100;
    if (idle >= 0) pct[idle] = 100 - used;
    return used;
}
