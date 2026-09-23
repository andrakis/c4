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
enum { C4IX_CONF_INTERVAL = 0, C4IX_CONF_HANDLER = 1,
       // c4m's CONF_TRAP_RESTORES_INTERVAL: TLEV restores the interrupt
       // interval from the trap frame, so the mask survives the switch.
       C4IX_CONF_TRAP_RESTORES_INTERVAL = 3 };
enum {
    C4IX_TRAP_ILLOP = 0,        // unknown opcode: the syscall gateway
    C4IX_TRAP_HARD_IRQ = 1,     // cycle interrupt: preemption
    C4IX_TRAP_SOFT_IRQ = 2,     // __c4_trap: cooperative yield
    C4IX_TRAP_SIGNAL = 3,
    C4IX_TRAP_SEGV = 4,
    C4IX_TRAP_OPV = 5,
    C4IX_TRAP_PM = 6            // syscall opcode executed in protected mode
};
// c4m mode register values, assigned into the trap handler's own
// parameter to choose the mode a task resumes in.
enum { C4IX_MODE_UNPROTECTED = 0, C4IX_MODE_PROTECTED = 1 };
// exit status given to a task cancelled by Ctrl-C
enum { C4IX_EXIT_INTERRUPTED = -2 };
// the guarded host opcodes C4IX services on behalf of user tasks
enum {
    C4IX_OP_OPEN = 30, C4IX_OP_READ = 31, C4IX_OP_CLOS = 32,
    C4IX_OP_PRTF = 33, C4IX_OP_MALC = 34, C4IX_OP_FREE = 35,
    C4IX_OP_EXIT = 38, C4IX_OP_PUTC = 39, C4IX_OP_PUTS = 40,
    // INFO is guarded too, and a C4KE program asks for it before it
    // does anything else -- u0's startup and ps both call __c4_info().
    C4IX_OP_INFO = 57
};

// ---- the syscall interface (sys.c) ----
// Userland reaches these through libc4ix (src/c4ix/lib/libc4ix.c),
// never directly: on c4m a stub executes custom opcode SYS_* which
// c4m does not know, raising TRAP_ILLOP into the kernel; on plain c4,
// where no trap machinery exists, the stub calls the kernel's
// dispatcher through the systable the loader injected.
enum {
    SYS_BASE = 200,
    SYS_WRITE = 200, SYS_READ = 201, SYS_OPEN = 202, SYS_CLOSE = 203,
    SYS_EXIT = 204, SYS_YIELD = 205, SYS_SPAWN = 206, SYS_WAIT = 207,
    SYS_SBRK = 208, SYS_GETPID = 209,
    SYS_DUP = 210, SYS_DUP2 = 211, SYS_PIPE = 212,
    SYS_CYCLES = 213, SYS_TASKINFO = 214,
    SYS_CHDIR = 215, SYS_MKDIR = 216, SYS_GETCWD = 217, SYS_READDIR = 218,
    SYS_KILL = 219,
    SYS_SLEEP = 220, SYS_AVAIL = 221, SYS_INTR = 222, SYS_CLOEXEC = 223,
    SYS_TOP = 224          // one past the last: sched_trap's range check
};
// taskinfo fills, in order: id, parent, state, privs, nsyscalls,
// ntraps, cycles, then the name packed into the remaining words.
// One task's worth of what utaskinfo reports: eight integers, then the
// name as TASK_NAME_MAX bytes. Sized generously and in WORDS, because
// how many words sixteen bytes is depends on the machine -- at 9 this
// was two words short on a 32-bit host and utaskinfo wrote past the
// caller's array.
enum { TASKINFO_WORDS = 24 };
enum { FD_STDIN = 0, FD_STDOUT = 1, FD_STDERR = 2, FD_MAX = 16 };

