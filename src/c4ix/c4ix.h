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

int   host_detect();
int   host_info();
int   host_type();
int   host_has(int bit);
char *host_name();

// ---- console (con.c) ----
// All kernel output goes through here; the only opcode used is PUTC,
// which every host implements.
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

// ---- tasks (task.c) ----
// Tasks live on a malloc'd singly linked list -- no fixed table, no
// TASK_MAX. task_next() wraps from the tail back to the head, which
// is the round-robin walk the X1 scheduler will context-switch with.
enum { TASK_NAME_MAX = 16 };
enum { TS_FREE = 0, TS_READY = 1, TS_DONE = 2 };

struct task {
    int  id;
    int  state;                // TS_*
    int  entry;                // function address, called via a variable
    int  exitcode;
    struct task *next;         // list order = creation order
    char name[TASK_NAME_MAX];
};

struct task *task_create(char *name, int entry);
struct task *task_get(int id);
struct task *task_first();
struct task *task_next(struct task *t);
int          task_count();
int          task_run(struct task *t);
void         task_shutdown();

// ---- init (init.c) ----
int init_main();

#endif // __C4IX_H
