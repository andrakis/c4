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

struct task *sched_cur;
// The context the machine booted in, which waits for the end once the
// scheduler runs (sched_run), and the task that runs when nothing else
// can (sched_idle). The idle task is never in the round-robin: sched_pick
// gives it the machine only when no other task can have it, so a busy
// task's syscalls do not each pass through it on the way back.
static struct task *kernel_task;
static struct task *idle_task;
static int sched_idle(int argc, int argv);
static int sched_interval;   // preemption interval, 0 = none
static int sched_lockdepth;

int sched_intrap;            // inside the trap handler: no re-entry (sched_in_trap)

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
// so Task Manager can weigh the two together.
//
// The differences are words, and the VM's counter wraps -- a difference
// is right modulo the word as long as it is less than half of it, and a
// trap at least every preemption slice sees to that.
static int acct_in;          // __c4_cycles() as this trap began
static int acct_out;         // ... as the last one ended
static int acct_napms;
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

long long task_cycles(struct task *t) {
    acct_fold(t);
    return t->ucycles + t->kcycles;
}

// A task leaving the table takes its cycles to the reaped totals, so the
// sum of everything ever run never goes backwards.
void sched_retire(struct task *t) {
    if (t->state == TS_ZOMBIE) --sched_nzombies;
    else if (t != kernel_task && t != idle_task) --sched_nlive;
    if (t == idle_task) idle_task = 0;   // shutdown: sched_pick must not fall back on it
    acct_fold(t);
    acct_gone_user = acct_gone_user + t->ucycles;
    acct_gone_kernel = acct_gone_kernel + t->kcycles;
}