int  sys_dispatch(int num, int *args);   // args[0]=first, args[1]=second...
int  sys_write(int fd, char *buf, int len);
int  sys_read(int fd, char *buf, int len);
int  sys_open(char *path, int flags);
int  sys_close(int fd);
int  sys_dup(int fd);
int  sys_dup2(int oldfd, int newfd);
int  sys_pipe(int *fds);                 // fds[0] read end, fds[1] write end
int  sys_taskinfo(int index, int *out);  // 1 if that slot exists
int  sys_chdir(char *path);
int  sys_mkdir(char *path);
int  sys_getcwd(char *buf, int len);
int  sys_readdir(char *path, int index, char *name);  // <0 end, else isdir
void sys_pmviolation(int op, int *sp, int *returnpc, int *a);

// Set when a syscall could not complete because the calling task had
// to block. The trap handler rewinds the saved pc by one word so the
// task re-executes the syscall opcode when it wakes -- the arguments
// are still on its stack, so the call simply happens again.
extern int sys_restart;

// ---- the VFS (vfs.c) ----
//
// The classic three layers, because they are what dup2 and pipes
// need: a VNODE is the thing (console, RAM file, host file, pipe);
// an open FILE description holds the flags and the seek position;
// an FD is an index in a per-task table pointing at a description.
// dup2 makes two fds share one description -- and therefore one
// position -- while two separate opens of the same file get
// independent positions. Spawned tasks inherit the table, which is
// what makes redirection work: set fd 1 up, spawn, restore.
enum {
    VN_CONSOLE = 1, VN_RAMFILE = 2, VN_HOSTFILE = 3, VN_PIPE = 4,
    VN_DIR = 5
};
enum { VN_NAME_MAX = 24, PATH_MAX = 96 };
// open() flags. The low two bits match the host's O_RDONLY/WRONLY/
// RDWR so they can be passed straight through for host files.
enum {
    C4IX_O_RDONLY = 0, C4IX_O_WRONLY = 1, C4IX_O_RDWR = 2,
    C4IX_O_CREAT = 256, C4IX_O_TRUNC = 512,
    // Linux's value. A task that opens a host file non-blocking means
    // it, and sys_open has to pass the bit through: a read on a
    // blocking host fd calls the host read() directly (vfs.c) and
    // stops the WHOLE VM, not just the caller.
    C4IX_O_NONBLOCK = 2048
};

struct vnode {
    int type;                  // VN_*
    int refs;                  // open descriptions pointing here
    char *data;                // RAMFILE/PIPE storage
    int size;                  // bytes of data valid
    int cap;                   // bytes allocated
    int rpos;                  // PIPE: read cursor into data
    int host;                  // HOSTFILE: the host descriptor
    int writers;               // PIPE: descriptions still open for write
    struct vnode *next;        // sibling in the parent directory
    struct vnode *child;        // VN_DIR: first entry
    struct vnode *parent;       // VN_DIR: enclosing directory
    char name[VN_NAME_MAX];     // one component, not a path
};

struct file {
    struct vnode *vn;
    int flags;
    int pos;                   // seek position (shared across dup2)
    int refs;                  // fds pointing at this description
};

void         vfs_init();
// Names are resolved a component at a time from CWD (or from the
// root when the path starts with '/'), which is what makes "." and
// ".." mean anything and what a flat namespace could never support.
struct vnode *vfs_lookup(char *path);
struct vnode *vfs_ramfile(char *path);   // find or create the file
struct vnode *vfs_mkdir(char *path);
struct vnode *vfs_root();
struct vnode *vfs_cwd();
int          vfs_chdir(char *path);
int          vfs_isdir(struct vnode *vn);
// Directory listing, one entry at a time: index-based so userland
// gets a stable interface without seeing kernel pointers.
int          vfs_direntry(struct vnode *dir, int index, char *name, int *isdir);
struct vnode *vfs_pipe();
struct vnode *vn_hostfile(int host);
int          vfs_read(struct file *f, char *buf, int len);
int          vfs_write(struct file *f, char *buf, int len);
int          vfs_readable(struct vnode *vn, int pos);  // 1 = read won't block
void         vfs_dump(char *name);       // kernel-side: print a RAM file
int          vfs_size(char *name);       // bytes stored, -1 if absent

