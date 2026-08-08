//
// smp1 -- exercises every opcode stage 3 added: CAS, XCHG, FADD, the
// CWAI/CWAK park-and-wake pair, and IPI.
//
// Phases 1-3 are correctness. Phase 4 and 5 are the interesting ones,
// because they are the first things in this project where one
// processor has to affect another deliberately rather than by sharing
// a word and hoping.
//
// The deliberate unlocked-counter race lives in the stage 4 guest, not
// here: this file is about proving each instruction does what it says.
//
// Run:  ./c4mp -cpus 4 c4mp-smp1.c4r
//

enum { MAXCPU = 16, NITER = 500 };
enum { CONF_HANDLER = 1 };      // __c4_configure(CONF_CYCLE_INTERRUPT_HANDLER, ..)
enum { HIRQ_IPI = 1 };

int lock;            // 0 = free, 1 = held
int shared;          // guarded by lock, or updated with FADD
int done[MAXCPU];    // one slot per CPU: written by it, read by CPU 0
int gate;            // phase 4 parks on this
int ipis;            // phase 5 counts arrivals here
int failures;

void check(char *what, int got, int want) {
    if (got == want) return;
    printf("smp1: FAIL %s: got %d want %d\n", what, got, want);
    failures = failures + 1;
}

// A spinlock is the smallest thing that needs an atomic, and the
// reason XCHG exists: the swap has to read the old value and store
// the new one without another processor seeing the gap.
void lock_acquire() { while (__c4_xchg((int)&lock, 1)) ; }
void lock_release() { __c4_xchg((int)&lock, 0); }

void worker_locked() {
    int i;
    for (i = 0; i < NITER; i = i + 1) {
        lock_acquire();
        shared = shared + 1;
        lock_release();
    }
    done[__c4_cpu_id()] = 1;
}

void worker_fadd() {
    int i;
    for (i = 0; i < NITER; i = i + 1) __c4_fadd((int)&shared, 1);
    done[__c4_cpu_id()] = 1;
}

// Phase 4: park until CPU 0 opens the gate. __c4_wait re-tests the
// value as it parks, so it does not matter whether the wake arrives
// before or after this call -- there is no window to lose it in.
void worker_wait() {
    while (!gate) __c4_wait((int)&gate, 0);
    done[__c4_cpu_id()] = 1;
}

// Phase 5: the interrupt handler. c4m's 7-parameter trap ABI, which
// c4mp keeps unchanged; returning from it executes LEV, which lands on
// the TLEV the machine planted and restores the interrupted context.
void irq(int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
    if (ins == HIRQ_IPI) ipis = ipis + 1;
}

void worker_ipi() {
    __c4_configure(CONF_HANDLER, (int)&irq);
    done[__c4_cpu_id()] = 1;        // "handler installed, send it"
    while (!ipis) ;                 // the handler runs underneath this loop
    done[__c4_cpu_id()] = 2;
}

// Start every secondary on fn and wait for all of them to report.
// done[] is written by one processor and read by one other, so it
// needs no atomic of its own.
void run_all(int n, int fn, int want) {
    int i, stack;
    for (i = 0; i < n; i = i + 1) done[i] = 0;
    for (i = 1; i < n; i = i + 1) {
        stack = (int)malloc(65536);
        __c4_cpu_start(i, fn, stack + 65536);
    }
    for (i = 1; i < n; i = i + 1) while (done[i] != want) ;
}

int main() {
    int n, i, old;

    if (!(__c4_info() & 0x100)) { printf("smp1: no SMP here\n"); return 1; }
    n = __c4_cpu_count();
    if (n > MAXCPU) n = MAXCPU;
    printf("smp1: %d processor(s)\n", n);

    // ---- 1. the atomics, on one processor, where the answers are exact
    shared = 10;
    check("cas hit returns old",  __c4_cas((int)&shared, 10, 20), 10);
    check("cas hit stores",       shared, 20);
    check("cas miss returns old", __c4_cas((int)&shared, 99, 30), 20);
    check("cas miss leaves it",   shared, 20);
    check("xchg returns old",     __c4_xchg((int)&shared, 7), 20);
    check("xchg stores",          shared, 7);
    check("fadd returns old",     __c4_fadd((int)&shared, 5), 7);
    check("fadd adds",            shared, 12);
    check("fadd negative",        __c4_fadd((int)&shared, -12), 12);
    check("fadd back to zero",    shared, 0);
    printf("smp1: atomics OK\n");

    // ---- 2. a spinlock across every processor
    shared = 0;
    lock = 0;
    run_all(n, (int)&worker_locked, 1);
    worker_locked();
    check("locked counter", shared, n * NITER);
    printf("smp1: spinlock OK, %d increments\n", shared);

    // ---- 3. the same total with no lock at all, via FADD
    shared = 0;
    run_all(n, (int)&worker_fadd, 1);
    worker_fadd();
    check("fadd counter", shared, n * NITER);
    printf("smp1: lock-free counter OK, %d increments\n", shared);

    // ---- 4. park and wake
    gate = 0;
    for (i = 0; i < n; i = i + 1) done[i] = 0;
    for (i = 1; i < n; i = i + 1) {
        old = (int)malloc(65536);
        __c4_cpu_start(i, (int)&worker_wait, old + 65536);
    }
    // Let them reach the park. Setting the gate first and waking
    // second is the safe order, and CWAI's value test makes it safe in
    // either order anyway.
    gate = 1;
    __c4_wake((int)&gate, 0);
    for (i = 1; i < n; i = i + 1) while (!done[i]) ;
    printf("smp1: park and wake OK\n");

    // ---- 5. inter-processor interrupts
    if (n > 1) {
        ipis = 0;
        for (i = 0; i < n; i = i + 1) done[i] = 0;
        for (i = 1; i < n; i = i + 1) {
            old = (int)malloc(65536);
            __c4_cpu_start(i, (int)&worker_ipi, old + 65536);
        }
        for (i = 1; i < n; i = i + 1) while (done[i] != 1) ;
        for (i = 1; i < n; i = i + 1) check("ipi raised", __c4_ipi(i), 1);
        for (i = 1; i < n; i = i + 1) while (done[i] != 2) ;
        check("ipi to a bad id", __c4_ipi(999), 0);
        printf("smp1: interrupts OK, %d delivered\n", ipis);
    } else {
        printf("smp1: interrupts skipped, needs more than one processor\n");
    }

    if (failures) { printf("smp1: %d FAILURES\n", failures); return 1; }
    printf("smp1: all OK\n");
    return 0;
}
