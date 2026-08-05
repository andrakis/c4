//
// C4IX: the C4KE compatibility layer.
//
// C4KE programs are built against include/u0.h and reach their kernel
// through custom opcodes -- numbers >= 128 that no VM implements, so
// executing one raises TRAP_ILLOP. That is the same door C4IX's own
// SYS_* gateway uses, which is why this fits: sched_trap already
// normalises the arguments (the opcode at sp[0], the arguments above
// it), so all that is missing is a second range test and this file.
//
// Only ONE number is fixed. u0's constructor asks for every service
// by NAME through OP_REQUEST_SYMBOL (128) and caches whatever it is
// told, so C4IX assigns its own numbering and C4KE binaries need no
// rebuild -- which is the whole point. If a .c4r has to be recompiled
// to run here, this layer is wrong.
//
// The goal is the tools, not the kernel: ps, top, spin, bench,
// benchtop and innerbench, unmodified, as C4IX tasks.
//

#include "c4ix.h"

// ---- symbol lookup ----
//
// Comparison is memcmp, which is the MCMP opcode -- ONE instruction.
// A hand-rolled character loop would be about eight instructions per
// character, and u0 issues 27 lookups before main() runs; against 27
// candidates of ~26 characters that is over a million instructions
// per program start, for a table lookup.
//
// C4KE compares memcmp(name, "OP_X", strlen("OP_X")), which is a
// PREFIX match -- that is why its chain has to be ordered by
// ascending length, and why a future name extending an earlier one
// would silently resolve to the wrong opcode. Comparing len + 1 bytes
// includes the terminator, making the match exact; the length test in
// front of it both rejects nearly every candidate in two instructions
// and guarantees that extra byte is in bounds.
#define CK_EQ(lit, len)  (n == (len) && !memcmp(name, lit, (len) + 1))

static int ck_symbol(char *name) {
    char *p;
    int n;

    p = name;
    n = 0;
    while (*p) { ++p; ++n; }
    if (n < 7) return 0;
    if (memcmp(name, "OP_", 3)) return 0;

    if (CK_EQ("OP_HALT", 7))                     return CK_HALT;
    if (CK_EQ("OP_TIME", 7))                     return CK_TIME;
    if (CK_EQ("OP_C4INFO", 9))                   return CK_C4INFO;
    if (CK_EQ("OP_USER_PID", 11))                return CK_USER_PID;
    if (CK_EQ("OP_SCHEDULE", 11))                return CK_SCHEDULE;
    if (CK_EQ("OP_TASK_EXIT", 12))               return CK_TASK_EXIT;
    if (CK_EQ("OP_AWAIT_PID", 12))               return CK_AWAIT_PID;
    if (CK_EQ("OP_TASK_FOCUS", 13))              return CK_TASK_FOCUS;
    if (CK_EQ("OP_USER_KILL", 12))               return CK_USER_KILL;
    if (CK_EQ("OP_USER_SLEEP", 13))              return CK_USER_SLEEP;
    if (CK_EQ("OP_TASK_FINISH", 14))             return CK_TASK_FINISH;
    if (CK_EQ("OP_TASK_CYCLES", 14))             return CK_TASK_CYCLES;
    if (CK_EQ("OP_USER_PARENT", 14))             return CK_USER_PARENT;
    if (CK_EQ("OP_USER_SIGNAL", 14))             return CK_USER_SIGNAL;
    if (CK_EQ("OP_AWAIT_MESSAGE", 16))           return CK_AWAIT_MESSAGE;
    if (CK_EQ("OP_REQUEST_SYMBOL", 17))          return CK_REQUEST_SYMBOL;
    if (CK_EQ("OP_USER_START_C4R", 17))          return CK_USER_START_C4R;
    if (CK_EQ("OP_KERN_TASK_COUNT", 18))         return CK_KERN_TASK_COUNT;
    if (CK_EQ("OP_KERN_TASK_RUNNING", 20))       return CK_KERN_TASK_RUNNING;
    if (CK_EQ("OP_KERN_TASKS_RUNNING", 21))      return CK_KERN_TASKS_RUNNING;
    if (CK_EQ("OP_KERN_TASKS_EXPORT", 20))       return CK_KERN_TASKS_EXPORT;
    if (CK_EQ("OP_DEBUG_KERNELSTATE", 20))       return CK_DEBUG_KERNELSTATE;
    if (CK_EQ("OP_KERN_TASK_CURRENT_ID", 23))    return CK_KERN_TASK_CURRENT_ID;
    if (CK_EQ("OP_KERN_TASKS_EXPORT_FREE", 25))  return CK_KERN_TASKS_EXPORT_FREE;
    if (CK_EQ("OP_KERN_REQUEST_EXCLUSIVE", 25))  return CK_KERN_REQUEST_EXCLUSIVE;
    if (CK_EQ("OP_KERN_RELEASE_EXCLUSIVE", 25))  return CK_KERN_RELEASE_EXCLUSIVE;
    if (CK_EQ("OP_CURRENTTASK_UPDATE_NAME", 26)) return CK_CURRENTTASK_UPDATE_NAME;
    if (CK_EQ("OP_KERN_TASKS_EXPORT_UPDATE", 27)) return CK_KERN_TASKS_EXPORT_UPDATE;

    // Loud, because the failure is otherwise invisible: u0 stores the
    // 0, and executing opcode 0 later raises TRAP_OPV somewhere else
    // entirely.
    kprintf("c4ix: c4ke: task %d asked for unknown symbol '%s'\n",
        sched_current() ? sched_current()->id : -1, name);
    return 0;
}

