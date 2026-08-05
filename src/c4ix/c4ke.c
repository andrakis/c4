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

    // ---- not yet implemented; each is a later stage ----
    if (num == CK_USER_SIGNAL)         return 0;   // stage 5
    if (num == CK_USER_KILL)           return -1;  // stage 5
    if (num == CK_AWAIT_PID)           return -1;  // stage 6
    if (num == CK_USER_START_C4R)      return 0;   // stage 6
    if (num == CK_KERN_TASKS_EXPORT)   return 0;   // stage 4
    if (num == CK_KERN_TASKS_EXPORT_UPDATE) return 0;
    if (num == CK_KERN_TASKS_EXPORT_FREE)   return 0;

    kprintf("c4ix: c4ke: task %d used unimplemented service %d\n",
        t ? t->id : -1, num);
    return -1;
}
