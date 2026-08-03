//
// C4IX init: the first task, and X1's demonstration workload.
//
// Three acts:
//   1. Cooperative ping/pong -- two tasks yielding explicitly. Runs
//      under sched_lock so the interleaving is pure round-robin on
//      both hosts and the output pins exactly.
//   2. Preemption proof (c4m only) -- two busy tasks that never
//      yield, each sampling the other's progress counter halfway
//      through its own loop. Only preemption can interleave them, so
//      "saw the other side mid-run" is the boolean that gets printed
//      -- no interleaving-sensitive output, the pin survives codegen
//      changes shifting cycle counts.
//   3. Spawn -- load a .c4r from the host filesystem (kernel argv[1]
//      if present), run it as a task, wait for its exit code.
//

#include "c4ix.h"

static int pp_rounds;

static int ping_task(int argc, int argv) {
    int i;
    i = 0;
    while (i < argc) {
        kprintf("ping %d\n", i);
        sched_yield();
        ++i;
    }
    return 100 + argc;
}

static int pong_task(int argc, int argv) {
    int i;
    i = 0;
    while (i < argc) {
        kprintf("pong %d\n", i);
        sched_yield();
        ++i;
    }
    return 200 + argc;
}

// busy tasks: no yields, just work and one mid-run sample of the
// other side's progress
static int busy_a_n;
static int busy_b_n;
static int busy_a_saw;
static int busy_b_saw;

enum { BUSY_SPIN = 60000 };

static int busy_a(int argc, int argv) {
    int i;
    i = 0;
    while (i < BUSY_SPIN) {
        busy_a_n = busy_a_n + 1;
        if (i == BUSY_SPIN / 2) busy_a_saw = busy_b_n;
        ++i;
    }
    return 0;
}

static int busy_b(int argc, int argv) {
    int i;
    i = 0;
    while (i < BUSY_SPIN) {
        busy_b_n = busy_b_n + 1;
        if (i == BUSY_SPIN / 2) busy_b_saw = busy_a_n;
        ++i;
    }
    return 0;
}

int init_main(int argc, int argv) {
    struct task *a, *b;
    char **av;
    int ra, rb, n;

    kprintf("init: c4ix init v1 on %s\n", host_name());

    // act 1: cooperative round-robin, preemption masked for an exact
    // interleaving pin on both hosts
    sched_lock();
    pp_rounds = 3;
    a = task_create("ping", (int)&ping_task, pp_rounds, 0);
    b = task_create("pong", (int)&pong_task, pp_rounds, 0);
    kprintf("init: ping id %d, pong id %d, tasks %d\n", a->id, b->id, task_count());
    ra = task_wait(a);
    rb = task_wait(b);
    kprintf("init: ping exited %d, pong exited %d\n", ra, rb);
    sched_unlock();

    // act 2: preemption, where the host has a cycle interrupt
    if (host_type() == HOST_C4M) {
        a = task_create("busy_a", (int)&busy_a, 0, 0);
        b = task_create("busy_b", (int)&busy_b, 0, 0);
        ra = task_wait(a);
        rb = task_wait(b);
        kprintf("init: busy done %d %d, preempted %d %d\n",
            busy_a_n == BUSY_SPIN, busy_b_n == BUSY_SPIN,
            busy_a_saw > 0, busy_b_saw > 0);
    } else {
        kprintf("init: no preemption on this host, skipping busy demo\n");
    }

    // act 3: spawn programs from the host filesystem. Each runs as
    // userland -- behind protected mode where the host provides it --
    // so every one of their syscalls passes through sys.c, whether
    // the program asks through libc4ix or just calls printf.
    if (argc > 1) {
        av = (char **)argv;
        n = 1;
        while (n < argc) {
            if ((a = task_spawn(av[n], argc - n, (int)(av + n)))) {
                kprintf("init: spawned '%s' as task %d (%s)\n", a->name, a->id,
                    a->privs == PRIV_USER ? "protected" : "unprotected");
                ra = task_wait(a);
                kprintf("init: '%s' exited %d after %d syscalls\n",
                    av[n], ra, task_last_syscalls);
            } else {
                kprintf("init: spawn of '%s' failed\n", av[n]);
            }
            ++n;
        }
    }

    return 0;
}