// ---- odds and ends ----

static void ck_namecpy(struct task *t, char *src) {
    int i;
    i = 0;
    while (i < TASK_NAME_MAX - 1 && src[i]) { t->name[i] = src[i]; ++i; }
    t->name[i] = 0;
}

// A C4KE task holds the exclusive lock so its output cannot be
// interleaved (ps brackets its whole listing in one). C4IX's
// sched_lock is exactly that, and the depth already travels with the
// task across context switches. The flag makes the pair idempotent:
// a program calling request twice, or release without request, must
// not be able to wedge sched_lockdepth permanently.
static int ck_exclusive(struct task *t, int on) {
    if (!t) return 0;
    if (on) {
        if (!t->ck_excl) { t->ck_excl = 1; sched_lock(); }
    } else {
        if (t->ck_excl) { t->ck_excl = 0; sched_unlock(); }
    }
    return 0;
}

static int ck_running(int id) {
    struct task *t;
    if (!(t = task_get(id))) return 0;
    return t->state != TS_ZOMBIE;
}

static int ck_tasks_running() {
    struct task *t;
    int n;
    n = 0;
    t = task_head;
    while (t) {
        if (t->state == TS_READY || t->state == TS_RUNNING) ++n;
        t = t->next;
    }
    return n;
}

static void ck_kernelstate() {
    struct task *t;
    int ready, waiting, zombie;

    ready = 0; waiting = 0; zombie = 0;
    t = task_head;
    while (t) {
        if (t->state == TS_ZOMBIE) ++zombie;
        else if (t->state == TS_READY || t->state == TS_RUNNING) ++ready;
        else ++waiting;
        t = t->next;
    }
    kprintf("c4ix: %d tasks: %d ready, %d waiting, %d zombie; %d switches\n",
        task_count(), ready, waiting, zombie, sched_switches);
}

// ---- the task table, as C4KE's ps and top expect to read it ----
//
// One kernel-allocated block: a three-word header, then CK_TASK_SLOTS
// fixed-size records that userland indexes directly. This is a hard
// ABI, not a copy-out API -- ps reads it with the offsets from u0.h
// and mutates its own copy in place -- so the layout in c4ix.h has to
// match u0.h exactly.

// Cycles per millisecond, measured once. C4IX has no per-task
// millisecond accounting and adding one would mean calling __time()
// at every context switch; the TIMEMS column is a display, so it is
// DERIVED from the cycle count that is already exact. Calibrating
// costs one busy wait at the first ps.
static int ck_cpms;