struct file *fd_get(struct task *t, int fd);
int          fd_install(struct task *t, struct file *f);
int          fd_open_vnode(struct task *t, struct vnode *vn, int flags);
int          fd_close(struct task *t, int fd);
int          fd_dup2(struct task *t, int oldfd, int newfd);
void         fd_init_console(struct task *t);
void         fd_clone(struct task *dst, struct task *src);
void         fd_closeall(struct task *t);

int   host_detect();
int   host_info();
int   host_type();
int   host_has(int bit);
char *host_name();

// ---- console (console.c) ----
// All kernel output goes through here; the only opcode used is PUTC,
// which every host implements. kprintf lines are atomic: the body
// runs inside sched_lock, so preemption never interleaves them.
void con_init();
void con_wake();           // let the next poll reach the host again
int con_poll();            // 1 if a console read would not block
int con_read(char *buf, int len);
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
enum { C4IX_STACK_WORDS = 8192 };   // 64KB per task
// TS_WAITING = blocked on another task (wait); TS_BLOCKED = blocked
// on a vnode (an empty pipe); TS_SLEEPING = blocked on the clock.
// All three are woken by the scheduler when their condition clears.
enum {
    TS_READY = 1, TS_RUNNING = 2, TS_ZOMBIE = 3,
    TS_WAITING = 4, TS_BLOCKED = 5, TS_SLEEPING = 6
};
// Privilege level. PRIV_USER tasks resume in c4m's protected mode:
// host syscall opcodes trap to the kernel instead of executing, so
// all their IO goes through sys.c whether they ask nicely (libc4ix
// syscalls) or not (a raw printf gets emulated onto the fd layer).
// Plain c4 has no protected mode; everything runs PRIV_KERNEL there
// and the boundary is a convention rather than an enforcement.
enum { PRIV_KERNEL = 0, PRIV_USER = 1 };
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
    int  img_cons;             // int * : constructors this task must run
    int  img_ncons;            //   before main, in its OWN context
    int  img_des;              // int * : destructors, run in reverse
    int  img_ndes;             //   after main returns
    int  privs;                // PRIV_*
    int  nsyscalls;            // syscalls serviced, for the X2 report
    int  wait_for;             // TS_WAITING: the task id being waited on
    int  wait_result;          // exit code delivered when the wait completes
    struct vnode *block_vn;    // TS_BLOCKED: the vnode being waited on
    int  block_pos;            // position the blocked read wants data past
    int  fds[FD_MAX];          // struct file *, 0 where the fd is closed
    int  fdcloexec[FD_MAX];    // 1: this fd is not inherited on spawn (SYS_CLOEXEC)
    struct vnode *cwd;         // working directory, inherited on spawn
    int  lockdepth;            // preemption-mask depth, saved across switches
    int  parent;               // task id that spawned this one
    // Cycles, in two words: cycles_hi * 1000000000 + cycles. One is not
    // enough -- the VM's counter is 32 bits on a 32-bit machine and a
    // single C4IX build passes 2^31 several times over, which used to
    // print as a NEGATIVE number of cycles in ps. Decimal carry, so
    // nothing here is ever wider than an int (sched_add_cycles).
    int  cycles;               // VM cycles this task has been given, low
    int  cycles_hi;            // ... and billions
    int  cycles_in;            // counter value when it last started running
    int  ntraps;               // traps taken on its behalf
    // C4KE compatibility state (c4ke.c). Zero for every task that
    // never uses it -- sl4b_alloc memsets, so this costs nothing to
    // carry and nothing to initialise.
    int  ck_sigh;              // int * : CK_SIG_MAX triples, 0 = never used
    int  ck_sigpend;           // pending signal count, so the fast path is one test
    int  ck_excl;              // holds the exclusive (preemption-masked) lock
    int  ck_wake;              // TS_SLEEPING: __time() value to wake at
    int  argv_vec;             // char ** owned by this task, 0 if not owned
    int  argv_data;            // char *  backing store for the above
    char name[TASK_NAME_MAX];
};

