//
// C4IX benchmark. Two halves:
//
//   compute -- the same pi and factorial workload C4KE's src/bench/
//              bench.c runs, copied so the numbers mean the same
//              thing. This measures the COMPILER and the VM, not the
//              OS, and should land close to C4KE's.
//
//   os      -- what C4IX actually costs: a syscall, a yield, and a
//              spawn+wait. These are the numbers that say whether
//              the rewrite was worth it.
//
// Everything is reported in VM CYCLES, not milliseconds. Cycles are
// deterministic -- the same run gives the same number -- while wall
// time on this VM is dominated by measurement windows and drifts
// 5-10% run to run (the lesson from innerbench).
//

#include "c4ix_user.h"

// ---- the C4KE compute workload, verbatim ----

static int pow_mod(int a, int b, int m) {
    int r, aa;
    r = 1;
    aa = a;
    while (b > 0) {
        if (b - (b / 2) * 2 == 1) r = (r * aa) % m;
        aa = (aa * aa) % m;
        b = b / 2;
    }
    return r;
}

static int nth_digit_of_pi(int n) {
    int sum, k, term1, term2, term3, term4;
    sum = 0;
    k = 0;
    while (k <= n) {
        term1 = (4 * pow_mod(16, n - k, 8 * k + 1)) / (8 * k + 1);
        term2 = (2 * pow_mod(16, n - k, 8 * k + 4)) / (8 * k + 4);
        term3 = (1 * pow_mod(16, n - k, 8 * k + 5)) / (8 * k + 5);
        term4 = (1 * pow_mod(16, n - k, 8 * k + 6)) / (8 * k + 6);
        sum = sum + (term1 - term2 - term3 - term4);
        sum = sum % 16;
        ++k;
    }
    return sum;
}

static int factorial_recursive(int n) {
    if (n <= 1) return 1;
    return n * factorial_recursive(n - 1);
}

// ---- the OS microbenchmarks ----

enum { SYS_REPS = 200, YIELD_REPS = 200, SPAWN_REPS = 5 };

static int bench_syscall() {
    int i, t0;
    t0 = ucycles();
    i = 0;
    while (i < SYS_REPS) { getpid(); ++i; }
    return (ucycles() - t0) / SYS_REPS;
}

static int bench_yield() {
    int i, t0;
    t0 = ucycles();
    i = 0;
    while (i < YIELD_REPS) { uyield(); ++i; }
    return (ucycles() - t0) / YIELD_REPS;
}

// Spawn a trivial program and wait for it: the whole round trip
// through the loader, the scheduler and the reaper.
static int bench_spawn(char *prog) {
    char *av[2];
    int i, t0, pid;

    av[0] = prog;
    av[1] = 0;
    t0 = ucycles();
    i = 0;
    while (i < SPAWN_REPS) {
        if ((pid = spawn(prog, 1, av)) < 0) return -1;
        uwait(pid);
        ++i;
    }
    return (ucycles() - t0) / SPAWN_REPS;
}

int main(int argc, char **argv) {
    int t0, d;

    uprintf("bench: cycles at start %d\n", ucycles());

    t0 = ucycles();
    d = nth_digit_of_pi(100);
    uprintf("bench: pi100 digit %d in %d cycles\n", d, ucycles() - t0);

    t0 = ucycles();
    d = nth_digit_of_pi(300);
    uprintf("bench: pi300 digit %d in %d cycles\n", d, ucycles() - t0);

    t0 = ucycles();
    d = factorial_recursive(10);
    uprintf("bench: fac10 %d in %d cycles\n", d, ucycles() - t0);

    uprintf("bench: syscall %d cycles each (%d reps)\n", bench_syscall(), SYS_REPS);
    uprintf("bench: yield %d cycles each (%d reps)\n", bench_yield(), YIELD_REPS);
    if (argc > 1)
        uprintf("bench: spawn+wait %d cycles each (%d reps)\n",
            bench_spawn(argv[1]), SPAWN_REPS);

    return 0;
}
