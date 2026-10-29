//
// C4IX scheduler.
//
// Every switch runs inside a trap handler -- the soft trap that
// sched_yield raises, or the cycle interrupt that gives preemption.
// c4m's trap() builds the handler an ordinary frame whose ARGUMENTS
// are the interrupted registers; assigning to those parameters and
// returning (through TLEV) IS the context switch.
//
// (There was a second backend, for plain c4, which has no traps: a
// cooperative switch through a trampoline LEV. No host ever reached it
// -- plain c4 runs C4IX only through c4m.c, so C4IX always sees c4m --
// and it went, with the systable it needed, in docs/c4ix-cpu.md step 10.
// src/tests/test_coop_switch.c still shows the trick.)
//
// Fresh tasks are forged SV_FRAME states: the fake pair sends control
// into task_shim, which calls the real entry and turns its return
// value into task_exit.
//

#include "c4ix.h"


// Idle backoff, in microseconds: how long the idle task sleeps when
// every other task is parked waiting on the outside world.
// With nobody reading the console, nothing can come from outside that a
// task is waiting for, and the idle task naps straight to the next
// deadline -- but no longer than IDLE_NAP_LONG, so a Ctrl-C, or a host
// whose sleep does not end early for one, is still seen promptly.
enum { IDLE_NAP_MIN = 100, IDLE_NAP_MAX = 20000, IDLE_NAP_LONG = 100000 };

// The context the machine booted in, which waits for the end once the
// scheduler runs (sched_run), and the task that runs when nothing else
// can (sched_idle). The idle task is never in the round-robin: sched_pick
// gives it the machine only when no other task can have it, so a busy
// task's syscalls do not each pass through it on the way back.
// (CPU 0's. Every other CPU has an idle task of its own: cpu_self->idle.)
static struct task *kernel_task;
static struct task *idle_task;
static int sched_idle(int argc, int argv);
static int sched_unblocked(struct task *n);
static void sched_banner();
static int sched_interval;   // preemption interval, 0 = none

// ---- processors (docs/c4ix-smp.md S3) ----
//
// What was the scheduler's own state -- the running task, the trap
// flag, the mask depth, the accounting stamps -- is each CPU's: struct
// cpu (c4ix.h), at cpu_self. On one CPU that is cpu_boot, and nothing
// else here changes: every test below of smp_ncpu fails, and the code
// is the code it was. Under SMP cpu_self is the CPU-local window, and:
//
// - The KERNEL LOCK (bkl_*) serializes the kernel. The trap handler
//   holds it from entry to exit; sched_lock takes it outside the
//   handler; a kernel task holds it the whole time it runs, since
//   kernel tasks call the kernel directly. User tasks run without it,
//   at once, one per CPU. Idle tasks run without it too, and take
//   sched_lock where they look at the kernel.
// - Kernel tasks run on CPU 0 only; each idle task on its own CPU.
// - A task that some CPU may still be standing on (oncpu) is neither
//   resumed elsewhere nor freed. A CPU is on the stack of the task it
//   switches out until its trap returns, so the mark is cleared at that
//   CPU's next trap, or when its idle loop next looks (smp_settle).
// - A CPU with nothing to run parks, CPU 0 in its idle task's nap and
//   the others in CWAI; a trap that leaves work some idle CPU could do
//   sends it an IPI (smp_kick), which arrives as a cycle interrupt.
#define sched_lockdepth (cpu_self->lockdepth)
#define acct_in (cpu_self->acct_in)
#define acct_out (cpu_self->acct_out)
#define sched_slice0 (cpu_self->slice0)
#define sched_resched (cpu_self->resched)
#define sched_dying (cpu_self->dying)
#define sched_dying_code (cpu_self->dying_code)

struct cpu *cpu_self;
static struct cpu cpu_boot;
int smp_ncpu;
static int smp_idle[CPU_MAX];        // 1: that CPU runs its idle task
static int smp_park[CPU_MAX];        // what an idle CPU parks on: only an IPI wakes it
static int smp_stack[CPU_MAX];       // each other CPU's boot stack, its idle task's
static struct task *smp_idle_t[CPU_MAX];   // ... and that idle task
static int smp_stopping;             // shutdown: the other CPUs halt
static int smp_halted;               // ... and count themselves out
static int smp_slots;                // library slots given out (loader.c)
// Spins before a CPU parks on a lock: a hold is usually a syscall's
// worth, much less than a park and a wake.
enum { BKL_SPIN = 64, SMP_STACK_BYTES = 65536 };

void sched_early() {
    cpu_self = &cpu_boot;
}

// ---- locks ----
//
// A kernel lock is two words: 1 + the CPU holding it (0 if none), and
// its holds, nested, so a CPU can take one it has. A CPU spins a little
// on one held elsewhere, then parks in CWAI until the holder lets go.
// On one CPU there is nothing to lock against, and these do nothing.
void klock_take(int *lk) {
    int me, o, spin;
    if (smp_ncpu < 2) return;
    me = cpu_self->id + 1;
    if (lk[0] == me) { ++lk[1]; return; }
    spin = 0;
    while ((o = __c4_cas((int)lk, 0, me))) {
        if (++spin > BKL_SPIN) { __c4_wait((int)lk, o); spin = 0; }
    }
    lk[1] = 1;
}

void klock_drop(int *lk) {
    if (smp_ncpu < 2) return;
    if (--lk[1]) return;
    __c4_xchg((int)lk, 0);
    __c4_wake((int)lk, 1);
}

// The big one, the KERNEL LOCK: everything in the kernel but what has a
// lock of its own -- so far, the tasks' memory (task.c, umem_lock), which
// a program's malloc and free reach without it (sched_trap's light path).
static int bkl[2];
static void bkl_take() { klock_take(bkl); }
static void bkl_drop() { klock_drop(bkl); }

// Whether N may run on CPU K by its mask (pin, SYS_AFFINITY). A mask
// naming no CPU this machine has is no mask at all.
static int sched_allows(struct task *n, int k) {
    int m;
    if (!n->cpumask) return 1;
    m = n->cpumask & ((1 << smp_ncpu) - 1);
    return !m || (m & (1 << k)) != 0;
}

// Whether this CPU may run N: not while another CPU is on its stack,
// an idle task only on its own CPU, a kernel task only on CPU 0, and a
// pinned task only on its CPUs.
static int sched_mine(struct task *n) {
    int me;
    if (smp_ncpu < 2) return 1;
    me = cpu_self->id + 1;
    if (n->oncpu && n->oncpu != me) return 0;
    if (n->cpuidle) return n->cpuidle == me;
    if (n->privs == PRIV_KERNEL) return me == 1;
    return sched_allows(n, me - 1);
}

// This CPU is off the stack of the task it last switched out. Under the
// lock: at a trap's start, or in an idle loop.
static void smp_settle() {
    struct task *p;
    if (!(p = cpu_self->prev)) return;
    if (p != sched_cur && p->oncpu == cpu_self->id + 1) p->oncpu = 0;
    cpu_self->prev = 0;
}

int task_busy(struct task *t) {
    if (smp_ncpu < 2) return t == sched_cur;
    return t->oncpu != 0;
}

// what ps and top say a task runs on
int task_cpu(struct task *t) {
    if (t->state != TS_RUNNING) return 0 - 1;
    if (smp_ncpu < 2) return 0;
    return t->oncpu - 1;
}

// A shared library's __c4_dbase slot, one word of the window: every CPU
// points it at the data of the task it runs (loader_switch)
int smp_slot_alloc() {
    if (smp_slots >= CPU_SLOTS) return 0;
    ++smp_slots;
    return CPU_WINDOW + (CPU_SLOT0 + smp_slots - 1) * sizeof(int);
}

// An idle CPU other than this one that N may run on, the highest, or -1
static int smp_idlefor(struct task *n) {
    int k;
    k = smp_ncpu - 1;
    while (k >= 0) {
        if (k != cpu_self->id && smp_idle[k] && sched_allows(n, k)) return k;
        --k;
    }
    return 0 - 1;
}

// Work some idle CPU could do: an IPI to one for each task ready, among
// the CPUs it may run on. Kernel tasks are CPU 0's. Under the lock, at
// the end of every trap; only when some CPU is idle does it look.
static void smp_kick() {
    struct task *n;
    int k, me, kern, idle;
    me = cpu_self->id;
    idle = 0;
    k = 0;
    while (k < smp_ncpu) { if (k != me && smp_idle[k]) ++idle; ++k; }
    if (!idle) return;
    kern = 0;
    n = task_head;
    while (n && !kern) {
        if (n->privs == PRIV_KERNEL && !n->cpuidle && !n->oncpu &&
            (n->state == TS_READY || sched_unblocked(n))) kern = 1;
        n = n->next;
    }
    if (kern && me != 0 && smp_idle[0]) { __c4_ipi(0); smp_idle[0] = 0; --idle; }
    n = task_head;
    while (n && idle > 0) {
        if (n->privs != PRIV_KERNEL && !n->cpuidle && !n->oncpu &&
            (n->state == TS_READY || sched_unblocked(n)) && (k = smp_idlefor(n)) >= 0) {
            __c4_ipi(k); smp_idle[k] = 0; --idle;
        }
        n = n->next;
    }
}