// The list head is shared extern data (c4lc L8 extern-data support:
// references resolve through symbol patches at link time).
extern struct task *task_head;
extern int task_last_syscalls;   // syscalls made by the last reaped task

struct task *task_create(char *name, int entry, int argc, int argv);
struct task *task_adopt(char *name);
void         task_unlink(struct task *t);
void         task_release(struct task *t);
void         task_reap();      // free tasks released while they ran
int          task_reap_orphans();          // zombies nobody waits on
int          task_ghost(int id, int *pcode);  // exit code of a reaped task
struct task *task_get(int id);
struct task *task_first();
struct task *task_next(struct task *t);
int          task_count();
void         task_shutdown();

// ---- C4KE compatibility (c4ke.c) ----
//
// C4KE's userland reaches its kernel through custom opcodes >= 128
// that no VM implements, so they arrive as TRAP_ILLOP exactly like
// C4IX's own SYS_* gateway. Only OP_REQUEST_SYMBOL is a fixed number;
// every other service is looked up BY NAME at program start (u0.h's
// __u0_ops_init), so C4IX is free to assign whatever numbers it
// likes. These are they.
enum {
    CK_BASE = 128,
    CK_REQUEST_SYMBOL = 128,   // fixed by include/u0.h
    CK_C4INFO = 129, CK_TIME = 130, CK_SCHEDULE = 131,
    CK_AWAIT_MESSAGE = 132, CK_AWAIT_PID = 133,
    CK_KERN_TASKS_EXPORT = 134, CK_KERN_TASKS_EXPORT_UPDATE = 135,
    CK_KERN_TASKS_EXPORT_FREE = 136, CK_KERN_TASKS_RUNNING = 137,
    CK_USER_START_C4R = 138, CK_KERN_TASK_CURRENT_ID = 139,
    CK_KERN_TASK_RUNNING = 140, CK_KERN_TASK_COUNT = 141,
    CK_TASK_FINISH = 142, CK_TASK_FOCUS = 143, CK_TASK_EXIT = 144,
    CK_USER_SIGNAL = 145, CK_USER_KILL = 146, CK_USER_SLEEP = 147,
    CK_USER_PID = 148, CK_USER_PARENT = 149,
    CK_CURRENTTASK_UPDATE_NAME = 150, CK_DEBUG_KERNELSTATE = 151,
    CK_KERN_REQUEST_EXCLUSIVE = 152, CK_KERN_RELEASE_EXCLUSIVE = 153,
    CK_TASK_CYCLES = 154, CK_HALT = 155,
    CK_TOP = 160               // exclusive; 156..159 spare for the VFS ops
};

// C4KE ABI constants. These MUST match include/u0.h -- they are read
// straight out of a shared buffer by an unmodified C4KE binary.
//
// The task-table export: a three-word header followed by a fixed
// number of fixed-size records. ps walks records 0 .. KTI_USED-1 and
// treats a ZERO STATE as an empty slot, so every live record must
// have STATE_LOADED set.
enum { CK_KTI_COUNT = 0, CK_KTI_USED = 1, CK_KTI_LIST = 2, CK_KTI__Sz = 3 };
enum {
    CK_KTE_STATE = 0, CK_KTE_WAITSTATE = 1, CK_KTE_ID = 2,
    CK_KTE_PARENT = 3, CK_KTE_NAME = 4, CK_KTE_NAMELEN = 5,
    CK_KTE_PRIORITY = 6, CK_KTE_PRIVS = 7, CK_KTE_NICE = 8,
    // CYCLES_HI is billions: C4KE keeps the count in two words because
    // one overflows at 2^31 and printed a negative number of cycles
    // (c4ke.c's kernel_add_cycles). The layout has to match include/u0.h
    // exactly -- this is C4KE's ABI, being spoken by a different kernel.
    CK_KTE_CYCLES = 9, CK_KTE_CYCLES_HI = 10, CK_KTE_TIMEMS = 11,
    CK_KTE_TRAPS = 12, CK_KTE_STACK = 13, CK_KTE_ALLOC = 14,
    CK_KTE__Sz = 15
};
// Bit flags, tested with & by ps -- not small integers like TS_*.
enum {
    CK_STATE_UNLOADED = 0x0, CK_STATE_LOADED = 0x1,
    CK_STATE_RUNNING = 0x2, CK_STATE_WAITING = 0x4,
    CK_STATE_TRAPPED = 0x8, CK_STATE_ETHEREAL = 0x10,
    CK_STATE_ZOMBIE = 0x20
};
enum {
    CK_WSTATE_NONE = 0, CK_WSTATE_TIME = 1, CK_WSTATE_PID = 2,
    CK_WSTATE_SYSCALL = 3, CK_WSTATE_MESSAGE = 4
};
// C4KE's table is a fixed 192 slots; C4IX's task list has no maximum,
// so this is purely how many the export can describe at once.
enum { CK_TASK_SLOTS = 128 };
enum { CK_SIG_MAX = 64 };
enum { CK_SIGINT = 2, CK_SIGKILL = 9, CK_SIGUSR1 = 10, CK_SIGTERM = 15 };
// u0.h:27 -- note this disagrees with C4IX's own PRIV_*, which is
// PRIV_KERNEL = 0, PRIV_USER = 1. Convert at the boundary.
enum { CK_PRIV_NONE = 0, CK_PRIV_USER = 1, CK_PRIV_KERNEL = 2 };