// The snapshot behind Task Manager and top: one trap, so nothing runs
// between the first record and the last.
int sched_cpustat(struct cpustat *h, struct cpustat_task *r, int max) {
    struct task *t;
    struct cpustat_task *p;
    int n, i;

    n = 0;
    t = task_head;
    while (t && n < max) {
        acct_fold(t);
        p = &r[n];
        p->id = t->id;
        p->parent = t->parent;
        p->state = t->state;
        p->privs = t->privs;
        p->nsyscalls = t->nsyscalls;
        p->user = t->ucycles;
        p->kernel = t->kcycles;
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
    return n;
}

int sched_switches;          // context switches since boot
static int sched_slice0;     // acct_in when the running task's slice began
static int sched_resched;    // a service in this trap asked for a switch
static int sched_dying;      // task_exit outside the handler: die in the next trap
static int sched_dying_code;

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
void sched_lock() {
    if (!sched_intrap && sched_lockdepth == 0 && sched_interval)
        __c4_configure(C4IX_CONF_INTERVAL, 0);
    ++sched_lockdepth;
}

void sched_unlock() {
    --sched_lockdepth;
    if (!sched_intrap && sched_lockdepth == 0 && sched_interval)
        __c4_configure(C4IX_CONF_INTERVAL, sched_interval);
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
        return 1;
    }
    if (n->state != TS_BLOCKED) return 0;
    if (!vfs_readable(n->block_vn, n->block_pos)) return 0;
    n->state = TS_READY;
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
void task_zombify(struct task *t, int code) {
    struct task *w;
    if (t->state == TS_ZOMBIE) return;
    t->exitcode = code;
    t->state = TS_ZOMBIE;
    ++sched_nzombies;
    if (t != kernel_task && t != idle_task) --sched_nlive;
    w = task_head;
    while (w) {
        if (w->state == TS_WAITING && w->wait_for == t->id) {
            // into wait_result for a kernel caller in task_wait, and into
            // sv_a for a user task parked mid-syscall: the switch restores
            // sv_a into the accumulator, where SYS_WAIT's result belongs
            w->wait_result = code;
            w->sv_a = code;
            w->state = TS_READY;
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
static struct task *sched_pick(struct task *t) {
    struct task *n, *start;
    start = (t->state == TS_ZOMBIE) ? task_head : task_next(t);
    n = start;
    while (n) {
        if (n != t && n != idle_task) {
            if (n->state == TS_READY) return n;
            if (sched_unblocked(n)) return n;
        }
        n = task_next(n);
        if (n == start) n = 0;
    }
    if (t->state == TS_RUNNING || t->state == TS_READY || sched_unblocked(t)) return t;
    return idle_task ? idle_task : t;
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
static void sched_trap(int interval, int trap, int param, int mode, int a, int bp, int sp, int returnpc) {
    struct task *t, *n, *me;

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
    if ((me = sched_cur)) {
        ++me->ntraps;
        me->uacc = me->uacc + (acct_in - acct_out);
    }

    // no nested switches while kernel structures move
    __c4_configure(C4IX_CONF_INTERVAL, 0);

    // Safe point: we are on the trapped task's stack, so any corpse
    // left over from an earlier exit can finally be freed. Usually
    // there is none, and then not even the call.
    if (task_reaplist) task_reap();
    // The stack guard. A task whose stack pointer is inside the guard,
    // or whose top guard word has been written, ran off the end of its
    // stack: kill it here, before what it wrote under the stack -- some
    // other allocation -- takes the machine down in some other task.
    if (sched_cur && sched_cur->stack && sched_cur->privs == PRIV_USER &&
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
            a = sys_dispatch(param, (int *)sp + 1);
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
    if ((trap == C4IX_TRAP_ILLOP || trap == C4IX_TRAP_PM) && !sched_resched
        && t->state == TS_RUNNING
        && (!sched_interval || acct_in - sched_slice0 < sched_interval))
        n = t;
    else {
        n = sched_pick(t);
        sched_slice0 = acct_in;
    }
    sched_resched = 0;
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
            sp = n->sv_sp + 2 * sizeof(int);
            returnpc = n->sv_pc;
        }
        // the mask travels with the task, not with the machine
        t->lockdepth = sched_lockdepth;
        sched_lockdepth = n->lockdepth;
        n->state = TS_RUNNING;
        sched_cur = n;
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
        if ((me->uacc | me->kacc) & ACCT_FOLD_AT) acct_fold(me);
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
    while (t->state == TS_WAITING && guard < 32) {
        if (!(n = task_get(t->wait_for))) break;
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
    while (t->state == TS_WAITING && guard < 32) {
        if (!(n = task_get(t->wait_for))) break;
        t = n;
        ++guard;
    }
    if (t == root || t->state == TS_ZOMBIE || t->privs != PRIV_USER) return 0;
    if (ck_has_handler(t, CK_SIGINT)) { ck_kill(t->id, CK_SIGINT); return 1; }
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
    task_zombify(t, C4IX_EXIT_INTERRUPTED);
}

// ---- the common face ----

void sched_yield() {
    if (!sched_cur) return;
    // Already inside the trap handler: it switches on the way out,
    // which IS the yield -- once it knows not to take the fast path.
    // Raising another trap here would nest.
    if (sched_intrap) { sched_resched = 1; return; }
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

int sched_forge(struct task *t, int entry, int argc, int argv) {
    int *stk, *x;
    int i, words;

    words = sched_next_stack_words > C4IX_STACK_WORDS ? sched_next_stack_words : C4IX_STACK_WORDS;
    if (!(stk = (int *)malloc(words * sizeof(int)))) return 0;
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

void sched_init(int interval) {
    // The idle task first, so it is task 0, as NT's System Idle Process
    // is; then the context we are running in becomes the kernel task.
    if (!(idle_task = task_create("idle", (int)&sched_idle, 0, 0))) {
        kputs("c4ix: panic: cannot create the idle task\n");
        exit(-1);
    }
    kernel_task = task_adopt("kernel");
    kernel_task->state = TS_RUNNING;
    idle_task->parent = kernel_task->id;
    sched_nlive = 0;   // the two that are always there are not still to finish
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
    }
}

// Disarm the trap machinery: after this the scheduler is inert and
// the task list can be torn down. Without it the cycle interrupt
// would fire after main returns, into a freed task list.
void sched_stop() {
    __c4_configure(C4IX_CONF_INTERVAL, 0);
    install_trap_handler(0);
    sched_interval = 0;
    sched_cur = 0;
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
    int nap, due, budget, ms;

    nap = 0;
    while (sched_nlive > 0) {
        // Sweep zombies nobody is waiting on. This is where C4KE reaps
        // too, and it is the only thing that collects a child whose
        // parent never waits -- innerbench spawns several benches and
        // can only wait on one.
        if (sched_nzombies) task_reap_orphans();
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
        // the counter stands still meanwhile: idle time in ms instead
        ms = __time();
        __c4_usleep(nap);
        acct_napms = acct_napms + (__time() - ms);
        // The cycle counter stood still while we slept, so the
        // console's own rate limit has no way to know that real
        // time passed. Tell it.
        con_wake();
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
    // The idle task was released while it ran, so it waits on the reap
    // list for a safe point -- and with nothing left there may not be
    // another trap. Here, on the kernel's own stack, is one.
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