static void ck_calibrate() {
    int t0, c0, ms;

    if (ck_cpms) return;
    // Wait for a tick boundary first, so the window is a whole
    // number of milliseconds rather than a partial one.
    t0 = __time();
    while (__time() == t0) ;
    t0 = __time();
    c0 = __c4_cycles();
    while ((ms = __time() - t0) < 100) ;
    ck_cpms = (__c4_cycles() - c0) / ms;
    if (ck_cpms < 1) ck_cpms = 1;
}

static char *ck_strdup(char *s) {
    char *d;
    int n, i;
    n = 0;
    while (s[n]) ++n;
    if (!(d = (char *)malloc(n + 1))) return 0;
    i = 0;
    while (i <= n) { d[i] = s[i]; ++i; }
    return d;
}

// C4IX's TS_* are small integers; C4KE's STATE_* are bit flags that
// ps tests with &. A zero state means "empty slot", so a live task
// must never map to one.
static int ck_state(struct task *t) {
    if (t->state == TS_ZOMBIE) return CK_STATE_ZOMBIE;
    if (t->state == TS_READY || t->state == TS_RUNNING)
        return CK_STATE_LOADED | CK_STATE_RUNNING;
    return CK_STATE_LOADED | CK_STATE_WAITING;
}

static int ck_wstate(struct task *t) {
    if (t->state == TS_SLEEPING) return CK_WSTATE_TIME;
    if (t->state == TS_WAITING) return CK_WSTATE_PID;
    // ps prints 'S' for this one: parked inside a syscall, which is
    // exactly what a blocked read is.
    if (t->state == TS_BLOCKED) return CK_WSTATE_SYSCALL;
    return CK_WSTATE_NONE;
}

// C4KE numbers privilege NONE=0, USER=1, KERNEL=2; C4IX numbers it
// KERNEL=0, USER=1. They agree on 1 and on nothing else. ps indexes
// prio_table = "-UK" with this, so getting it wrong prints '-' for
// every kernel task -- a wrong answer that looks plausible.
static int ck_privs_out(int p) {
    return (p == PRIV_KERNEL) ? CK_PRIV_KERNEL : CK_PRIV_USER;
}

static void ck_export_fill(int *kti) {
    int *kte;
    struct task *t;
    int n, cyc;

    ck_calibrate();
    kte = kti + CK_KTI__Sz;
    n = 0;
    t = task_head;
    while (t && n < CK_TASK_SLOTS) {
        // The name is a COPY. One address space means a pointer into
        // the task would work -- right up to the refresh where that
        // task has been reaped and ps prints from freed slab memory.
        if (kte[CK_KTE_NAME]) free((char *)kte[CK_KTE_NAME]);
        kte[CK_KTE_NAME] = (int)ck_strdup(t->name);
        kte[CK_KTE_NAMELEN] = 0;
        while (t->name[kte[CK_KTE_NAMELEN]]) ++kte[CK_KTE_NAMELEN];

        cyc = t->cycles + ((t == sched_current()) ? (__c4_cycles() - t->cycles_in) : 0);

        kte[CK_KTE_STATE] = ck_state(t);
        kte[CK_KTE_WAITSTATE] = ck_wstate(t);
        kte[CK_KTE_ID] = t->id;
        kte[CK_KTE_PARENT] = t->parent;
        kte[CK_KTE_PRIORITY] = 0;
        kte[CK_KTE_PRIVS] = ck_privs_out(t->privs);
        kte[CK_KTE_NICE] = 0;
        kte[CK_KTE_CYCLES] = cyc;
        kte[CK_KTE_TIMEMS] = cyc / ck_cpms;
        kte[CK_KTE_TRAPS] = t->ntraps;
        // BYTES USED, not the base address: the stack grows down from
        // the top of the allocation, so used = top - sp. sv_sp is only
        // current as of this task's last context switch, which is the
        // same accuracy C4KE reports. The boot task runs on the VM's
        // own stack and has no allocation to measure.
        kte[CK_KTE_STACK] = t->stack
            ? (t->stack + C4IX_STACK_WORDS * 8) - t->sv_sp : 0;
        kte[CK_KTE_ALLOC] = 0;   // C4IX does not track per-task allocation

        kte = kte + CK_KTE__Sz;
        ++n;
        t = t->next;
    }
    // Clear any records left over from a larger listing, or ps would
    // report tasks that have gone.
    while (n < CK_TASK_SLOTS) {
        if (kte[CK_KTE_NAME]) { free((char *)kte[CK_KTE_NAME]); kte[CK_KTE_NAME] = 0; }
        kte[CK_KTE_STATE] = CK_STATE_UNLOADED;
        kte = kte + CK_KTE__Sz;
        ++n;
    }
    kti[CK_KTI_USED] = task_count() < CK_TASK_SLOTS ? task_count() : CK_TASK_SLOTS;
}

