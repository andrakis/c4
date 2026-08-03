//
// C4IX: kernel-wide declarations.
//
// Every module includes this and nothing else. The preprocessor
// (gcc -E, the Makefile's PREPROC) runs before c4lc, so plain
// macros and quoted includes are available even though c4lc itself
// has no preprocessor.
//

#ifndef __C4IX_H
#define __C4IX_H 1

// ---- host detection (host.c) ----
// __c4_info() bit values, mirroring c4m.c's C4I_* enum. Plain c4's
// INFO opcode returns 0, which is the whole degraded-mode signal.
enum {
    C4IX_I_C4   = 0x01,  // ultimately running under plain c4
    C4IX_I_C4M  = 0x02,  // running under c4m
    C4IX_I_HRT  = 0x10,  // high resolution timer
    C4IX_I_SIG  = 0x20,  // signals supported
    C4IX_I_FLT  = 0x40,  // floating point supported
    C4IX_I_PROT = 0x80   // protected mode available
};
enum { HOST_C4 = 0, HOST_C4M = 1 };

// c4m trap machinery numbers (c4m.c enums), used by sched.c on the
// c4m host only.
enum { C4IX_CONF_INTERVAL = 0, C4IX_CONF_HANDLER = 1 };
enum { C4IX_TRAP_ILLOP = 0, C4IX_TRAP_HARD_IRQ = 1, C4IX_TRAP_SOFT_IRQ = 2 };

int   host_detect();
int   host_info();
int   host_type();
int   host_has(int bit);
char *host_name();

// ---- console (con.c) ----
// All kernel output goes through here; the only opcode used is PUTC,
// which every host implements. kprintf lines are atomic: the body
// runs inside sched_lock, so preemption never interleaves them.
int kputc(int c);
int kputs(char *s);
int kprintf(char *fmt, ...);

// ---- varargs (va.c) ----
// The stock stdarg.h keeps its va area in per-unit statics. Statics
// never merge across objects, so a cross-module variadic call would
// push on the caller's counter and pop the callee's. C4IX therefore
// owns ONE va area, in va.c, reached through extern functions.
#define va_list int *
#define va_arg(AP, TYPE)   (AP = AP + 1, *((TYPE *) (AP - 1)))
#define va_start(AP, LAST) (AP = *(&LAST - 1), va_arg(AP, int))
#define va_end(AP)         (AP = AP - 1, __c4ix_va_adj(1 + va_arg(AP, int)))
int *__c4cc_make_va(int count);  // compiler-inserted at variadic call sites
void __c4ix_va_adj(int n);

// ---- SL4B (sl4b.c) ----
// Slab allocator for fixed-size kernel objects: caches hand out
// objects from malloc'd slabs, frees thread back onto a freelist.
enum { SL4B_NAME_MAX = 16 };

struct sl4b_slab {
    struct sl4b_slab *next;
};

struct sl4b_cache {
    int objsize;               // bytes, rounded up to words
    int perslab;               // objects added per slab grow
    struct sl4b_slab *slabs;
    int *freelist;             // threaded through the free objects
    int nallocs;
    int nfrees;
    int nslabs;
    struct sl4b_cache *next;   // all-caches list, for sl4b_stats
    char name[SL4B_NAME_MAX];
};

struct sl4b_cache *sl4b_cache_create(char *name, int objsize, int perslab);
char *sl4b_alloc(struct sl4b_cache *c);
void  sl4b_free(struct sl4b_cache *c, char *obj);
void  sl4b_stats();

// ---- tasks (task.c) ----
// Tasks live on a malloc'd singly linked list -- no fixed table.
// Structs come from an SL4B cache, stacks from malloc (64KB each).
// task_next() wraps from the tail back to the head: that round-robin
// walk is the loop the scheduler context-switches along.
enum { TASK_NAME_MAX = 16 };
enum { TS_READY = 1, TS_RUNNING = 2, TS_ZOMBIE = 3 };
// How a suspended task's context is saved (sched.c):
//   SV_NONE   running, or the adopted boot context (nothing saved)
//   SV_FRAME  a (frame, saved_bp, saved_pc) triple for the LEV path;
//             fresh tasks are forged in this form
//   SV_REGS   full (a, bp, sp, pc) captured by the c4m trap handler
enum { SV_NONE = 0, SV_FRAME = 1, SV_REGS = 2 };

struct task {
    int  id;
    int  state;                // TS_*
    int  entry;                // function address, called via a variable
    int  exitcode;
    struct task *next;         // list order = creation order
    int  sv;                   // SV_*
    int  sv_a;                 // SV_REGS: a
    int  sv_bp;                // SV_REGS: bp    SV_FRAME: saved bp value
    int  sv_sp;                // SV_REGS: sp    SV_FRAME: frame address
    int  sv_pc;                // SV_REGS: pc    SV_FRAME: saved pc value
    int  stack;                // malloc'd stack base, 0 for the boot task
    int  img_code;             // loaded .c4r segments to free on reap,
    int  img_data;             //   0 for kernel-code tasks
    char name[TASK_NAME_MAX];
};

// The list head is shared extern data (c4lc L8 extern-data support:
// references resolve through symbol patches at link time).
extern struct task *task_head;

struct task *task_create(char *name, int entry, int argc, int argv);
struct task *task_adopt(char *name);
void         task_unlink(struct task *t);
void         task_release(struct task *t);
struct task *task_get(int id);
struct task *task_first();
struct task *task_next(struct task *t);
int          task_count();
void         task_shutdown();

// ---- scheduler (sched.c) ----
// One switch mechanism, two backends. On c4m every switch runs in a
// trap handler (soft trap for yield, cycle interrupt for preemption)
// that rewrites its own parameters; TLEV loads them into the VM
// registers. On plain c4 there are no traps: yield rewrites its own
// frame's (saved_bp, return_pc) pair and returns through a double
// LEV -- see src/tests/test_coop_switch.c for the proof of concept.
void         sched_init(int interval);
void         sched_run();
void         sched_stop();
void         sched_yield();
void         sched_lock();     // disable preemption (nestable)
void         sched_unlock();
struct task *sched_current();
int          sched_forge(struct task *t, int entry, int argc, int argv);
void         task_exit(int code);
int          task_wait(struct task *t);
extern int   sched_switches;   // context switches since boot

// ---- .c4r loader (loader.c) ----
struct c4r_image {
    int code;                  // malloc'd code segment (int *)
    int data;                  // malloc'd data segment (char *)
    int entry;                 // absolute entry address
    int ncons;                 // constructors already run at load
    int ndes;                  // destructors present but NOT run (X1)
};

int          c4r_load(char *path, struct c4r_image *img);
struct task *task_spawn(char *path, int argc, int argv);

// ---- init (init.c) ----
int init_main(int argc, int argv);

#endif // __C4IX_H
