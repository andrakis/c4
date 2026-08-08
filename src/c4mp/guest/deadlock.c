//
// deadlock -- every processor parks on a word nobody will ever change.
//
// This exists to be run and to FAIL. It is the one case a simulated
// multiprocessor can diagnose and a real one cannot: when no CPU can
// run, the machine knows whether that is because they all finished or
// because they are all parked, and it can say which address each one
// is waiting on. On real hardware the same program is an unkillable
// hang with nothing to look at.
//
// Expected: c4mp prints "deadlock: every processor is parked and none
// can be woken", lists each CPU, and exits non-zero. The line below
// marked unreachable must never appear.
//
// Run:  ./c4mp -cpus 4 c4mp-deadlock.c4r
//

int never;

void parker() {
    while (!never) __c4_wait((int)&never, 0);
}

int main() {
    int n, i, stack;

    if (!(__c4_info() & 0x100)) { printf("deadlock: no SMP here\n"); return 1; }
    n = __c4_cpu_count();
    printf("deadlock: parking %d processor(s) on a word nobody sets\n", n);

    for (i = 1; i < n; i = i + 1) {
        stack = (int)malloc(65536);
        __c4_cpu_start(i, (int)&parker, stack + 65536);
    }
    parker();

    printf("deadlock: UNREACHABLE -- a parked processor was resumed\n");
    return 0;
}