static int ck_export() {
    int *kti;
    int words;

    words = CK_KTI__Sz + CK_TASK_SLOTS * CK_KTE__Sz;
    if (!(kti = (int *)malloc(words * 8))) return 0;
    memset(kti, 0, words * 8);
    kti[CK_KTI_COUNT] = CK_TASK_SLOTS;
    kti[CK_KTI_LIST] = (int)(kti + CK_KTI__Sz);
    ck_export_fill(kti);
    return (int)kti;
}

static void ck_export_free(int *kti) {
    int *kte;
    int n;

    if (!kti) return;
    kte = kti + CK_KTI__Sz;
    n = 0;
    while (n < CK_TASK_SLOTS) {
        if (kte[CK_KTE_NAME]) free((char *)kte[CK_KTE_NAME]);
        kte = kte + CK_KTE__Sz;
        ++n;
    }
    free(kti);
}

// ---- spawning a C4KE program ----
//
// kern_user_start_c4r(argc, argv, name, privileges), so args[0]=argc,
// args[1]=argv, args[2]=name, args[3]=privileges.

// C4KE numbers privilege the other way round (see ck_privs_out), and
// demotes a request it will not grant rather than refusing it.
static int ck_privs_in(int p, struct task *caller) {
    if (p == CK_PRIV_KERNEL && caller && caller->privs == PRIV_KERNEL)
        return PRIV_KERNEL;
    return PRIV_USER;
}

// C4IX does not copy argv -- one address space, and the shell keeps
// its own copies alive. C4KE's callers assume the kernel copies, and
// they are right to: benchtop rewrites its OWN argv array between
// spawns, and innerbench frees its argv while the spawned tasks are
// still running. So copy, and let the task own it.
static int ck_argv_copy(struct task *t, int argc, char **argv) {
    char **vec;
    char *blob, *p;
    int i, n, bytes;

    t->argv_vec = 0;
    t->argv_data = 0;
    if (argc <= 0 || !argv) return 1;

    bytes = 0;
    i = 0;
    while (i < argc) {
        if (argv[i]) { n = 0; while (argv[i][n]) ++n; bytes = bytes + n + 1; }
        else bytes = bytes + 1;
        ++i;
    }
    if (!(vec = (char **)malloc((argc + 1) * 8))) return 0;
    if (!(blob = (char *)malloc(bytes))) { free((int *)vec); return 0; }

    p = blob;
    i = 0;
    while (i < argc) {
        vec[i] = p;
        if (argv[i]) { n = 0; while (argv[i][n]) { *p = argv[i][n]; ++p; ++n; } }
        *p = 0; ++p;
        ++i;
    }
    vec[argc] = 0;
    t->argv_vec = (int)vec;
    t->argv_data = (int)blob;
    return 1;
}

// C4KE programs are named plainly -- innerbench spawns "bench",
// benchtop spawns "top" -- so a bare name has to be found. NOT the
// shell's order, which tries c4ix-NAME.c4r first: that would silently
// hand back C4IX's own top for C4KE's, two different programs.
static struct task *ck_spawn(char *name, int argc, int argv, int privs) {
    char path[PATH_MAX];
    struct task *t;
    int n, i;

    n = 0;
    while (name[n] && n < PATH_MAX - 24) { path[n] = name[n]; ++n; }
    path[n] = 0;
    if ((t = task_spawn_priv(path, argc, argv, privs))) return t;

