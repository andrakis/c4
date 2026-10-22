// cpustat.h -- C4IX's CPU accounting, as top and System Monitor read it
// (docs/c4ix-cpu.md). Include after c4ix_user.h.
//
// ucpustat copies out one snapshot of every task, taken in one trap,
// with the bytes each holds and the machine heap's size and use. A
// task's cycles are two counts: `user`, what it ran itself, and `kernel`,
// what the kernel spent on its syscalls and switches -- which is shown as
// System, not against the program. The idle task's cycles are idle time,
// and so are the milliseconds the machine napped, when nothing ran at all.
//
// The counts are long long, which is why this is not in c4ix_user.h:
// every program includes that, and only these two need the arithmetic.
// The functions are in cpustat.c, beside this file, which this includes
// unless C4IX_SHARED is defined: then they are C4IX's shared libc4ix's.

#ifndef __C4IX_CPUSTAT_H
#define __C4IX_CPUSTAT_H 1

enum { CPUSTAT_NAME = 16, CPU_MAXT = 32 };

// the kernel's struct cpustat and struct cpustat_task (c4ix.h)
struct cpustat {
    int ntasks;                // records that follow
    int ms;                    // machine ms when it was taken
    int napms;                 // ms the idle loop has slept, since boot
    int idle;                  // the idle task's id
    long long gone_user;       // cycles of tasks already reaped
    long long gone_kernel;
    int heap;                  // the machine's heap in bytes, and how much of it is
    int heapused;              // in use; both 0 where the host does not say
};
struct cpustat_task {
    int id, parent, state, privs, nsyscalls;
    long long user;
    long long kernel;
    int mem;                   // bytes it holds: image, stack, library data, allocations
    char name[CPUSTAT_NAME];
};

struct cpusample {
    struct cpustat h;
    struct cpustat_task t[CPU_MAXT];
};

int cpu_sample(struct cpusample *s);
int cpu_find(struct cpusample *s, int id);
int cpu_kdelta(struct cpusample *a, struct cpusample *b, int k, int kernel);
int cpu_pct(int c, int all, int busy, int ms);
int cpu_shares(struct cpusample *a, struct cpusample *b, int *pct, int *sys);

#ifndef C4IX_SHARED
#include "cpustat.c"
#endif

#endif
