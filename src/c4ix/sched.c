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


// Idle backoff, in microseconds: how long the boot task sleeps when
// every other task is parked waiting on the outside world.
enum { IDLE_NAP_MIN = 100, IDLE_NAP_MAX = 20000 };

struct task *sched_cur;
static struct task *boot_task;
static int sched_on_c4m;
static int sched_interval;   // preemption interval, 0 = cooperative only
static int sched_lockdepth;
static int sched_tramp;      // address of the trampoline's LEV
static int sched_intrap;     // inside the trap handler: no re-entry

int sched_switches;          // context switches since boot

// Compiles to ENT 0 / LEV; entered at its LEV (sched_tramp) the
// second LEV re-derives sp from the just-installed bp.
static int trampoline() { }

struct task *sched_current() {
    return sched_cur;
}

// ---- preemption masking (c4m only; nestable) ----
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
    if (sched_on_c4m && !sched_intrap) {
        if (sched_lockdepth == 0 && sched_interval)
            __c4_configure(C4IX_CONF_INTERVAL, 0);
    }
    ++sched_lockdepth;
}

void sched_unlock() {
    --sched_lockdepth;
    if (sched_on_c4m && !sched_intrap) {
        if (sched_lockdepth == 0 && sched_interval)
            __c4_configure(C4IX_CONF_INTERVAL, sched_interval);
    }
}

int sched_in_trap() {
    return sched_intrap;
}

// ---- picking the next task ----