    // NAME.c4r
    i = n;
    path[i] = '.'; path[i+1] = 'c'; path[i+2] = '4'; path[i+3] = 'r'; path[i+4] = 0;
    if ((t = task_spawn_priv(path, argc, argv, privs))) return t;

    return 0;
}

static int ck_start_c4r(struct task *caller, int *args) {
    struct task *t;
    char *name;
    int privs;

    if (!(name = (char *)args[2])) return 0;
    privs = ck_privs_in(args[3], caller);
    if (!(t = ck_spawn(name, args[0], args[1], privs))) {
        kprintf("c4ix: c4ke: cannot start '%s'\n", name);
        return 0;
    }
    // Replace the borrowed argv with the task's own copy, now that
    // there is a task to own it. The forged frame already holds the
    // caller's pointer, so patch it where the shim will read it.
    if (args[0] > 0 && args[1]) {
        if (ck_argv_copy(t, args[0], (char **)args[1]))
            sched_forge_argv(t, t->argv_vec);
    }
    return t->id;
}

// ---- signals ----
//
// Per task, lazily: CK_SIG_MAX triples of {pending, blocked, handler},
// the same shape C4KE uses so the semantics are directly comparable.
// Allocated on the first signal() call, which for a u0 program is
// eight calls into its constructor.
//
// Delivery is the interesting half, and it happens in
// ck_signal_deliver below.

static int ck_tlev_word;   // a TLEV instruction, addressed as data

static int *ck_sigtab(struct task *t, int make) {
    int *s;
    if (t->ck_sigh) return (int *)t->ck_sigh;
    if (!make) return 0;
    if (!(s = (int *)malloc(CK_SIG_MAX * 3 * 8))) return 0;
    memset(s, 0, CK_SIG_MAX * 3 * 8);
    t->ck_sigh = (int)s;
    return s;
}

static int ck_signal(struct task *t, int sig, int handler) {
    int *s;
    int old;
    if (!t || sig < 1 || sig >= CK_SIG_MAX) return 0;
    if (!(s = ck_sigtab(t, 1))) return 0;
    old = s[sig * 3 + 2];
    s[sig * 3 + 2] = handler;
    return old;
}

int ck_has_handler(struct task *t, int sig) {
    int *s;
    if (!t || sig < 1 || sig >= CK_SIG_MAX) return 0;
    if (!(s = ck_sigtab(t, 0))) return 0;
    return s[sig * 3 + 2] != 0;
}

// The default action when nobody installed a handler.
//
// IGNORE is the important half and it is not the obvious one: top
// does kill(parent(), SIGUSR1) unconditionally to say it started, and
// innerbench -- its parent -- installs no SIGUSR1 handler. A POSIX
// "terminate" default would kill innerbench the moment top came up.
// The three that do terminate are the three a person means by them.
static int ck_default_kills(int sig) {
    return sig == CK_SIGKILL || sig == CK_SIGTERM || sig == CK_SIGINT;
}

int ck_kill(int pid, int sig) {
    struct task *t;
    int *s;

    if (sig < 1 || sig >= CK_SIG_MAX) return -1;
    if (!(t = task_get(pid))) return -1;
    if (t->state == TS_ZOMBIE) return -1;

    s = ck_sigtab(t, 0);
    if (!s || !s[sig * 3 + 2]) {
        if (!ck_default_kills(sig)) return 0;
        t->exitcode = C4IX_EXIT_INTERRUPTED;
        t->state = TS_ZOMBIE;
        return 0;
    }

    ++s[sig * 3];
    ++t->ck_sigpend;
    // A signal wakes a parked task -- otherwise a handler installed to
    // shut a sleeping program down would never get to run, which is
    // exactly what innerbench's graceful shutdown depends on.
    if (t->state == TS_SLEEPING || t->state == TS_WAITING
        || t->state == TS_BLOCKED) t->state = TS_READY;
    return 0;
}

