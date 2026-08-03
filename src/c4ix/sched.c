//
// C4IX scheduler: one switch mechanism, two backends.
//
// c4m backend: every switch runs inside a trap handler -- the soft
// trap that sched_yield raises, or the cycle interrupt that gives
// preemption. c4m's trap() builds the handler an ordinary frame
// whose ARGUMENTS are the interrupted registers; assigning to those
// parameters and returning (through TLEV) IS the context switch.
//
// plain c4 backend: no traps exist. A frame's bp is the address of
// a (saved_bp, return_pc) pair and LEV rebuilds sp from bp, so
// coop_switch saves its own pair, plants the target's pair, and
// returns through a trampoline whose second LEV makes sp consistent.
// Proof of concept: src/tests/test_coop_switch.c (verified under
// c4lc, plain and -O, on both hosts).
//
// Fresh tasks are forged SV_FRAME states that both backends can
// resume: the fake pair sends control into task_shim, which calls
// the real entry and turns its return value into task_exit.
//

#include "c4ix.h"

enum { C4IX_STACK_WORDS = 8192 };   // 64KB per task

struct task *sched_cur;
static struct task *boot_task;
static int sched_on_c4m;
static int sched_interval;   // preemption interval, 0 = cooperative only
static int sched_lockdepth;
static int sched_tramp;      // address of the trampoline's LEV

int sched_switches;          // context switches since boot

// Compiles to ENT 0 / LEV; entered at its LEV (sched_tramp) the
// second LEV re-derives sp from the just-installed bp.
static int trampoline() { }

struct task *sched_current() {
    return sched_cur;
}

// ---- preemption masking (c4m only; nestable) ----

void sched_lock() {
    if (sched_on_c4m) {
        if (sched_lockdepth == 0 && sched_interval)
            __c4_configure(C4IX_CONF_INTERVAL, 0);
    }
    ++sched_lockdepth;
}

void sched_unlock() {
    --sched_lockdepth;
    if (sched_on_c4m) {
        if (sched_lockdepth == 0 && sched_interval)
            __c4_configure(C4IX_CONF_INTERVAL, sched_interval);
    }
}

// ---- picking the next task ----

// Round-robin from t: the next TS_READY task, or t itself.
static struct task *sched_pick(struct task *t) {
    struct task *n;
    n = task_next(t);
    while (n != t) {
        if (n->state == TS_READY) return n;
        n = task_next(n);
    }
    return t;
}

// ---- the c4m backend: switching inside a trap handler ----

// Serves both the soft yield trap and the hard cycle interrupt.
// Assigning to a/bp/sp/returnpc changes where TLEV resumes.
static void sched_trap(int trap, int param, int mode, int a, int bp, int sp, int returnpc) {
    struct task *t, *n;

    // no nested switches while kernel structures move
    __c4_configure(C4IX_CONF_INTERVAL, 0);

    t = sched_cur;
    n = sched_pick(t);
    if (n != t) {
        t->sv = SV_REGS;
        t->sv_a = a; t->sv_bp = bp; t->sv_sp = sp; t->sv_pc = returnpc;
        if (t->state == TS_RUNNING) t->state = TS_READY;
        if (n->sv == SV_REGS) {
            a = n->sv_a; bp = n->sv_bp; sp = n->sv_sp; returnpc = n->sv_pc;
        } else {
            // SV_FRAME: registers derive from the pair. sp lands just
            // past it, exactly where the frame's LEV would have left it.
            a = 0;
            bp = n->sv_bp;
            sp = n->sv_sp + 16;
            returnpc = n->sv_pc;
        }
        n->state = TS_RUNNING;
        sched_cur = n;
        ++sched_switches;
    }

    if (sched_interval && sched_lockdepth == 0)
        __c4_configure(C4IX_CONF_INTERVAL, sched_interval);
}

// ---- the plain-c4 backend: the LEV trick ----

