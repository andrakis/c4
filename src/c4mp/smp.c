//
// c4mp processor management: the CPU table, and the round robin that
// interleaves them.
//
// This is the simulated half of SMP, and it is the half that runs
// everywhere -- natively today, and hosted by c4m, which has no
// multiprocessing of its own to lend. Pass 2 replaces this scheduler
// with one host thread per CPU calling c4_run(c, -1) exactly once;
// everything above it, including every opcode, stays as it is.
//
// Two properties are worth stating because they are the reason to
// build the simulated version at all, and they are properties real
// SMP does not have:
//
//   * It is DETERMINISTIC. Fixed quantum, fixed order, so a run is
//     reproducible and its output can be pinned in a transcript. A
//     native SMP run cannot be, and its tests have to assert
//     invariants instead.
//   * Deadlock is DIAGNOSABLE. When no CPU can run, the machine knows
//     whether that is because they all finished or because they are
//     all parked, and can say which and where. A hung SMP kernel is
//     otherwise the hardest thing in this project to debug.
//
// Every instruction is atomic here, because CPUs only ever change at
// a c4_run boundary. What the simulation does provide is arbitrary
// interleaving BETWEEN instructions, which is where the races that
// matter live.
//

#include "c4mp.h"

struct c4_cpu *c4_cpus;
int c4_ncpu;

// Where a secondary's entry function returns to. Holding CPUH means a
// CPU that runs off the end of its entry halts cleanly instead of
// falling into whatever follows -- the same trick c4m uses to make
// main's return call exit, and the same one c4_tlev_word uses to give
// a trap handler's LEV somewhere to land.
static int c4_halt_word;

int c4_smp_init(int ncpu) {
    int i;

    if (ncpu < 1) return 0;
    c4_halt_word = CPUH;
    if (!(c4_cpus = (struct c4_cpu *)malloc(ncpu * sizeof(struct c4_cpu))))
        return 0;
    memset(c4_cpus, 0, ncpu * sizeof(struct c4_cpu));
    c4_ncpu = ncpu;
    for (i = 0; i < ncpu; ++i) {
        c4_cpus[i].id = i;
        c4_cpus[i].state = CPU_OFF;
        c4_cpus[i].mode = MODE_UNPROTECTED;
    }
    return 1;
}

void c4_smp_free() {
    int i;
    if (!c4_cpus) return;
    // CPU 0's stack belongs to main.c; a secondary's belongs to the
    // guest that passed it to __c4_cpu_start, and the guest may still
    // hold the pointer. Only free what this module allocated, which is
    // currently nothing -- stkbase is recorded so that stops being
    // true silently.
    for (i = 0; i < c4_ncpu; ++i)
        if (c4_cpus[i].stkbase) c4_cpus[i].stkbase = 0;
    free(c4_cpus);
    c4_cpus = 0;
    c4_ncpu = 0;
}

//
// __c4_cpu_start(id, entry, stacktop). The guest owns the stack: it
// allocates the block and passes the address just past its end.
//
// entry points at the function's ENT, so setting pc there and letting
// the ENT build the frame is all that is needed -- except for the
// return address the eventual LEV will pop, which is pushed here.
// Returns 1 on success, 0 if the id is out of range or that CPU is
// already running.
//
int c4_cpu_start(int id, int entry, int stacktop) {
    struct c4_cpu *c;
    int *sp;

    if (id < 0 || id >= c4_ncpu) return 0;
    c = c4_cpus + id;
    if (c->state == CPU_RUN || c->state == CPU_WAIT) return 0;
    if (!entry || !stacktop) return 0;

    sp = (int *)stacktop;
    *--sp = (int)&c4_halt_word;   // where the entry function's LEV lands

    c->pc = (int *)entry;
    c->sp = sp;
    c->bp = sp;
    c->a = 0;
    c->mode = MODE_UNPROTECTED;
    c->cycle = 0;
    c->status = 0;
    c->traph = 0;
    c->ihand = 0;
    c->ival = 0;
    c->tri = 0;
    c->stkbase = 0;               // the guest's, not ours to free
    c->state = CPU_RUN;
    return 1;
}

// Report where every CPU stopped. Only interesting when the machine
// wedged, which is exactly when there is nothing else to go on.
static void c4_smp_report(char *why) {
    struct c4_cpu *c;
    int i;

    printf("c4mp: %s\n", why);
    for (i = 0; i < c4_ncpu; ++i) {
        c = c4_cpus + i;
        printf("  cpu %d: ", i);
        if (c->state == CPU_OFF)       printf("off");
        else if (c->state == CPU_RUN)  printf("running");
        else if (c->state == CPU_WAIT) printf("waiting");
        else                           printf("halted, status %d", c->status);
        printf(", pc 0x%X, %d cycles\n", c->pc, c->cycle);
    }
}

//
// Run until the machine stops. Returns why.
//
// EXIT on CPU 0 stops everything, which is c4m's semantics -- main
// returning ends the program -- and is what keeps single-CPU c4mp a
// drop-in. EXIT or CPUH on a secondary halts only that processor.
//
int c4_smp_run(int quantum) {
    struct c4_cpu *c;
    int i, r, ran, waiting;

    for (;;) {
        ran = 0;
        waiting = 0;
        // A straight walk from 0 each pass. Round robin from wherever
        // the last pass ended would be fairer, but every CPU gets the
        // same quantum, so it would only change WHICH deterministic
        // interleaving happens, not whether one CPU can starve another.
        for (i = 0; i < c4_ncpu; ++i) {
            c = c4_cpus + i;
            if (c->state == CPU_WAIT) { ++waiting; continue; }
            if (c->state != CPU_RUN) continue;

            ran = 1;
            r = c4_run(c, quantum);
            if (r == RUN_FAULT) return RUN_FAULT;
            // CPU 0 is the program. When it exits, the others do not
            // get to finish -- same as a process exiting out from
            // under its threads.
            if (r == RUN_EXIT && i == 0) return RUN_EXIT;
        }

        if (ran) continue;

        // Nothing ran. Two very different situations.
        if (waiting) {
            c4_smp_report("deadlock: every processor is parked and none can be woken");
            return RUN_FAULT;
        }
        // Everything halted without CPU 0 exiting -- it ran off the
        // end of its entry, or halted itself. Its status still stands.
        return RUN_HALT;
    }
}