// Deliver one pending signal to the task that is about to resume, by
// building it a trap frame by hand -- the same frame c4m's trap()
// builds, so the handler returns through an ordinary TLEV and lands
// exactly where the task was.
//
// MODE and INTERVAL are the context the task would have resumed with,
// computed by the caller. They go INTO the frame, so the handler's
// return restores them; the caller's own frame slots keep them too,
// which is what runs the handler itself in the right mode.
//
// Two slots are easy to leave out and neither failure is local:
//
//   bp+6 mode      -- TLEV loads it into the mode register. C4KE left
//                     this uninitialised, and the resulting garbage
//                     made the next PRTF look like a protected-mode
//                     violation. That was its shutdown segfault.
//   bp+9 interval  -- C4IX opted into CONF_TRAP_RESTORES_INTERVAL, so
//                     TLEV restores the preemption interval from here.
//                     Omit it and the handler's return loads garbage:
//                     preemption stops, or the cycle interrupt fires
//                     continuously.
void ck_signal_deliver(struct task *t, int *pa, int *pbp, int *psp,
                       int *ppc, int mode, int interval) {
    int *sp, *s, *h;
    int sig;

    if (!t || !t->ck_sigpend || t->state == TS_ZOMBIE) return;
    if (!(s = ck_sigtab(t, 0))) { t->ck_sigpend = 0; return; }

    sig = 1;
    while (sig < CK_SIG_MAX) {
        if (s[sig * 3] && s[sig * 3 + 2]) break;
        ++sig;
    }
    if (sig == CK_SIG_MAX) { t->ck_sigpend = 0; return; }
    --s[sig * 3];
    --t->ck_sigpend;
    h = (int *)s[sig * 3 + 2];

    if (!ck_tlev_word) ck_tlev_word = __opcode("TLEV");

    sp = (int *)*psp;
    sp = sp - 15;              // c4m's TRAP_OFFSET, so the frame has slack

    *--sp = interval;          // bp+9
    *--sp = C4IX_TRAP_SIGNAL;  // bp+8
    *--sp = sig;               // bp+7
    *--sp = mode;              // bp+6
    *--sp = *pa;               // bp+5
    *--sp = *pbp;              // bp+4
    *--sp = *psp;              // bp+3  the ORIGINAL sp, not the offset one
    *--sp = *ppc;              // bp+2
    *--sp = (int)&ck_tlev_word; // bp+1  where the handler's LEV lands
    --sp;                      // bp+0
    *pbp = (int)sp;
    *sp = (int)sp;             // self-referential, exactly as c4m does

    // Room for the handler's own locals, read from the ENT it skips.
    sp = sp - h[1];
    *psp = (int)sp;
    *ppc = (int)(h + 2);
}

void ck_task_free(struct task *t) {
    if (t->ck_sigh) { free((int *)t->ck_sigh); t->ck_sigh = 0; }
    if (t->argv_vec) { free((int *)t->argv_vec); t->argv_vec = 0; }
    if (t->argv_data) { free((char *)t->argv_data); t->argv_data = 0; }
}