static void coop_switch(struct task *next) {
    int *bp;     // FIRST local: &bp + 1 == this frame's bp
    int *f;
    struct task *old;

    bp = (int *)(&bp + 1);
    old = sched_cur;

    // save the outgoing task: its frame address and the pair there
    old->sv = SV_FRAME;
    old->sv_sp = (int)bp;
    old->sv_bp = *bp;
    old->sv_pc = *(bp + 1);
    if (old->state == TS_RUNNING) old->state = TS_READY;

    // plant the incoming task's own pair back into its frame
    f = (int *)next->sv_sp;
    *f = next->sv_bp;
    *(f + 1) = next->sv_pc;

    // rewrite our own frame: LEV installs next's bp, the trampoline's
    // LEV then rebuilds sp from it and pops the planted pair
    *bp = (int)f;
    *(bp + 1) = sched_tramp;

    next->state = TS_RUNNING;
    sched_cur = next;
    ++sched_switches;
}

// ---- the common face ----

void sched_yield() {
    struct task *n;
    if (!sched_cur) return;
    if (sched_on_c4m) {
        __c4_trap(C4IX_TRAP_SOFT_IRQ, 0);
        return;
    }
    n = sched_pick(sched_cur);
    if (n != sched_cur) coop_switch(n);
}

// ---- task start and end ----

static int task_lost() {
    kputs("c4ix: panic: task escaped its shim\n");
    exit(-1);
    return 0;
}

// Every task starts here (via the forged frame): call the real entry,
// turn its return value into an exit.
static int task_shim(int entry, int argc, int argv) {
    int e, r;
    e = entry;
    r = e(argc, argv);
    task_exit(r);
    return 0;   // unreachable
}

void task_exit(int code) {
    sched_cur->exitcode = code;
    sched_cur->state = TS_ZOMBIE;
    while (1) sched_yield();   // a zombie is never picked again
}

int task_wait(struct task *t) {
    int code;
    while (t->state != TS_ZOMBIE) sched_yield();
    code = t->exitcode;
    task_release(t);
    return code;
}

// Build the stack + fake frame that makes a never-run task resumable
// by either backend. Layout (see test_coop_switch.c):
//   X[0] saved bp (junk; the entry's ENT overwrites X[1], not this)
//   X[1] return pc -> task_shim
//   X[2] where task_shim's own LEV would land -> task_lost
//   X[3..5] task_shim's args, last first: argv, argc, entry
int sched_forge(struct task *t, int entry, int argc, int argv) {
    int *stk, *x;

    if (!(stk = (int *)malloc(C4IX_STACK_WORDS * 8))) return 0;
    memset(stk, 0, C4IX_STACK_WORDS * 8);
    x = stk + C4IX_STACK_WORDS - 8;   // slack above, as C4KE leaves

    x[0] = 0;
    x[1] = (int)&task_shim;
    x[2] = (int)&task_lost;
    x[3] = argv;
    x[4] = argc;
    x[5] = entry;

    t->stack = (int)stk;
    t->sv = SV_FRAME;
    t->sv_sp = (int)x;
    t->sv_bp = 0;
    t->sv_pc = (int)&task_shim;
    return 1;
}

// ---- lifecycle ----

void sched_init(int interval) {
    sched_on_c4m = (host_type() == HOST_C4M);
    sched_tramp = (int)&trampoline + 16;   // past ENT 0: the LEV word

    boot_task = task_adopt("boot");
    boot_task->state = TS_RUNNING;
    sched_cur = boot_task;

    if (sched_on_c4m) {
        install_trap_handler((int)&sched_trap);
        if (interval) {
            sched_interval = interval;
            __c4_configure(C4IX_CONF_HANDLER, (int)&sched_trap);
            __c4_configure(C4IX_CONF_INTERVAL, interval);
        }
    }
}

// Disarm the trap machinery: after this the scheduler is inert and
// the task list can be torn down. Without it the cycle interrupt
// would fire after main returns, into a freed task list.
void sched_stop() {
    if (sched_on_c4m) {
        __c4_configure(C4IX_CONF_INTERVAL, 0);
        install_trap_handler(0);
    }
    sched_interval = 0;
    sched_cur = 0;
}

// Run until only the boot task is left alive, then reap anything
// nobody waited for.
void sched_run() {
    struct task *t;
    int live;

    live = 1;
    while (live) {
        live = 0;
        t = task_head;
        while (t) {
            if (t != sched_cur && t->state != TS_ZOMBIE) live = 1;
            t = t->next;
        }
        if (live) sched_yield();
    }
    live = 1;
    while (live) {
        live = 0;
        t = task_head;
        while (t) {
            if (t->state == TS_ZOMBIE) { task_wait(t); live = 1; t = 0; }
            else t = t->next;
        }
    }
}