// Something this CPU could run now: an idle loop asks, under the lock,
// before it parks
static int sched_ready_here() {
    struct task *n;
    n = task_head;
    while (n) {
        if (!n->cpuidle && sched_mine(n) && (n->state == TS_READY || sched_unblocked(n))) return 1;
        n = n->next;
    }
    return 0;
}

// ---- accounting ----
//
// Every cycle belongs to someone (docs/c4ix-cpu.md). From the end of one
// trap to the start of the next, the running task ran its own code; from
// the start of a trap to its end the kernel ran on behalf of the task that
// trapped. So the handler reads the counter twice, and the two differences
// are the whole of it: no per-switch stamps, and nothing the kernel does
// goes to whichever task happened to be switched in afterwards.
//
// The idle loop's cycles are idle time, and so is the time it naps, during
// which the counter stands still: that is counted in machine milliseconds,
// so System Monitor can weigh the two together.
//
// The differences are words, and the VM's counter wraps -- a difference
// is right modulo the word as long as it is less than half of it, and a
// trap at least every preemption slice sees to that.
// (acct_in and acct_out, the two readings, are each CPU's: struct cpu)
static int acct_napms;
// And by CPU (docs/c4ix-smp.md S4): the cycles each ran, busy (every
// task but its idle task) or idle, and the machine ms it parked, which
// for CPU 0 are acct_napms. Added to with the tasks' own, under the
// kernel lock; a parked CPU adds its own ms.
static long long acct_cbusy[CPU_MAX];
static long long acct_cidle[CPU_MAX];
static int acct_cnap[CPU_MAX];

static void acct_cpu(struct task *t, int d) {
    if (t->cpuidle) acct_cidle[cpu_self->id] = acct_cidle[cpu_self->id] + d;
    else acct_cbusy[cpu_self->id] = acct_cbusy[cpu_self->id] + d;
}

// Under SMP an idle CPU's park or nap ends when it is woken, which is an
// IPI: its trap switches to the work at once, before the idle loop's next
// line, so the park is closed in the trap, or by that line if it comes
// first. (Closed by the loop alone, it was charged the time the CPU then
// spent running tasks too.) A park still going is the snapshot's to count
// as far as it has got -- a CPU can park for many seconds -- so where each
// stands is here, where every CPU can read it, not in the CPU's window.
static int acct_parked[CPU_MAX];
static int acct_parked_at[CPU_MAX];

static void acct_park() {
    acct_parked_at[cpu_self->id] = __time();
    acct_parked[cpu_self->id] = 1;
}
static void acct_unpark() {
    int d, k;
    k = cpu_self->id;
    if (!acct_parked[k]) return;
    acct_parked[k] = 0;
    d = __time() - acct_parked_at[k];
    if (k) acct_cnap[k] = acct_cnap[k] + d;
    else acct_napms = acct_napms + d;
}

// CPU k's parked ms since boot, the park it is in included
static int acct_napped(int k) {
    int n;
    n = k ? acct_cnap[k] : acct_napms;
    if (smp_ncpu > 1 && acct_parked[k]) n = n + (__time() - acct_parked_at[k]);
    return n;
}
static long long acct_gone_user, acct_gone_kernel;

// An accumulator past 2^30 (or, impossibly, negative) folds now, long
// before it could overflow; -2^30 is that mask at either word size.
enum { ACCT_FOLD_AT = -1073741824 };

static void acct_fold(struct task *t) {
    t->ucycles = t->ucycles + t->uacc;
    t->kcycles = t->kcycles + t->kacc;
    t->uacc = 0;
    t->kacc = 0;
}

// Read, not folded: under SMP the CPU running a task adds to its
// accumulators without the kernel lock (sched_trap's light path), so only
// that CPU folds them, and a task that has stopped.
long long task_cycles(struct task *t) {
    return t->ucycles + t->uacc + t->kcycles + t->kacc;
}

// A task leaving the table takes its cycles to the reaped totals, so the
// sum of everything ever run never goes backwards.
void sched_retire(struct task *t) {
    if (t->state == TS_ZOMBIE) --sched_nzombies;
    else if (t != kernel_task && !t->cpuidle) --sched_nlive;
    if (t == idle_task) idle_task = 0;   // shutdown: sched_pick must not fall back on it
    acct_fold(t);
    acct_gone_user = acct_gone_user + t->ucycles;
    acct_gone_kernel = acct_gone_kernel + t->kcycles;
}

// The snapshot behind System Monitor and top: one trap, so nothing runs
// between the first record and the last.
// The bytes a task holds: its image, its stack, and what it allocated
// (its ublocks, each with its header, task.c)
static int task_mem(struct task *t) {
    int b;
    b = t->img_bytes;
    if (t->stack) b = b + t->stack_words * sizeof(int);
    return b + task_umem(t);
}

int sched_cpustat(struct cpustat *h, struct cpustat_task *r, int max) {
    struct task *t;
    struct cpustat_task *p;
    int n, i;

    n = 0;
    t = task_head;
    while (t && n < max) {
        p = &r[n];
        p->id = t->id;
        p->parent = t->parent;
        p->state = t->state;
        p->privs = t->privs;
        p->nsyscalls = t->nsyscalls;
        p->user = t->ucycles + t->uacc;       // read, not folded (task_cycles)
        p->kernel = t->kcycles + t->kacc;
        p->mem = task_mem(t);
        p->cpu = task_cpu(t);
        i = 0;
        while (i < TASK_NAME_MAX) { p->name[i] = t->name[i]; ++i; }
        ++n;
        t = t->next;
    }
    h->ntasks = n;
    h->ms = __time();
    h->napms = acct_napms;
    h->idle = idle_task ? idle_task->id : -1;
    h->gone_user = acct_gone_user;
    h->gone_kernel = acct_gone_kernel;
    // the heap, where the host says (libjs: ANNOUNCED, so never probed)
    h->heap = 0;
    h->heapused = 0;
    if (host_has(C4IX_I_C4MJS)) {
        h->heap = *(int *)C4IX_HEAP_END - *(int *)C4IX_HEAP_BASE;
        h->heapused = *(int *)C4IX_HEAP_USED;
    }
    // the processors, CPU 0's naps as its parking
    h->ncpu = smp_ncpu > 1 ? smp_ncpu : 1;
    i = 0;
    while (i < CPU_MAX) {
        h->idles[i] = 0 - 1;
        h->cnap[i] = acct_napped(i);
        h->cbusy[i] = acct_cbusy[i];
        h->cidle[i] = acct_cidle[i];
        ++i;
    }
    h->idles[0] = h->idle;
    i = 1;
    while (i < h->ncpu) { if (smp_idle_t[i]) h->idles[i] = smp_idle_t[i]->id; ++i; }
    return n;
}

int sched_switches;          // context switches since boot
// (sched_reload, sched_stale_stack, sched_slice0, sched_resched and
// sched_dying are each CPU's: struct cpu)

// ---- preemption masking (nestable) ----
//
// The depth belongs to the TASK, not to the machine. It has to: the
// mask says "do not preempt me", and if it were global then a task
// that held it across a context switch would hand the next task an
// interrupt-masked machine. A compute-bound task landing in that
// state never traps again -- it just runs, burning cycles, while
// nothing else is scheduled and nothing can stop it. (C4KE shows the
// same failure: a rogue benchmark with a cycle count thousands of
// times everyone else's, issuing no traps.) So the switch saves the
// outgoing depth and restores the incoming one, and the re-arm at
// the end of the handler reflects whoever is about to run.

// Inside the trap handler the interrupt is already off and the
// handler re-arms it on the way out, so the hardware half is skipped
// there -- re-arming mid-handler would invite a nested interrupt.
//
// Under SMP the mask is also the kernel lock, outside the handler: the
// handler holds that already. Masked first, so that nothing preempts a
// CPU holding it.
void sched_lock() {
    if (!sched_intrap) {
        if (sched_lockdepth == 0 && sched_interval) __c4_configure(C4IX_CONF_INTERVAL, 0);
        bkl_take();
    }
    ++sched_lockdepth;
}

void sched_unlock() {
    --sched_lockdepth;
    if (!sched_intrap) {
        bkl_drop();
        if (sched_lockdepth == 0 && sched_interval) __c4_configure(C4IX_CONF_INTERVAL, sched_interval);
    }
}