// ---- the dispatcher ----
//
// args[0] is the first argument, as sched_trap normalises it. The
// return value becomes the trapped accumulator, which is the value of
// the __c4_opcode(...) expression in the calling program.
//
// Anything that "does not return" -- exit, and later sleep -- is a
// STATE CHANGE here, not a jump. sched_trap ends in a scheduling
// decision that acts on it, so none of C4KE's __c4_jmp gymnastics are
// needed.
int ck_dispatch(int num, int *args) {
    struct task *t;

    t = sched_current();
    if (t) ++t->nsyscalls;

    if (num == CK_REQUEST_SYMBOL) return ck_symbol((char *)args[0]);

    // identity and accounting
    if (num == CK_USER_PID)            return t ? t->id : -1;
    if (num == CK_USER_PARENT)         return t ? t->parent : -1;
    if (num == CK_KERN_TASK_CURRENT_ID) return t ? t->id : -1;
    if (num == CK_TASK_CYCLES)
        return t ? t->cycles + (__c4_cycles() - t->cycles_in) : 0;
    if (num == CK_C4INFO)              return host_info();
    if (num == CK_TIME)                return __time();

    // the task table, as far as this stage goes
    if (num == CK_KERN_TASK_COUNT)     return task_count();
    if (num == CK_KERN_TASK_RUNNING)   return ck_running(args[0]);
    if (num == CK_KERN_TASKS_RUNNING)  return ck_tasks_running();

    // scheduling. The switch at the end of sched_trap IS the yield,
    // so there is nothing to do but say a switch is possible.
    if (num == CK_SCHEDULE)            return ck_tasks_running() > 1;

    if (num == CK_TASK_EXIT)           { task_exit(args[0]); return 0; }
    // C4KE plants this on a task's stack so returning from main traps
    // here; C4IX uses task_shim instead, so it should never execute.
    // It still has to resolve, and behaving like exit is the honest
    // thing for it to do if it ever does.
    if (num == CK_TASK_FINISH)         { task_exit(0); return 0; }

    if (num == CK_CURRENTTASK_UPDATE_NAME) {
        if (t) ck_namecpy(t, (char *)args[0]);
        return 0;
    }

    if (num == CK_KERN_REQUEST_EXCLUSIVE) return ck_exclusive(t, 1);
    if (num == CK_KERN_RELEASE_EXCLUSIVE) return ck_exclusive(t, 0);

    if (num == CK_DEBUG_KERNELSTATE)   { ck_kernelstate(); return 0; }

    // C4IX derives the foreground job from the wait chain rather than
    // being told, so this is accepted and ignored. See sched.c.
    if (num == CK_TASK_FOCUS)          return 0;

    // C4KE's own halt does nothing either.
    if (num == CK_HALT) {
        kprintf("c4ix: c4ke: task %d requested halt (ignored)\n", t ? t->id : -1);
        return 0;
    }

    // Sleep is a STATE CHANGE, not a spin: park on the clock and let
    // the scheduling decision at the end of sched_trap pick someone
    // else. No sys_restart -- unlike a blocking read, a sleep
    // COMPLETES when it wakes, so the opcode must not re-execute.
    // The 0 returned here is saved into sv_a and is what the task
    // sees when it resumes.
    //
    // __time() is called directly and deliberately not cached: c4m
    // picks the right clock for its host (a plain system call when
    // native, the /proc/uptime reader only under plain c4) and
    // degrades cleanly through any nesting depth.
    if (num == CK_USER_SLEEP || num == CK_AWAIT_MESSAGE) {
        if (!t) return 0;
        if (args[0] <= 0) return 0;
        t->ck_wake = __time() + args[0];
        t->state = TS_SLEEPING;
        return 0;
    }

    if (num == CK_USER_SIGNAL)         return ck_signal(t, args[0], args[1]);
    if (num == CK_USER_KILL)           return ck_kill(args[0], args[1]);

    if (num == CK_USER_START_C4R)      return ck_start_c4r(t, args);

    // Byte for byte the SYS_WAIT in-trap path: park and let the
    // scheduler complete the wait, writing the exit code into sv_a --
    // which is the accumulator this opcode returns in.
    if (num == CK_AWAIT_PID) {
        struct task *s;
        int code;
        if (!t) return -1;
        if (!(s = task_get(args[0]))) {
            // Swept by the idle reaper before we asked; its status
            // outlived it.
            if (task_ghost(args[0], &code)) return code;
            return -1;
        }
        if (s->state == TS_ZOMBIE) {
            code = s->exitcode;
            task_release(s);
            return code;
        }
        t->wait_for = s->id;
        t->state = TS_WAITING;
        return 0;
    }
    if (num == CK_KERN_TASKS_EXPORT)   return ck_export();
    if (num == CK_KERN_TASKS_EXPORT_UPDATE) {
        if (args[0]) ck_export_fill((int *)args[0]);
        return 0;
    }
    if (num == CK_KERN_TASKS_EXPORT_FREE) {
        ck_export_free((int *)args[0]);
        return 0;
    }

    kprintf("c4ix: c4ke: task %d used unimplemented service %d\n",
        t ? t->id : -1, num);
    return -1;
}