extern int loader_quiet;   // suppress "not an image" while probing paths

int  ck_dispatch(int num, int *args);
void ck_task_free(struct task *t);   // release compat state on reap
int  ck_kill(int pid, int sig);      // queue a signal, or apply the default
int  ck_has_handler(struct task *t, int sig);
// Deliver one pending signal to the task about to resume, by building
// it a trap frame by hand. MODE and INTERVAL are the context it would
// otherwise have resumed with; they go into the frame so the handler's
// return restores them.
void ck_signal_deliver(struct task *t, int *pa, int *pbp, int *psp,
                       int *ppc, int mode, int interval);

// ---- scheduler (sched.c) ----
// One switch mechanism, two backends. On c4m every switch runs in a
// trap handler (soft trap for yield, cycle interrupt for preemption)
// that rewrites its own parameters; TLEV loads them into the VM
// registers. On plain c4 there are no traps: yield rewrites its own
// frame's (saved_bp, return_pc) pair and returns through a double
// LEV -- see src/tests/test_coop_switch.c for the proof of concept.
void         sched_init(int interval);
void         sched_run();
int          sched_sleep_due(int *pms);   // earliest sleeper deadline, 0 if none
void         sched_forge_argv(struct task *t, int argv);  // pre-start argv patch
void         sched_stop();
void         sched_yield();
int          sched_intr_below(struct task *root);   // a terminal's Ctrl-C
void         sched_lock();     // disable preemption (nestable)
void         sched_unlock();
struct task *sched_current();
int          sched_forge(struct task *t, int entry, int argc, int argv);
int          sched_in_trap();  // servicing a trap: no re-entry allowed
void         task_exit(int code);
int          task_wait(struct task *t);
extern int   sched_switches;   // context switches since boot

// ---- .c4r loader (loader.c) ----
// Constructor and destructor lists are copied out of the image buffer
// and resolved to ABSOLUTE addresses, because the task runs them
// itself: at load time the task does not exist yet, and a constructor
// that asks who it is would be told the spawning task.
struct c4r_image {
    int code;                  // malloc'd code segment (int *)
    int data;                  // malloc'd data segment (char *)
    int entry;                 // absolute entry address
    int cons;                  // malloc'd int[ncons], absolute addresses
    int ncons;
    int des;                   // malloc'd int[ndes], absolute addresses
    int ndes;
};

int          c4r_load(char *path, struct c4r_image *img);
struct task *task_spawn(char *path, int argc, int argv);
struct task *task_spawn_priv(char *path, int argc, int argv, int privs);

// ---- init (init.c) ----
int init_main(int argc, int argv);

#endif // __C4IX_H