// The running task's slice starts again now: after a service that
// spent its slice in the kernel on the task's behalf (SYS_SPAWN).
void sched_new_slice() {
    sched_slice0 = __c4_cycles();
}

// ---- picking the next task ----

// What a parked task is waiting for, asked as sched_pick passes it. (A
// TS_WAITING task is not asked: task_zombify wakes it when its target
// dies, below.)
// A TS_BLOCKED task is parked on a vnode -- an empty pipe. It wakes
// when the data it wanted arrives, or when the last writer closes
// and the emptiness becomes end-of-file instead.
// A TS_SLEEPING task is parked on the clock. Subtract rather than
// compare the raw values -- the habit that survives a wrap.
static int sched_unblocked(struct task *n) {
    if (n->state == TS_SLEEPING) {
        if (__time() - n->ck_wake < 0 && !(n->ev_wait && sched_events(n))) return 0;
        n->ev_wait = 0;
        n->state = TS_READY;
        n->woke = 1;
        return 1;
    }
    if (n->state != TS_BLOCKED) return 0;
    if (!vfs_readable(n->block_vn, n->block_pos)) return 0;
    n->state = TS_READY;
    n->woke = 1;
    return 1;
}

// What SYS_EVWAIT waits for besides the clock: an event in the task's GUI
// ring, whose header is [capacity, head, tail] -- the host moves the head,
// the task the tail -- or data, or end of file, on a pipe it reads.
int sched_events(struct task *t) {
    struct file *f;
    int fd, *ring;
    if (t->ev_wait & 1) {
        ring = (int *)t->ev_ring;
        if (ring && ring[1] != ring[2]) return 1;
    }
    if (t->ev_wait & 2) {
        fd = 0;
        while (fd < FD_MAX) {
            f = (struct file *)t->fds[fd];
            if (f && f->vn->type == VN_PIPE && (f->flags & 3) == C4IX_O_RDONLY
                && (f->vn->size > f->vn->rpos || !f->vn->writers)) return 1;
            ++fd;
        }
    }
    return 0;
}

// The earliest deadline among sleeping tasks, or 0 if none. The idle
// loop uses it to cap its nap so a short sleep is not served late.
int sched_sleep_due(int *pms) {
    struct task *t;
    int found;

    found = 0;
    t = task_head;
    while (t) {
        if (t->state == TS_SLEEPING) {
            if (!found || t->ck_wake - *pms < 0) { *pms = t->ck_wake; found = 1; }
        }
        t = t->next;
    }
    return found;
}

// ---- the living and the dead ----
//
// Both counted as they change, so that nothing scans the table to find
// out. sched_nlive is the tasks still to finish: all but the kernel and
// idle tasks and the zombies (background jobs too), and shutdown is when
// it reaches 0. sched_nzombies says whether the orphan reaper has
// anything to look for. task.c counts a task in as it joins the table;
// task_zombify and sched_retire count them out.
int sched_nlive;
int sched_nzombies;

// Every death comes through here: a task's exit, a Ctrl-C, a signal's
// default action. Whoever waits on the task is woken now, with its code,
// and reaps it -- the event sched_pick used to look for, on every pick,
// by asking each waiting task whether its target had died yet. A task
// that starts waiting later finds the zombie and reaps it itself.
//
// A waitpid waiter (wait_posix) is only woken: its syscall runs again and
// reaps whichever child it finds, so waiting for any child (-1) works too.
//
// A vfork child's death gives its parent its memory back; a vfork
// parent's takes the child with it, since what it was running in goes.
void task_zombify(struct task *t, int code) {
    struct task *w;
    int was;
    if (t->state == TS_ZOMBIE) return;
    was = t->state;
    t->exitcode = code;
    t->state = TS_ZOMBIE;
    ++sched_nzombies;
    if (t != kernel_task && !t->cpuidle) --sched_nlive;
    task_unborrow(t);
    sys_display_drop(t->id);      // the desktop has the display back
    if (was == TS_VFORK && (w = task_get(t->wait_for)) && w->vfparent == t) task_zombify(w, code);
    w = task_head;
    while (w) {
        if (w->state == TS_WAITING && w->wait_posix &&
            (w->wait_for == t->id || (w->wait_for == -1 && t->parent == w->id))) {
            w->wait_posix = 0;
            w->state = TS_READY;
            w->woke = 1;
            return;
        }
        if (w->state == TS_WAITING && w->wait_for == t->id && !w->wait_posix) {
            // into wait_result for a kernel caller in task_wait, and into
            // sv_a for a user task parked mid-syscall: the switch restores
            // sv_a into the accumulator, where SYS_WAIT's result belongs
            w->wait_result = code;
            w->sv_a = code;
            w->state = TS_READY;
            w->woke = 1;
            task_release(t);
            return;
        }
        w = w->next;
    }
}

// Round-robin after t: the next runnable task; else t itself, if it can
// still run; else the idle task, which never takes a turn of its own.
// A task that has just died may be off the list already -- its waiter
// reaped it -- so the walk starts from the head for a zombie, and stops
// when it comes back round to where it started.
// Under SMP, only what this CPU may run (sched_mine), and the idle task
// is this CPU's.
static struct task *sched_pick(struct task *t) {
    struct task *n, *start, *idle;
    idle = cpu_self->idle;
    start = (t->state == TS_ZOMBIE) ? task_head : task_next(t);
    n = start;
    while (n) {
        if (n != t && n != idle && !n->cpuidle && sched_mine(n)) {
            if (n->state == TS_READY) return n;
            if (sched_unblocked(n)) return n;
        }
        n = task_next(n);
        if (n == start) n = 0;
    }
    if ((t->state == TS_RUNNING || t->state == TS_READY || sched_unblocked(t)) &&
        (t == idle || t->cpuidle || sched_mine(t))) return t;
    return idle ? idle : t;
}

// ---- the c4m backend: switching inside a trap handler ----

// The kernel's single trap entry: yields, preemption, syscalls and
// protected-mode violations all arrive here. Assigning to the
// a/bp/sp/returnpc/mode parameters changes what TLEV resumes with --
// that is both the context switch and the syscall return path.
//
// c4m enters a handler unprotected with the cycle interrupt off, so
// the kernel always runs privileged; the mode assignment on the way
// out is what puts a user task back behind the boundary.
//
// The leading `interval` parameter is the frame slot c4m saves the
// cycle interrupt interval into, and it is a register like the rest:
// assigning to it says what the interrupt state should be for the
// context this trap returns to. That is the only safe way to re-arm
// preemption. Doing it with __c4_configure before returning arms the
// machine while the switch is still only half done -- sched_cur has
// moved but the registers have not, because they do not change until
// TLEV runs -- and an interrupt in that window saves the outgoing
// task's registers into the incoming task and restores the incoming
// task's stale ones. The two contexts trade places, one then rebuilds
// a trap frame over the frame the other is parked on, and the machine
// ends up spinning on a TLEV that returns to itself forever.
// ---- priority: nice ----
//
// A task's nice is -10, the most favoured, to 10; 0 for most, and when
// every task has 0 -- until something sets one -- scheduling is the round
// robin it always was. A favoured task is served sooner and longer, but
// never to the exclusion of the rest, which a game drawing in a window
// needs as much as anyone, since the desktop draws it:
// - sooner: when it wakes, from a sleep or a wait, it preempts a less
//   favoured task at once, instead of waiting out that task's slice;
// - longer: a turn of 1 - nice slices in a row, where the others' is one.
// The turns still go round, so a favoured task that never sleeps cannot
// starve the others; a task with a positive nice is only ever preempted
// by a woken task with a lower one. A child starts with its parent's.
static int sched_niced;          // some task has had a nice set

// the cycles a task runs before its turn passes
static int sched_turn(struct task *t) {
    return t->nice < 0 ? sched_interval * (1 - t->nice) : sched_interval;
}

// A task that has woken and is more favoured than T, this CPU's to run:
// the most favoured, or 0
static struct task *sched_waker(struct task *t) {
    struct task *n, *best;
    best = 0;
    n = task_head;
    while (n) {
        if (n != t && !n->cpuidle && n->nice < t->nice && (!best || n->nice < best->nice) &&
            sched_mine(n) && (n->state == TS_READY || sched_unblocked(n)) && n->woke) best = n;
        n = n->next;
    }
    return best;
}

// SYS_NICE: task ID's nice; one outside -10..10 only asks. The one it had,
// or -100: no such task, or one that is not a program's.
int sched_nice(int id, int nice) {
    struct task *t;
    int old;
    sched_lock();
    t = task_get(id);
    if (!t || t->cpuidle || t->privs == PRIV_KERNEL) { sched_unlock(); return 0 - 100; }
    old = t->nice;
    if (nice >= 0 - 10 && nice <= 10) {
        t->nice = nice;
        if (nice) sched_niced = 1;
    }
    sched_unlock();
    return old;
}