// A TS_WAITING task becomes runnable once the task it waits on is a
// zombie. Completing the wait here delivers the exit code twice
// over: into wait_result for a kernel caller blocked in task_wait,
// and into sv_a for a user task parked mid-syscall -- the switch
// restores sv_a into the accumulator, which is exactly where the
// SYS_WAIT opcode's result belongs.
// A TS_BLOCKED task is parked on a vnode -- an empty pipe. It wakes
// when the data it wanted arrives, or when the last writer closes
// and the emptiness becomes end-of-file instead.
// A TS_SLEEPING task is parked on the clock. Subtract rather than
// compare the raw values -- the habit that survives a wrap.
static int sched_unblocked(struct task *n) {
    if (n->state == TS_SLEEPING) {
        if (__time() - n->ck_wake < 0) return 0;
        n->state = TS_READY;
        return 1;
    }
    if (n->state != TS_BLOCKED) return 0;
    if (!vfs_readable(n->block_vn, n->block_pos)) return 0;
    n->state = TS_READY;
    return 1;
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

static int sched_waitdone(struct task *n) {
    struct task *w;
    if (sched_unblocked(n)) return 1;
    if (n->state != TS_WAITING) return 0;
    if ((w = task_get(n->wait_for))) {
        if (w->state != TS_ZOMBIE) return 0;
        n->wait_result = w->exitcode;
        task_release(w);
    } else {
        n->wait_result = -1;   // target vanished: nothing to reap
    }
    n->sv_a = n->wait_result;
    n->state = TS_READY;
    return 1;
}

// Round-robin from t: the next runnable task, or t itself.
static struct task *sched_pick(struct task *t) {
    struct task *n;
    n = task_next(t);
    while (n != t) {
        if (n->state == TS_READY) return n;
        if (sched_waitdone(n)) return n;
        n = task_next(n);
    }
    sched_waitdone(t);   // t itself may be waiting on a finished task
    return t;
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
    struct task *t, *n;

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
    if (sched_cur) ++sched_cur->ntraps;

    // no nested switches while kernel structures move
    __c4_configure(C4IX_CONF_INTERVAL, 0);

    // Safe point: we are on the trapped task's stack, so any corpse
    // left over from an earlier exit can finally be freed.
    task_reap();
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
        returnpc = returnpc - 8;
        sys_restart = 0;
    }

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
        // Charge the outgoing task for the slice it just ran, and
        // start the incoming task's clock. This is the accounting ps
        // and top report, and it is exact: the VM counter is the
        // same one the scheduler runs on.
        t->cycles = t->cycles + (__c4_cycles() - t->cycles_in);
        n->cycles_in = __c4_cycles();
        // the mask travels with the task, not with the machine
        t->lockdepth = sched_lockdepth;
        sched_lockdepth = n->lockdepth;
        n->state = TS_RUNNING;
        sched_cur = n;
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
    // what runs the handler itself in the task's own mode.
    ck_signal_deliver(sched_cur, &a, &bp, &sp, &returnpc, mode, interval);

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

    // The session's top-level program: whatever a kernel task waits on.
    t = 0;
    n = task_head;
    while (n) {
        if (n->privs == PRIV_KERNEL && n->state == TS_WAITING) {
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

// Kernel tasks are spared (killing init or boot ends everything). The
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
    t->exitcode = C4IX_EXIT_INTERRUPTED;
    t->state = TS_ZOMBIE;
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

    old->cycles = old->cycles + (__c4_cycles() - old->cycles_in);
    next->cycles_in = __c4_cycles();
    old->lockdepth = sched_lockdepth;
    sched_lockdepth = next->lockdepth;
    next->state = TS_RUNNING;
    sched_cur = next;
    ++sched_switches;
}

// ---- the common face ----

void sched_yield() {
    struct task *n;
    if (!sched_cur) return;
    // Already inside the trap handler: it switches on the way out,
    // which IS the yield. Raising another trap here would nest.
    if (sched_intrap) return;
    if (sched_on_c4m) {
        __c4_trap(C4IX_TRAP_SOFT_IRQ, 0);
        return;
    }
    n = sched_pick(sched_cur);
    if (n != sched_cur) coop_switch(n);
    task_reap();   // safe point: whatever died, we are not on it now
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

void task_exit(int code) {
    sched_cur->exitcode = code;
    sched_cur->state = TS_ZOMBIE;
    // From inside the trap handler, marking the zombie is enough:
    // the switch on the way out picks someone else and never comes
    // back. Outside, spin on yield -- a zombie is never picked again.
    if (sched_intrap) return;
    while (1) sched_yield();
}

// Block until t exits. No spinning: the caller parks in TS_WAITING
// and sched_waitdone wakes it with the exit code once t is a zombie
// (and has reaped t by then -- t must not be touched after waking).
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
    sched_on_c4m = (host_type() == HOST_C4M);
    sched_tramp = (int)&trampoline + 16;   // past ENT 0: the LEV word

    boot_task = task_adopt("boot");
    boot_task->state = TS_RUNNING;
    sched_cur = boot_task;

    if (sched_on_c4m) {
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
    int live, ready, nap, due, budget;

    nap = 0;
    live = 1;
    while (live) {
        live = 0;
        ready = 0;
        t = task_head;
        while (t) {
            if (t != sched_cur && t->state != TS_ZOMBIE) {
                live = 1;
                if (t->state != TS_BLOCKED && t->state != TS_WAITING
                    && t->state != TS_SLEEPING) ready = 1;
            }
            t = t->next;
        }
        if (!live) break;
        // Sweep zombies nobody is waiting on. This is the idle task,
        // which is where C4KE reaps too, and it is the only thing that
        // collects a child whose parent never waits -- innerbench
        // spawns several benches and can only wait on one.
        task_reap_orphans();
        // Everything alive is parked on something that has not
        // arrived -- console input, usually. Nothing can make progress
        // until the outside world does, and yielding in a tight loop
        // would burn a core waiting for a keystroke. So back off the
        // way c4sh does: double the nap up to a ceiling, and drop
        // straight back to zero the moment there is work again. The
        // ceiling is well inside human reaction time, so the prompt
        // still feels immediate.
        if (ready) nap = 0;
        else {
            nap = nap ? nap * 2 : IDLE_NAP_MIN;
            if (nap > IDLE_NAP_MAX) nap = IDLE_NAP_MAX;
            // A sleeping task has a deadline, so the backoff must not
            // overshoot it: a 1ms sleep should not wait out a 20ms
            // nap. Milliseconds in, microseconds out.
            if (sched_sleep_due(&due)) {
                budget = (due - __time()) * 1000;
                if (budget < 0) budget = 0;
                if (budget < nap) nap = budget;
            }
            __c4_usleep(nap);
            // The cycle counter stood still while we slept, so the
            // console's own rate limit has no way to know that real
            // time passed. Tell it.
            con_wake();
        }
        sched_yield();
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
