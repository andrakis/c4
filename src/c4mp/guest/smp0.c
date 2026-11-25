//
// smp0 -- the first program to run on more than one C4 processor.
//
// Stage 2 has processors and a scheduler but no atomics, no futex and
// no IPI, so this deliberately shares nothing that would need them.
// Each worker touches only its own slot, and CPU 0 waits by watching
// per-CPU flags -- a plain write on one side and a plain read on the
// other, which needs no ordering guarantee to be correct.
//
// The shared-counter race, and the spinlock that fixes it, are stage
// 4's job. Reaching for them here would only prove the machine can
// produce a wrong answer.
//
// What this DOES prove:
//
//   * secondaries start, run, and halt by returning
//   * __c4_cpu_id / __c4_cpu_count report what the machine thinks
//   * execution genuinely interleaves -- the tick lines alternate,
//     rather than each CPU running to completion in turn
//   * the interleaving is deterministic, so this transcript can be
//     pinned
//
// Run it as:  ./c4mp -cpus 4 c4mp-smp0.c4r
//

enum { MAXCPU = 16, TICKS = 4, SPIN = 3000 };

// One slot per CPU, so no two processors ever write the same word.
int counter[MAXCPU];
int flag[MAXCPU];

// Busy work, sized so a worker spans several quanta and its ticks land
// between the other CPUs' ticks rather than after them.
void work(int id, int tick) {
    int i;
    i = 0;
    while (i < SPIN) {
        counter[id] = counter[id] + 1;
        i = i + 1;
    }
    printf("cpu %d tick %d count %d\n", id, tick, counter[id]);
}

// Every processor including CPU 0 runs this. Returning from it lands
// on the halt sentinel __c4_cpu_start planted, so a secondary needs no
// explicit __c4_cpu_halt().
void worker() {
    int id, t;
    id = __c4_cpu_id();
    t = 0;
    while (t < TICKS) {
        work(id, t);
        t = t + 1;
    }
    flag[id] = 1;
}

int main(int argc, char **argv) {
    int n, i, total, stack;

    // Ask before probing. On c4m every opcode below is an illegal
    // instruction, and the polite way to find that out is the info
    // word rather than a trap.
    if (!(__c4_info() & 0x100)) {
        printf("smp0: this VM has no processors to offer\n");
        return 1;
    }

    n = __c4_cpu_count();
    if (n > MAXCPU) n = MAXCPU;
    printf("smp0: %d processor(s)\n", n);

    for (i = 0; i < n; i = i + 1) { counter[i] = 0; flag[i] = 0; }

    // The guest owns each secondary's stack. c4mp only records the
    // address it was handed, so this allocation must outlive the CPU
    // -- which it does, because nothing here frees it.
    for (i = 1; i < n; i = i + 1) {
        stack = (int)malloc(65536);
        if (!stack) { printf("smp0: out of memory for cpu %d\n", i); return 1; }
        if (!__c4_cpu_start(i, (int)&worker, stack + 65536)) {
            printf("smp0: cpu %d refused to start\n", i);
            return 1;
        }
    }

    // CPU 0 does a share of the work too, then waits. flag[] is
    // written by exactly one processor and read by exactly one other,
    // so this needs no atomics; a real wait belongs on CWAI, which
    // arrives with the rest of the SMP opcodes.
    worker();

    for (i = 1; i < n; i = i + 1)
        while (!flag[i]) ;

    total = 0;
    for (i = 0; i < n; i = i + 1) total = total + counter[i];
    printf("smp0: total %d across %d cpu(s), expected %d\n",
           total, n, n * TICKS * SPIN);
    return total == n * TICKS * SPIN ? 0 : 1;
}