// Under SMP, may this trap be served without the kernel lock? A protected
// program's malloc or free -- a compile makes thousands -- touches only
// the heap, whose books are the host's and every thread's (libjs sheap.js),
// and the program's own list of blocks, under its own lock (task.c,
// umem_lock). So it may, as long as nothing else is to be done in this
// trap: the task runs on with its slice left, nothing is pending for it,
// and this CPU has nothing to settle. Everything else takes the lock, and
// does what the light path leaves for later -- the reaping, the stack
// guard -- at the next trap.
static int sched_light(int trap, int param) {
    struct task *t;
    t = sched_cur;
    return trap == C4IX_TRAP_PM && (param == C4IX_OP_MALC || param == C4IX_OP_FREE) &&
        t && t->state == TS_RUNNING && t->privs == PRIV_USER && !t->ck_sigpend &&
        !sched_dying && !sched_resched && !sched_reload && !cpu_self->prev &&
        (!sched_interval || acct_in - sched_slice0 < sched_turn(t));
}

static void sched_trap(int interval, int trap, int param, int mode, int a, int bp, int sp, int returnpc) {
    struct task *t, *n, *me, *fv;
    int reload, hold;

    // Re-entry guard. c4m disables the cycle interrupt when IT takes
    // a trap, but not when user code raises one (the syscall gateway
    // and __c4_trap), so an interrupt can still land in the window
    // between entering this handler and the line below. The handler
    // keeps its state in globals and is not re-entrant, so a nested
    // invocation returns immediately: TLEV then resumes the outer
    // handler exactly where it was, and the interrupt stays disabled
    // (c4m's own cycle path clears the interval) until the outer
    // handler re-arms it on the way out.
    if (sched_intrap) return;
    // Claim the handler BEFORE anything else, including the mask
    // below. Neither the syscall gateway nor __c4_trap disables the
    // cycle interrupt on the way in, so an interrupt can land in the
    // few instructions between entry and that __c4_configure -- and
    // if the flag were still clear it would run this whole handler
    // recursively, on top of the switch already in progress. Setting
    // it first makes that window a no-op instead of corruption.
    //
    // In-trap services also read this flag: yield and exit become
    // state changes the switch below acts on, rather than re-entering
    // the trap machinery.
    sched_intrap = 1;
    // Until now the task's own code ran; from here, the kernel on its
    // behalf (accounting, above). It is the one charged on the way out
    // too, whoever that switches to.
    acct_in = __c4_cycles();
    if (smp_ncpu > 1) {
        // The kernel is one CPU's at a time; then this CPU is off the
        // stack of the task it last switched out. (A task's accumulators
        // are its CPU's alone: a snapshot reads them, folds nothing.)
        __c4_configure(C4IX_CONF_INTERVAL, 0);
        // The light path: a program's malloc or free that leaves it
        // running is served without the kernel lock (sched_light)
        if (sched_light(trap, param)) {
            me = sched_cur;
            ++me->ntraps;
            me->uacc = me->uacc + (acct_in - acct_out);
            acct_cpu(me, acct_in - acct_out);
            sys_pmviolation(param, (int *)sp, (int *)returnpc, &a);
            acct_out = __c4_cycles();
            me->kacc = me->kacc + (acct_out - acct_in);
            acct_cpu(me, acct_out - acct_in);
            if ((me->uacc | me->kacc) & ACCT_FOLD_AT) acct_fold(me);
            mode = C4IX_MODE_PROTECTED;
            interval = (sched_interval && sched_lockdepth == 0) ? sched_interval : 0;
            sched_intrap = 0;
            return;
        }
        bkl_take();
        smp_settle();
        acct_unpark();
        // Killed by another CPU while it ran here: nothing of it is to be
        // served, only switched away from
        if (sched_cur && sched_cur->state == TS_ZOMBIE) trap = C4IX_TRAP_SOFT_IRQ;
    }
    if ((me = sched_cur)) {
        ++me->ntraps;
        me->uacc = me->uacc + (acct_in - acct_out);
        acct_cpu(me, acct_in - acct_out);
    }

    // no nested switches while kernel structures move
    __c4_configure(C4IX_CONF_INTERVAL, 0);

    // Safe point: we are on the trapped task's stack, so any corpse
    // left over from an earlier exit can finally be freed. Usually
    // there is none, and then not even the call.
    if (task_reaplist) task_reap();
    // the stack an execve left, which its handler was standing on
    if (sched_stale_stack) { free((int *)sched_stale_stack); sched_stale_stack = 0; }
    // The stack guard. A task whose stack pointer is inside the guard,
    // or whose top guard word has been written, ran off the end of its
    // stack: kill it here, before what it wrote under the stack -- some
    // other allocation -- takes the machine down in some other task.
    if (sched_cur && sched_cur->stack && sched_cur->privs == PRIV_USER && sched_cur->state != TS_ZOMBIE &&
        (sp < sched_cur->stack + C4IX_STACK_GUARD * sizeof(int) ||
         ((int *)sched_cur->stack)[C4IX_STACK_GUARD - 1] != C4IX_STACK_POISON)) {
        kprintf("c4ix: task %d (%s) overran its %d-byte stack; killing it\n",
            sched_cur->id, sched_cur->name, sched_cur->stack_words * sizeof(int));
        task_exit(-1);
        trap = C4IX_TRAP_SOFT_IRQ;   // nothing left to service: just switch
    }
    // a task_exit from outside the handler (above) takes effect here
    if (sched_dying && me) {
        sched_dying = 0;
        task_zombify(me, sched_dying_code);
        trap = C4IX_TRAP_SOFT_IRQ;
    }
    if (trap == C4IX_TRAP_ILLOP) {
        if (param >= SYS_BASE && param < SYS_TOP) {
            // OPCD leaves the syscall number at *sp with the
            // arguments above it. Cast BEFORE the offset: sp is an
            // int parameter, so "sp + 1" would step one byte.
            // vfork needs the trap's frame pointer, so it is served here
            if (param == SYS_VFORK) a = sched_vfork(bp);
            else a = sys_dispatch(param, (int *)sp + 1);
        } else if (param >= CK_BASE && param < CK_TOP) {
            // A C4KE program. Same door, same argument shape --
            // C4KE's ABI puts the opcode at sp[0] too.
            a = ck_dispatch(param, (int *)sp + 1);
        } else {
            kprintf("c4ix: task %d hit illegal opcode %d\n",
                sched_cur ? sched_cur->id : -1, param);
            task_exit(-1);
        }
    } else if (trap == C4IX_TRAP_OPV) {
        // c4m raises this when OPCD is handed a number below ADJ --
        // in practice, opcode 0, which is what a C4KE program ends up
        // executing when a symbol lookup failed and it stored the 0.
        // Without this branch the task would sail on with a wrong
        // result and the real fault would surface somewhere useless.
        kprintf("c4ix: task %d executed opcode %d via OPCD "
                "(an unresolved C4KE symbol?); killing it\n",
            sched_cur ? sched_cur->id : -1, param);
        task_exit(-1);
    } else if (trap == C4IX_TRAP_PM) {
        sys_pmviolation(param, (int *)sp, (int *)returnpc, &a);
    } else if (trap == C4IX_TRAP_SIGNAL) {
        sched_interrupt();
    }

    // The syscall could not finish because the task had to block.
    // Rewind one word so it re-executes the syscall opcode when it
    // wakes: the arguments are still on its own stack, untouched, so
    // the call simply happens again. (Both doors trap one word past
    // a single-word opcode, so the arithmetic is the same for the
    // gateway and for an emulated host opcode.)
    if (sys_restart) {
        returnpc = returnpc - sizeof(int);
        sys_restart = 0;
    }

    // The fast path. A syscall, or an opcode emulated for a protected
    // task, that left the task able to run and asked for no switch goes
    // straight back to it while its slice lasts. Picking costs the same
    // whether anything changed or not, and most syscalls change nothing:
    // a task they wake waits at most the rest of a slice. The slice is
    // kept here, in software, because c4m's cycle interrupt fires only
    // when the counter is a multiple of the interval, and every multiple
    // that falls inside the kernel is lost -- without this a task making
    // a syscall at the wrong moments would never be preempted.
    t = sched_cur;
    // a favoured task that has woken goes first (nice, above)
    fv = (sched_niced && sched_lockdepth == 0 && t->state == TS_RUNNING && !t->cpuidle)
        ? sched_waker(t) : 0;
    if (fv) {
        n = fv;
        sched_slice0 = acct_in;
    }
    else if ((trap == C4IX_TRAP_ILLOP || trap == C4IX_TRAP_PM) && !sched_resched
        && t->state == TS_RUNNING && (t->cpuidle || sched_mine(t))
        && (!sched_interval || acct_in - sched_slice0 < sched_turn(t)))
        n = t;
    // An IPI arrives whatever the mask: a task that masked preemption (an
    // idle loop under sched_lock, a C4KE program's exclusive section)
    // keeps the CPU. What it was sent for is found when the mask lifts.
    else if (trap == C4IX_TRAP_HARD_IRQ && sched_lockdepth > 0 && t->state == TS_RUNNING)
        n = t;
    // a favoured task's turn is several slices
    else if (trap == C4IX_TRAP_HARD_IRQ && t->nice < 0 && t->state == TS_RUNNING && !sched_resched &&
             sched_mine(t) && acct_in - sched_slice0 < sched_turn(t))
        n = t;
    else {
        n = sched_pick(t);
        // Several CPUs: a task preempted while another CPU idles keeps
        // this one, and what is ready goes to the idle one (smp_kick).
        // Switched out here, it could not: it would be this CPU's until its
        // next trap, which would switch the two back, and so on, one CPU
        // taking turns between them while the other parked. (Preempted: its
        // slice ran out, at the cycle interrupt or in a syscall; not a yield.)
        if (smp_ncpu > 1 && n != t && t->state == TS_RUNNING && !t->cpuidle && n->privs == PRIV_USER &&
            sched_mine(t) &&
            (trap == C4IX_TRAP_HARD_IRQ || ((trap == C4IX_TRAP_ILLOP || trap == C4IX_TRAP_PM) && !sched_resched)) &&
            smp_idlefor(n) >= 0) n = t;
        sched_slice0 = acct_in;
    }
    sched_resched = 0;
    // An execve forged the running task a new context: it is resumed
    // from that even if it is picked again, and the trap's registers,
    // the old image's, are not saved over it.
    reload = sched_reload;
    sched_reload = 0;
    if (n != t || reload) {
        if (!reload) {
            t->sv = SV_REGS;
            t->sv_a = a; t->sv_bp = bp; t->sv_sp = sp; t->sv_pc = returnpc;
        }
        if (t->state == TS_RUNNING) t->state = TS_READY;
        if (n->sv == SV_REGS) {
            a = n->sv_a; bp = n->sv_bp; sp = n->sv_sp; returnpc = n->sv_pc;
        } else {
            // SV_FRAME: registers derive from the pair. sp lands just
            // past it, exactly where the frame's LEV would have left it.
            a = 0;
            bp = n->sv_bp;
            sp = n->sv_sp + 2 * sizeof(int);
            returnpc = n->sv_pc;
        }
        // the mask travels with the task, not with the machine
        t->lockdepth = sched_lockdepth;
        sched_lockdepth = n->lockdepth;
        n->state = TS_RUNNING;
        n->woke = 0;
        sched_cur = n;
        if (smp_ncpu > 1) {
            // this CPU stays on t's stack until the trap returns
            if (n != t) cpu_self->prev = t;
            n->oncpu = cpu_self->id + 1;
            smp_idle[cpu_self->id] = (n == cpu_self->idle);
        }
        loader_switch(n);
        ++sched_switches;
    } else if (t->state == TS_ZOMBIE) {
        // The task died and nothing else is runnable: with no context
        // to resume, the kernel cannot return from this trap.
        kputs("c4ix: panic: last task exited with nothing to switch to\n");
        exit(-1);
    }

    // A user task resumes behind the boundary; the kernel does not.
    mode = (sched_cur->privs == PRIV_USER)
        ? C4IX_MODE_PROTECTED : C4IX_MODE_UNPROTECTED;

    // Preemption for whoever is about to run. sched_lockdepth was
    // swapped above, so it already belongs to the incoming task: a
    // task that trapped while holding the mask gets it back, and one
    // that did not gets a running clock. Writing the frame slot rather
    // than the machine leaves the interrupt off until TLEV has finished
    // installing the context -- there is no half-switched window for it
    // to land in.
    interval = (sched_interval && sched_lockdepth == 0) ? sched_interval : 0;

    // If the task about to run has a signal waiting, enter its handler
    // instead -- by rewriting the very registers just settled on.
    //
    // This is the ONLY delivery point, and it is here for a reason.
    // Above, a SV_FRAME task has already been turned into real
    // registers, so this never sees a forged frame; and whether or not
    // a switch happened, a/bp/sp/returnpc now describe the incoming
    // context, so the same code serves both. The mode and interval
    // just computed are handed over to be stored in the frame the
    // handler returns through, while this frame keeps them -- which is
    // what runs the handler itself in the task's own mode. Almost no
    // task has a signal waiting, and the count says so without a call.
    if (sched_cur->ck_sigpend)
        ck_signal_deliver(sched_cur, &a, &bp, &sp, &returnpc, mode, interval);

    acct_out = __c4_cycles();
    if (me) {
        me->kacc = me->kacc + (acct_out - acct_in);
        acct_cpu(me, acct_out - acct_in);
        if ((me->uacc | me->kacc) & ACCT_FOLD_AT) acct_fold(me);
    }

    // The lock: work for an idle CPU first, while it is still held; then
    // a kernel task keeps a hold of its own while it runs, and the
    // handler lets go of its.
    if (smp_ncpu > 1) {
        smp_kick();
        hold = sched_cur->privs == PRIV_KERNEL && !sched_cur->cpuidle;
        if (cpu_self->bkl_task && !hold) { bkl_drop(); cpu_self->bkl_task = 0; }
        else if (!cpu_self->bkl_task && hold) { bkl_take(); cpu_self->bkl_task = 1; }
        bkl_drop();
    }

    // The flag stays set for the WHOLE handler, not just the service
    // call: everything below it -- task_release, the slab allocator,
    // any kprintf -- takes sched_lock, and an unlock outside the flag
    // would re-arm the cycle interrupt while the handler is still
    // mid-switch, letting a nested trap corrupt the switch it was
    // performing.
    sched_intrap = 0;
}

// Ctrl-C. c4m catches the host signal, records it, and raises
// TRAP_SIGNAL at the next instruction boundary with whatever handler
// was registered -- so it arrives here like any other trap.
//
// What should die is the program that is running, not the machine,
// and not whichever task the signal happened to land on. Since the
// console stopped blocking the host, the idle loop runs alongside
// user work, so "the current task" is as likely to be the kernel's
// idle loop as the program the person meant to stop.
//
// C4IX has no process groups and does not need them. A terminal picks
// its foreground job from a structure that is already here: init waits
// on the program it launched, a shell waits on the job it is running,
// so DESCEND THE WAIT CHAIN and the innermost task is the one in
// front. C4KE asks its programs to declare themselves instead
// (c4ke_set_focus), which is why the compat layer accepts that call
// and ignores it -- this answer is derived, and cannot go stale.
// Whom a parked task is waiting on: its vfork child, the task its wait
// names, or for a waitpid(-1) its newest child still alive.
static struct task *sched_waitee(struct task *t) {
    struct task *n, *c;
    if (t->state != TS_WAITING && t->state != TS_VFORK) return 0;
    if (t->wait_for != -1) return task_get(t->wait_for);
    c = 0;
    n = task_head;
    while (n) {
        if (n->parent == t->id && n->state != TS_ZOMBIE) c = n;
        n = n->next;
    }
    return c;
}

static struct task *sched_foreground() {
    struct task *t, *n;
    int guard;

    // The session's top-level program: whatever a kernel task waits on
    // -- other than the kernel itself, which waits on the idle task.
    t = 0;
    n = task_head;
    while (n) {
        if (n != kernel_task && n->privs == PRIV_KERNEL && n->state == TS_WAITING) {
            if ((t = task_get(n->wait_for))) break;
        }
        n = n->next;
    }
    if (!t) return 0;
    // Then inwards, one job at a time. The guard is paranoia about a
    // wait cycle, which should be impossible.
    guard = 0;
    while (guard < 32) {
        if (!(n = sched_waitee(t))) break;
        t = n;
        ++guard;
    }
    return (t->state == TS_ZOMBIE) ? 0 : t;
}

// A task parked on console input is a shell waiting to be typed at,
// which means the Ctrl-C was typed AT it. Nothing to cancel -- and in
// particular not a background job, which is not in front of anything.
static int sched_reading_console(struct task *t) {
    return t && t->state == TS_BLOCKED && t->block_vn
        && t->block_vn->type == VN_CONSOLE;
}

// Interrupt the foreground job below one task -- a terminal's own Ctrl-C.
// The console's Ctrl-C starts from the session's top-level program; a
// terminal window (c4ix-desktop) starts from the shell it runs, and
// descends the same wait chain from there. Nothing is printed: the caller
// is the terminal, and it shows ^C in its own window. 1 if a job was
// cancelled or signalled, 0 if the root was not waiting on anything.
int sched_intr_below(struct task *root) {
    struct task *t, *n;
    int guard;
    if (!root) return -1;
    t = root;
    guard = 0;
    while (guard < 32) {
        if (!(n = sched_waitee(t))) break;
        t = n;
        ++guard;
    }
    if (t == root || t->state == TS_ZOMBIE || t->privs != PRIV_USER) return 0;
    if (ck_has_handler(t, CK_SIGINT)) { ck_kill(t->id, CK_SIGINT); return 1; }
    t->killsig = CK_SIGINT;
    task_zombify(t, C4IX_EXIT_INTERRUPTED);
    return 1;
}

// Kernel tasks are spared (killing init or kernel ends everything). The
// victim becomes a zombie with a distinguishable status, so whoever
// waits on it learns it was interrupted.
static void sched_interrupt() {
    struct task *t;

    t = sched_foreground();
    if (sched_reading_console(t)) t = 0;
    // No foreground job: fall back to the running task, which covers a
    // program with nothing above it at all.
    if (!t) t = sched_cur;
    if (!t || t->privs != PRIV_USER) {
        kputs("\nc4ix: interrupt (nothing running to cancel)\n");
        return;
    }
    // A C4KE program that installed a SIGINT handler gets to shut
    // itself down; that is the whole point of installing one.
    if (ck_has_handler(t, CK_SIGINT)) {
        kprintf("\nc4ix: interrupt: SIGINT to task %d '%s'\n", t->id, t->name);
        ck_kill(t->id, CK_SIGINT);
        return;
    }
    kprintf("\nc4ix: interrupt: cancelling task %d '%s'\n", t->id, t->name);
    t->killsig = CK_SIGINT;
    task_zombify(t, C4IX_EXIT_INTERRUPTED);
}

// ---- the common face ----

// SYS_AFFINITY: the CPUs task ID may run on, bit k for CPU k, 0 for any;
// MASK < 0 only asks. Its mask before, or -1: no such task, or one that
// is not a program's (kernel tasks are CPU 0's, idle tasks their own
// CPU's). A task running where it may no longer goes at its next trap --
// at once, if it is the caller.
int sched_affinity(int id, int mask) {
    struct task *t;
    int old;
    sched_lock();
    t = task_get(id);
    if (!t || t->cpuidle || t->privs == PRIV_KERNEL) { sched_unlock(); return 0 - 1; }
    old = t->cpumask;
    if (mask >= 0) {
        t->cpumask = mask;
        if (t == sched_cur) sched_resched = 1;
    }
    sched_unlock();
    return old;
}

void sched_yield() {
    if (!sched_cur) return;
    // Already inside the trap handler: it switches on the way out,
    // which IS the yield -- once it knows not to take the fast path.
    // Raising another trap here would nest.
    if (sched_intrap) { sched_resched = 1; return; }
    // A user task gets here in its own context: task_shim, once its entry
    // has returned, dying through task_exit. That is protected mode, and
    // __c4_trap does not leave it (on c4m or libjs), so the handler would
    // run protected, every host opcode in it a nested trap that the
    // re-entry guard drops. It reaps there, so a dead task's frees were
    // all dropped: each cc left c4sc's stack (4 MB then) and its image behind.
    // The syscall gate raises the trap unprotected.
    if (sched_cur->privs == PRIV_USER) { __c4_opcode(0, 0, 0, SYS_YIELD); return; }
    __c4_trap(C4IX_TRAP_SOFT_IRQ, 0);
}

// ---- task start and end ----

static int task_lost() {
    kputs("c4ix: panic: task escaped its shim\n");
    exit(-1);
    return 0;
}

// Every task starts here (via the forged frame): run the image's
// constructors, call the real entry, run its destructors, and turn
// the return value into an exit.
//
// The constructors run HERE rather than at load time because this is
// the first code that executes as the new task. The loader runs in
// the SPAWNING task's context -- the new task does not exist yet, so
// a constructor asking who it is would be told the parent. C4KE's u0
// caches its pid and its parent's and installs eight signal handlers
// in its constructor, so every one of them would have been wired to
// the wrong task.
//
// Destructors run in reverse, the C convention, and it matters here:
// u0's own destructor frees the table the others might use. This is
// the first time C4IX has run them at all -- an X1 debt, closed by
// having somewhere to run them that still has the image mapped.
static int task_shim(int entry, int argc, int argv) {
    struct task *t;
    int e, r, i;

    t = sched_cur;
    i = 0;
    while (i < t->img_ncons) {
        e = ((int *)t->img_cons)[i];
        // The one argument is u0's `int *c4r` module pointer, which it
        // stores and never dereferences. A constructor declared with
        // no parameters simply never reads it.
        e(0);
        ++i;
    }
    e = entry;
    r = e(argc, argv);
    i = t->img_ndes;
    while (i) {
        --i;
        e = ((int *)t->img_des)[i];
        e();
    }
    task_exit(r);
    return 0;   // unreachable
}

// Inside the trap handler a death is a state change, which the switch
// on the way out acts on: someone else is picked, and the zombie never
// comes back. Outside it -- a task whose entry returned, in task_shim --
// the dying task is still running its own code on its own stack, a user
// task behind protected mode, and that is no place to die from:
// task_zombify releases the task to its waiter there and then, and the
// release takes the scheduler lock, which from protected mode is itself
// a trap, out of which the zombie is never picked again, half released.
// So it leaves the code in sched_dying and dies in the trap it raises next.

void task_exit(int code) {
    if (sched_intrap) {
        task_zombify(sched_cur, code);
        return;
    }
    sched_dying_code = code;
    sched_dying = 1;
    // a zombie is never picked again
    while (1) sched_yield();
}

// Block until t exits. No spinning: the caller parks in TS_WAITING
// and task_zombify wakes it with the exit code when t dies (and has
// reaped t by then -- t must not be touched after waking).
int task_wait(struct task *t) {
    struct task *me;
    int code;

    if (t->state == TS_ZOMBIE) {
        code = t->exitcode;
        task_release(t);
        return code;
    }
    me = sched_cur;
    me->wait_for = t->id;
    me->wait_posix = 0;
    me->state = TS_WAITING;
    while (me->state == TS_WAITING) sched_yield();
    return me->wait_result;
}

// Build the stack + fake frame that makes a never-run task resumable:
// sched_trap turns it into registers. Layout (see test_coop_switch.c):
//   X[0] saved bp (junk; the entry's ENT overwrites X[1], not this)
//   X[1] return pc -> task_shim
//   X[2] where task_shim's own LEV would land -> task_lost
//   X[3..5] task_shim's args, last first: argv, argc, entry
int sched_next_stack_words;
// An idle task's stack comes from the kernel arena instead (sched_init)
static int sched_next_kstack;

int sched_forge(struct task *t, int entry, int argc, int argv) {
    int *stk, *x;
    int i, words;

    words = sched_next_stack_words > C4IX_STACK_WORDS ? sched_next_stack_words : C4IX_STACK_WORDS;
    if (sched_next_kstack) {
        words = KMEM_STACK_BYTES / sizeof(int);
        if (!(stk = (int *)kmem_alloc(words * sizeof(int)))) return 0;
    } else if (!(stk = (int *)malloc(words * sizeof(int)))) return 0;
    memset(stk, 0, words * sizeof(int));
    x = stk + words - 8;              // slack above, as C4KE leaves
    t->stack_words = words;
    // the guard: the stack grows down towards it
    i = 0;
    while (i < C4IX_STACK_GUARD) { stk[i] = C4IX_STACK_POISON; ++i; }

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

// ---- vfork ----
//
// The child borrows the parent's image, library data and heap until it
// execs or exits, and the parent waits in TS_VFORK meanwhile: one address
// space, as on any machine without an MMU. What it cannot borrow is the
// parent's stack. On a register machine the child runs on it, below the
// frame that called vfork, and that frame's state is in registers the
// kernel saved. Here the stack IS the registers: the caller F of vfork
// has its expression temporaries pushed below its locals -- in `pid =
// vfork()`, the address of pid -- and the child's first call would write
// its own frame over them, and over vfork's return address.
//
// So the child gets a stack of its own, and the trap returns it from
// vfork straight into F: pc is vfork's return address, bp is F's frame,
// in the parent's stack, so F's locals are the parent's own -- which is
// what vfork promises, and what Busybox's spawn() uses, a child's
// failed = errno that the parent reads -- and sp is in the new stack,
// over a copy of everything between vfork's frame and F's, its
// temporaries among it. A function the child calls builds its frame
// there, and the parent's stack below F is never touched.
//
// BP is the frame of the function that executed SYS_VFORK, libc's vfork:
// it must call nothing, so that its caller is F. The usual rule then
// holds as it does everywhere: the child must not return from F, which
// would put it back on the parent's stack, and must leave by execve or
// _exit.
int sched_vfork(int bp) {
    struct task *p, *c;
    int *w;
    int n, top;

    p = sched_cur;
    ++p->nsyscalls;
    if (p->privs != PRIV_USER) return -1;
    w = (int *)bp;
    // [w + 2, F's bp): what F had pushed when it called, and its locals
    n = w[0] - (bp + 2 * sizeof(int));
    if (n < 0 || n > C4IX_STACK_WORDS * sizeof(int) / 2) return -1;
    if (!(c = task_vchild(p))) return -1;
    top = c->stack + (c->stack_words - 8) * sizeof(int);   // sched_forge's slack
    c->sv = SV_REGS;
    c->sv_a = 0;
    c->sv_bp = w[0];
    c->sv_pc = w[1];
    c->sv_sp = top - n;
    memcpy((char *)c->sv_sp, (char *)(bp + 2 * sizeof(int)), n);
    p->posix = 1;
    p->wait_for = c->id;
    p->state = TS_VFORK;
    return c->id;
}

// Point a not-yet-started task's argv somewhere else. The forged frame
// holds whatever the spawner passed, which for a C4KE program is a
// borrowed pointer the caller is about to reuse or free -- so the
// compat layer copies it and then patches the copy in here. Only
// valid while the task is still SV_FRAME; once it has run, argv is
// wherever its own frame put it.
void sched_forge_argv(struct task *t, int argv) {
    int *x;
    if (!t || t->sv != SV_FRAME) return;
    x = (int *)t->sv_sp;
    x[3] = argv;
}

// ---- lifecycle ----

// The processors C4IX uses: libjs's SMP, which has the window, up to
// CPU_MAX; otherwise one.
int sched_ncpus() {
    int n;
    if (!host_has(C4IX_I_C4MJS) || !host_has(C4IX_I_SMP)) return 1;
    n = __c4_cpu_count();
    return n > CPU_MAX ? CPU_MAX : n;
}

// ---- off the boot stack ----
//
// The machine starts the kernel on its boot stack, the megabyte at the top
// of memory, above the heap. While the kernel task stood on it -- main,
// parked in sched_run for as long as the machine ran -- a program that
// wrote past the top of the heap landed in its frames, and nothing noticed
// until shutdown, or until something switched to it. So main's work after
// boot is the kernel task's on a stack from the kernel arena, the boot
// stack is left with only main's own frame on it, and libjs guards it:
// from then on a read or write there halts the machine, naming the pc and
// the address (libjs/c4m.js, guardHit), before it can do harm. At the end
// the guard is lifted and the kernel comes back, so main returns as it
// always did, through the image's destructors.
//
// Both hops are a trap whose handler swaps the registers with the ones
// saved on the other side, as every task switch is (sched_trap).
static int hop_sp, hop_bp, hop_pc;   // the side that is not running
static int hop_entry, hop_argc, hop_argv, hop_ret;

static void sched_hop(int interval, int trap, int param, int mode, int a, int bp, int sp, int returnpc) {
    int x;
    x = sp; sp = hop_sp; hop_sp = x;
    x = bp; bp = hop_bp; hop_bp = x;
    x = returnpc; returnpc = hop_pc; hop_pc = x;
}

// The boot stack guarded (1) or not (0), where libjs can
static void sched_bootguard(int on) {
    if (host_has(C4IX_I_C4MJS)) *(int *)C4IX_BOOT_GUARD = on;
}

// Where the first hop lands: the kernel task's stack, with nothing below.
static void sched_kmain() {
    int e;
    install_trap_handler(0);
    sched_bootguard(1);
    e = hop_entry;
    hop_ret = e(hop_argc, hop_argv);
    sched_bootguard(0);
    install_trap_handler((int)&sched_hop);
    __c4_trap(C4IX_TRAP_SOFT_IRQ, 0);
}

int sched_offboot(int entry, int argc, int argv) {
    int *stk, *x;
    if (!(stk = (int *)kmem_alloc(KMEM_STACK_BYTES))) {
        kputs("c4ix: panic: no stack for the kernel task\n");
        exit(-1);
    }
    // a return from sched_kmain, which never returns, is lost
    x = stk + KMEM_STACK_BYTES / sizeof(int) - 8;
    x[0] = (int)&task_lost;
    hop_entry = entry; hop_argc = argc; hop_argv = argv;
    hop_sp = (int)x; hop_bp = 0; hop_pc = (int)&sched_kmain;
    install_trap_handler((int)&sched_hop);
    __c4_trap(C4IX_TRAP_SOFT_IRQ, 0);
    // back from the kernel task: everything has shut down
    install_trap_handler(0);
    return hop_ret;
}

// This CPU's traps: the handler, and preemption. Every CPU's own.
static void sched_arm(int interval) {
    install_trap_handler((int)&sched_trap);
    if (interval) {
        __c4_configure(C4IX_CONF_TRAP_RESTORES_INTERVAL, 1);
        __c4_configure(C4IX_CONF_HANDLER, (int)&sched_trap);
        __c4_configure(C4IX_CONF_INTERVAL, interval);
    }
}

// Another CPU, from __c4_cpu_start: its struct cpu is its own copy of
// the window, so it says who it is there, arms its traps, and becomes
// its idle task, which the task list already has. That parks until an
// IPI says there is work, which arrives as a cycle interrupt and is a
// switch like any other. At shutdown it halts.
static void smp_ap() {
    struct task *me;
    int k, r;
    k = __c4_cpu_id();
    me = smp_idle_t[k];
    cpu_self->id = k;             // the rest of the window starts zeroed
    cpu_self->cur = me;
    cpu_self->idle = me;
    acct_out = __c4_cycles();
    sched_arm(sched_interval);
    while (!smp_stopping) {
        sched_lock();
        smp_settle();
        r = sched_ready_here();
        if (!r) smp_idle[k] = 1;
        sched_unlock();
        if (r) sched_yield();
        else {
            // parked: idle time that counts no cycles, as CPU 0's naps
            acct_park();
            __c4_wait((int)&smp_park[k], 0);
            acct_unpark();
        }
    }
    __c4_fadd((int)&smp_halted, 1);
    __c4_wake((int)&smp_halted, 0);
    __c4_cpu_halt();
}

void sched_init(int interval) {
    struct task *t;
    char name[TASK_NAME_MAX];
    int k;

    // The idle task first, so it is task 0, as NT's idle
    // is; then the context we are running in becomes the kernel task.
    // Both stand on the kernel arena (sched_offboot).
    sched_next_kstack = 1;
    idle_task = task_create("idle", (int)&sched_idle, 0, 0);
    sched_next_kstack = 0;
    if (!idle_task) {
        kputs("c4ix: panic: cannot create the idle task\n");
        exit(-1);
    }
    idle_task->cpuidle = 1;
    cpu_self->idle = idle_task;
    kernel_task = task_adopt("kernel");
    kernel_task->state = TS_RUNNING;
    idle_task->parent = kernel_task->id;
    // Processors: libjs's SMP, which has the window. Each other CPU's
    // idle task is in the list now, running, so nothing picks it, and
    // the CPU becomes it when it starts (smp_ap).
    smp_ncpu = sched_ncpus();
    kprintf("c4ix: %d processor%s\n", smp_ncpu, (smp_ncpu > 1) ? "s" : "");
    if (smp_ncpu > 1) {
        // The boot CPU's state moves into the window, where it is its
        // own: a word at a time, since memcpy is the host's and writes the
        // memory under the window, not this CPU's copy
        cpu_self = (struct cpu *)CPU_WINDOW;
        k = 0;
        while (k < sizeof(struct cpu) / sizeof(int)) {
            ((int *)cpu_self)[k] = ((int *)&cpu_boot)[k];
            ++k;
        }
        kernel_task->oncpu = 1;
        cpu_self->bkl_task = 1;
        bkl_take();               // the kernel task's: it runs, and holds the lock
        k = 1;
        while (k < smp_ncpu) {
            name[0] = 'i'; name[1] = 'd'; name[2] = 'l'; name[3] = 'e';
            name[4] = '0' + k % 10; name[5] = 0;
            if (k >= 10) { name[4] = '0' + k / 10; name[5] = '0' + k % 10; name[6] = 0; }
            if (!(t = task_adopt(name))) {
                kputs("c4ix: panic: cannot create an idle task\n");
                exit(-1);
            }
            t->cpuidle = k + 1;
            t->oncpu = k + 1;
            t->parent = kernel_task->id;
            smp_idle_t[k] = t;
            ++k;
        }
    }
    sched_nlive = 0;   // the ones that are always there are not still to finish
    sched_cur = kernel_task;
    acct_out = __c4_cycles();

    install_trap_handler((int)&sched_trap);
    // Ctrl-C arrives as a trap through the same handler.
    __c4_signal(__c4_sigint(), (int)&sched_trap);
    if (interval) {
        // Ask c4m to treat the interrupt mask as part of the
        // trapped context, restored by TLEV. That is what lets
        // sched_trap set preemption for the incoming task through
        // its `interval` parameter instead of arming the machine
        // mid-switch -- and it also tells c4m that a zero interval
        // means "masked", so signals are deferred to the next
        // unmasked instruction rather than delivered into a
        // handler that cannot accept them.
        //
        // Only meaningful with preemption: without it the interval
        // is always zero and nothing would ever be delivered.
        __c4_configure(C4IX_CONF_TRAP_RESTORES_INTERVAL, 1);
        sched_interval = interval;
        __c4_configure(C4IX_CONF_HANDLER, (int)&sched_trap);
        __c4_configure(C4IX_CONF_INTERVAL, interval);
        kprintf("c4ix: cycle interval set to %dk\n", interval / 1024);
    }

    // and the other CPUs, each on a stack of its own, in the kernel arena
    k = 1;
    while (k < smp_ncpu) {
        if (!(smp_stack[k] = (int)kmem_alloc(SMP_STACK_BYTES))) {
            kputs("c4ix: panic: cannot start the other processors\n");
            exit(-1);
        }
        __c4_cpu_start(k, (int)&smp_ap, smp_stack[k] + SMP_STACK_BYTES);
        kprintf("c4ix: CPU %d stack allocated: 0x%08x, CPU booting\n", k, smp_stack[k]);
        ++k;
    }
    sched_banner();
}

// What the machine is, said once at boot, where libjs says it: the
// memory and its layout, and the processors -- which share all of it and
// the one heap, each with nothing of its own but its window (and its idle
// task's stack, in the kernel arena). From the bottom: the kernel image,
// the kernel arena (which starts where the heap does: the va area is the
// heap's first block, taken before main), the heap programs use, whatever
// the host keeps for itself (the mailbox), the boot stack, guarded, and
// the arguments above it, which are the rest.
static void sched_banner() {
    int hb, he, bb, bt;
    if (host_has(C4IX_I_C4MJS)) {
        hb = *(int *)C4IX_HEAP_BASE;
        he = *(int *)C4IX_HEAP_END;
        bb = *(int *)C4IX_BOOT_BASE;
        bt = *(int *)C4IX_BOOT_TOP;
        kprintf("c4ix: %d MB of memory:\n", (bt + 4096) / 1048576);
        kprintf("  [mem 0x%08x-0x%08x] Kernel image (%d KB)\n", 0, hb - 1, hb / 1024);
        kprintf("  [mem 0x%08x-0x%08x] Kernel arena - tasks, files, kernel stacks (%d KB)\n",
            hb, kmem_end - 1, (kmem_end - hb) / 1024);
        kprintf("  [mem 0x%08x-0x%08x] Heap - all your applications and memory (%d MB)\n",
            kmem_end, he - 1, ((he - kmem_end) / 1024) / 1024);
        if (bb > he)
            kprintf("  [mem 0x%08x-0x%08x] Reserved by the host - its mailbox (%d KB)\n", he, bb - 1, (bb - he) / 1024);
        kprintf("  [mem 0x%08x-0x%08x] Boot stack, guarded (%d KB)\n", bb, bt - 1, (bt - bb) / 1024);
        kprintf("  [mem 0x%08x-0x%08x] Arguments (%d KB)\n", bt, bt + 4095, 4);
    }

    if (smp_ncpu > 1) {
        kprintf("  %s\n  CPUs share all memory and the heap\n  each with a %d-byte window of its own at 0x%08x\n",
            host_has(C4IX_I_PAR) ? "a thread each, all at once" : "simulated, taking turns on one thread",
            256, CPU_WINDOW);
    }
}

// Shutdown, from the kernel task: the other CPUs halt. Each takes the
// lock on its way out, so the kernel task lets go of its own meanwhile,
// masked -- an IPI could still arrive, and must not switch it away.
static void smp_stop() {
    int k, h;
    if (smp_ncpu < 2) return;
    __c4_configure(C4IX_CONF_INTERVAL, 0);
    ++sched_lockdepth;
    if (cpu_self->bkl_task) { cpu_self->bkl_task = 0; bkl_drop(); }
    smp_stopping = 1;
    k = 1;
    while (k < smp_ncpu) { __c4_ipi(k); ++k; }
    while ((h = smp_halted) < smp_ncpu - 1) __c4_wait((int)&smp_halted, h);
    bkl_take();
    cpu_self->bkl_task = 1;
    --sched_lockdepth;
    if (sched_interval) __c4_configure(C4IX_CONF_INTERVAL, sched_interval);
    k = 1;
    while (k < smp_ncpu) { kmem_free((char *)smp_stack[k]); smp_stack[k] = 0; ++k; }
}

// Disarm the trap machinery: after this the scheduler is inert and
// the task list can be torn down. Without it the cycle interrupt
// would fire after main returns, into a freed task list.
// (No CPU is on any task's stack now: the others have halted.)
void sched_stop() {
    struct task *t;
    __c4_configure(C4IX_CONF_INTERVAL, 0);
    install_trap_handler(0);
    sched_interval = 0;
    sched_cur = 0;
    t = smp_ncpu > 1 ? task_head : 0;
    while (t) { t->oncpu = 0; t = t->next; }
}

// A task parked on console input: what a keystroke would wake.
static int sched_console_reader() {
    struct task *t;
    t = task_head;
    while (t) {
        if (sched_reading_console(t)) return 1;
        t = t->next;
    }
    return 0;
}

// The idle task: what runs when nothing else can, which is to say
// almost always on an idle desktop. It returns -- and the machine shuts
// down -- once no task is left alive but the kernel, which waits on it.
static int sched_idle(int argc, int argv) {
    int nap, due, budget, ms, ready;

    nap = 0;
    while (sched_nlive > 0) {
        // Sweep zombies nobody is waiting on. This is where C4KE reaps
        // too, and it is the only thing that collects a child whose
        // parent never waits -- innerbench spawns several benches and
        // can only wait on one.
        if (sched_nzombies) task_reap_orphans();
        // Under SMP the idle task looks at the kernel under its lock, as
        // the other CPUs' idle tasks do, and says it idles (smp_kick)
        // -- and does not nap when there is work after all: the IPI that
        // said so came while it held the lock, and changed nothing then
        ready = 0;
        if (smp_ncpu > 1) {
            sched_lock();
            smp_settle();
            ready = sched_ready_here();
            smp_idle[0] = !ready;
        }
        // The idle task runs only when nothing else can: everything alive
        // is parked on something that has not arrived, and nothing can
        // make progress until the clock or the outside world moves.
        //
        // A task reading the console is waiting for a keystroke, which
        // comes with no deadline to sleep to, so then the nap backs off
        // the way c4sh does: double it up to a ceiling well inside human
        // reaction time, so the prompt still feels immediate. (It used to
        // drop back to the floor when a pass found a task ready, which
        // since the idle task runs only when none is, it never does.)
        // Nobody reading it, the next thing that can happen is a
        // sleeper's deadline, and the nap goes straight there: one pass
        // where the backoff took seven to cover a desktop's 15 ms sleep.
        if (sched_console_reader()) {
            nap = nap ? nap * 2 : IDLE_NAP_MIN;
            if (nap > IDLE_NAP_MAX) nap = IDLE_NAP_MAX;
        } else nap = IDLE_NAP_LONG;
        // A sleeping task has a deadline, so the backoff must not
        // overshoot it: a 1ms sleep should not wait out a 20ms
        // nap. Milliseconds in, microseconds out.
        if (sched_sleep_due(&due)) {
            budget = (due - __time()) * 1000;
            if (budget < 0) budget = 0;
            if (budget < nap) nap = budget;
        }
        // (an IPI ends the nap early: another CPU left work for this one)
        if (smp_ncpu > 1) sched_unlock();
        // the counter stands still meanwhile: idle time in ms instead
        ms = __time();
        if (smp_ncpu > 1) {
            if (!ready) { acct_park(); __c4_usleep(nap); acct_unpark(); }
        } else {
            __c4_usleep(nap);
            acct_napms = acct_napms + (__time() - ms);
        }
        // The cycle counter stood still while we slept, so the
        // console's own rate limit has no way to know that real
        // time passed. Tell it.
        if (smp_ncpu > 1) sched_lock();
        con_wake();
        if (smp_ncpu > 1) sched_unlock();
        sched_yield();
    }
    return 0;
}

// The kernel task's part once everything is set up: wait for the idle
// task, which ends when nothing else is alive, then reap anything nobody
// waited for.
void sched_run() {
    struct task *t;
    int live;

    task_wait(idle_task);
    smp_stop();
    // The idle task was released while it ran, so it waits on the reap
    // list for a safe point -- and with nothing left there may not be
    // another trap. Here, on the kernel's own stack, is one. (Under SMP
    // the switch away from it left its mark: this CPU is off its stack.)
    smp_settle();
    task_reap();
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
