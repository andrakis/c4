//
// C4IX: kernel-wide declarations.
//
// Every module includes this and nothing else. c4lc preprocesses
// each module itself (its L9 preprocessor; no gcc -E in the build),
// so plain macros and quoted includes are available.
//

#ifndef __C4IX_H
#define __C4IX_H 1

// ---- host detection (host.c) ----
// __c4_info() bit values, mirroring c4m.c's C4I_* enum. boot.c refuses
// a host without C4IX_I_C4M. (Plain c4 has no INFO opcode at all -- the
// "returns 0" degraded signal was only true while c4.c carried extended
// opcodes, reverted in 29d6f57 -- so C4IX reaches plain c4 only through
// c4m: c4 -> c4m.c -> C4IX.)
enum {
    C4IX_I_C4   = 0x01,  // ultimately running under plain c4
    C4IX_I_C4M  = 0x02,  // running under c4m
    C4IX_I_HRT  = 0x10,  // high resolution timer
    C4IX_I_SIG  = 0x20,  // signals supported
    C4IX_I_FLT  = 0x40,  // floating point supported
    C4IX_I_PROT = 0x80,  // protected mode available
    C4IX_I_GUI  = 0x4000 // libjs's display is fitted (libjs/guest/gui.h)
};
// The display's ring register (libjs/gui-device.js): where the host
// writes events, into a task's memory. task.c reads it, and clears it
// when the memory goes with a task that never detached.
enum { C4IX_GUI_RING = 0x410 };
// The kernel image as libjs's loader placed it (src/c4bb/sim/devices.js,
// IMG_*): code, data, where the file's data ends and BSS begins, and the
// end. Libjs's alone: C4IX_I_C4MJS.
enum { C4IX_IMG_CODE = 0x1f0, C4IX_IMG_DATA = 0x1f4, C4IX_IMG_BSS = 0x1f8, C4IX_IMG_END = 0x1fc };
// More of the machine, for the boot report (boot.c): the drives, the
// mailbox, the display and the sound card
enum { C4IX_DISK_DRIVE = 0x13c, C4IX_DISK_COUNT = 0x188, C4IX_DISK_RO = 0x18c,
       C4IX_MBOX_BASE = 0x1b4, C4IX_MBOX_LEN = 0x1b8,
       C4IX_GUI_CAPS = 0x400, C4IX_GUI_W = 0x404, C4IX_GUI_H = 0x408,
       C4IX_I_PIT = 0x800, C4IX_I_MBOX = 0x2000, C4IX_I_SND = 0x8000, C4IX_I_FPU = 0x10000, C4IX_I_DRV = 0x40000 };
// The heap's extent and use (c4bb's HEAP_BASE and HEAP_END, libjs's
// HEAP_USED, src/c4bb/sim/devices.js), for the CPU snapshot.
enum { C4IX_HEAP_BASE = 0x144, C4IX_HEAP_END = 0x148, C4IX_HEAP_USED = 0x1c0, C4IX_I_C4MJS = 8 };
// The boot stack (libjs's STACK_BASE, STACK_TOP and STACK_GUARD, same
// file): the megabyte at the top of memory, under the arguments, where the
// machine started the kernel. Once the kernel is off it for good, libjs
// guards it (sched.c, "Off the boot stack"). Libjs's alone: C4IX_I_C4MJS.
enum { C4IX_BOOT_BASE = 0x1e4, C4IX_BOOT_TOP = 0x1e8, C4IX_BOOT_GUARD = 0x1ec };
// Processors (docs/c4ix-smp.md S3): c4mp's C4I_SMP, which libjs reports
// with --cpus. Only libjs has the CPU-local window C4IX needs, 0x800 to
// 0x8FF, where each CPU sees 64 words of its own: its struct cpu at the
// bottom, the shared libraries' data slots above it (loader.c).
enum { C4IX_I_SMP = 0x100, CPU_MAX = 16, CPU_WINDOW = 0x800, CPU_SLOT0 = 24, CPU_SLOTS = 40 };
// libjs's: the CPUs run at once, a thread each, not in turns on one
enum { C4IX_I_PAR = 0x20000 };

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
    // ITH would take the machine's trap handler, the kernel's: refused
    C4IX_OP_ITH = 44,
    // USLP sleeps the whole machine, so a task gets a sleep of its own
    // SIGH sets the machine's handler, the kernel's: a task gets its own
    C4IX_OP_SIGH = 54,
    C4IX_OP_USLP = 56,
    // INFO is guarded too, and a C4KE program asks for it before it
    // does anything else -- u0's startup and ps both call __c4_info().
    C4IX_OP_INFO = 57
};

// ---- the syscall interface (sys.c) ----
// Userland reaches these through libc4ix (src/c4ix/lib/libc4ix.c),
// never directly: a stub executes custom opcode SYS_* which c4m does
// not know, raising TRAP_ILLOP into the kernel.
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
    SYS_STAT = 224, SYS_UNLINK = 225, SYS_RENAME = 226, SYS_LAZYFILE = 227,
    SYS_STACK = 228, SYS_CPUSTAT = 229,
    SYS_EVWAIT = 230, SYS_DISCARD = 231,
    // POSIX's process and file calls, for Busybox (docs/busybox-port.md §4)
    SYS_VFORK = 232, SYS_EXECVE = 233, SYS_WAITPID = 234, SYS_LSEEK = 235,
    SYS_FSTAT = 236, SYS_LINK = 237, SYS_FTRUNCATE = 238, SYS_ENVIRON = 239,
    // libjs's display, taken and given back (sys.c, "The display")
    SYS_DISPLAY = 240,
    // the CPUs a task may run on (sched.c, sched_affinity)
    SYS_AFFINITY = 242,
    // a task's priority (sched.c, sched_nice)
    SYS_NICE = 243,
    // c4bb's device registers for this task, under the MMU (mmu.c)
    SYS_IOPERM = 244,
    SYS_MAPFROM = 245,
    SYS_TOP = 246          // one past the last: sched_trap's range check
};
// SYS_DISPLAY's operations, and TAKE's flag (include/display.h)
enum { DISP_TAKE = 0, DISP_STATE = 1, DISP_YIELD = 2, DISP_GIVE = 3, DISP_YIELDS = 1 };
// What SYS_STAT and SYS_FSTAT write, a word each, as many as the caller
// asks for: SYS_STAT's callers from before M7c ask for none, which is
// the first four.
enum {
    ST_TYPE = 0,           // 1 directory, 2 RAM file, 3 anything else
    ST_SIZE = 1, ST_PROG = 2, ST_MTIME = 3,
    ST_INO = 4,            // the vnode's number: two names of one file share it
    ST_NLINK = 5,          // its names (SYS_LINK)
    ST_MODE = 6,           // made up: 0755 for directories and programs, 0644 for the rest
    ST_DEV = 7,            // 1 for the RAM filesystem, 2 for a host file, 0 for the console or a pipe
    ST_WORDS = 8
};
// SYS_WAITPID: options, and the two words it writes (exit code, signal)
enum { C4IX_WNOHANG = 1 };
// taskinfo fills, in order: id, parent, state, privs, nsyscalls,
// ntraps, cycles, then the name packed into the remaining words.
// One task's worth of what utaskinfo reports: eight integers, then the
// name as TASK_NAME_MAX bytes. Sized generously and in WORDS, because
// how many words sixteen bytes is depends on the machine -- at 9 this
// was two words short on a 32-bit host and utaskinfo wrote past the
// caller's array.
// Past the name, at TI_CPU, the CPU running the task (-1 none), and at
// TI_NCPU how many the machine has (docs/c4ix-smp.md S4).
enum { TASKINFO_WORDS = 24, TI_CPU = 20, TI_NCPU = 21 };
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
void sys_display_drop(int id);

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
// VN_LINK is a second name for a RAM file (SYS_LINK): an entry in a
// directory that holds no bytes, only the file it names. A path that
// ends at one resolves to its file; the file keeps its first name.
// VN_PROC is a read-only file whose bytes the kernel makes when it is
// read: /proc/uptime (vfs.c, vfs_proc).
enum {
    VN_CONSOLE = 1, VN_RAMFILE = 2, VN_HOSTFILE = 3, VN_PIPE = 4,
    VN_DIR = 5, VN_LINK = 6, VN_PROC = 7
};
// 64: the root archive carries the repo's own tree, whose longest file
// name is 26 characters (24 truncated them). Userland buffers that
// receive a name from readdir must hold VN_NAME_MAX bytes.
enum { VN_NAME_MAX = 64, PATH_MAX = 256 };
// open() flags. The low two bits match the host's O_RDONLY/WRONLY/
// RDWR so they can be passed straight through for host files.
enum {
    C4IX_O_RDONLY = 0, C4IX_O_WRONLY = 1, C4IX_O_RDWR = 2,
    C4IX_O_CREAT = 256, C4IX_O_TRUNC = 512,
    // Linux's value. A task that opens a host file non-blocking means
    // it, and sys_open has to pass the bit through: a read on a
    // blocking host fd calls the host read() directly (vfs.c) and
    // stops the WHOLE VM, not just the caller.
    C4IX_O_NONBLOCK = 2048,
    // every write lands at the end; and with O_CREAT, fail if it exists.
    // libc's values (libc.h), so its open() passes them straight through.
    C4IX_O_APPEND = 4096, C4IX_O_EXCL = 8192
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
    // A lazy RAM file (SYS_LAZYFILE): its bytes are still on the host,
    // under this name, and are read in the first time anything opens,
    // reads, writes or loads it. size is already right.
    char *hostname;
    int lazy;
    // When it last changed, as a place in the order of changes rather
    // than a time (vfs_touch): b4ke compares two, nothing else does. A
    // file still on the host is 0, older than anything made here.
    int mtime;
    int ino;                    // its number, for stat: one per vnode, never reused
    // Links (SYS_LINK). A VN_LINK entry's target is its file, and lnext
    // the file's next link. A file's links is its first link and nlinks
    // how many it has, so stat's nlink is one more.
    struct vnode *target;
    struct vnode *links;
    struct vnode *lnext;
    int nlinks;
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
int          vfs_stat(char *path, int *out, int n);   // the first N ST_* words; -1 if no such path
void         vn_stat(struct vnode *vn, int *out, int n);   // the same, of an open file's vnode
void         vfs_touch(struct vnode *vn);        // it changed: give it the next mtime
int          vfs_link(char *path, char *newpath);   // a second name for a RAM file
int          vfs_truncate(struct vnode *vn, int len);  // a RAM file cut, or grown with zeros, to LEN
int          vfs_unlink(char *path);
int          vfs_rename(char *path, char *newname);
int          vfs_lazyfile(char *path, char *host, int size);
int          vfs_fill(struct vnode *vn);         // read a lazy file's bytes in; 1 if it has them
int          vfs_put(char *path, char *buf, int len);   // C4KE OP_VFS_PUT
struct vnode *vfs_klog();                                // /log/kernel, held by the kernel
void         vfs_logc(struct vnode *vn, int c, int max); // one byte on its end, up to MAX
char        *vfs_getdata(char *path, int *plen);        // C4KE OP_VFS_GET
int          vfs_nfiles();                              // C4KE OP_VFS_COUNT: files in the cwd
char        *vfs_fname(int i);                          // C4KE OP_VFS_NAME
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
void         fd_clone_all(struct task *dst, struct task *src);   // vfork: close-on-exec fds too
void         fd_closeall(struct task *t);
void         fd_close_on_exec(struct task *t);

int   host_detect();
int   host_info();
int   task_info();   // host_info() as a task sees it: plus C4I_TRAPH
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
void con_logopen();        // /log/kernel exists now: the console is logged to it
void con_booted();         // boot is over: tasks' console output is no longer logged
void con_write_task(char *buf, int len);  // a task's bytes to the console
int kputs(char *s);
int kprintf(char *fmt, ...);
// The kernel log (console.c): every line the kernel says, stamped with
// the machine's time, in a ring that dmesg reads through /proc/kmsg.
// klog's lines go there, and to the console too while the boot goes on
// libjs (klog_boot: the page and cli.js, as Linux's boot does; -q keeps
// them in the log) and always when the kernel was booted with -v
// (klog_verbose); kprintf's go to both.
int klog(char *fmt, ...);
extern int klog_verbose;
extern int klog_boot;
extern int klog_cpms;          // cycles in a machine millisecond, once klog_calibrate has measured
int klog_calibrate();
int klog_copy(char *dst);      // the log into DST, KLOG_MAX bytes at least; its length
enum { KLOG_MAX = 65536 };
int con_is_booting();          // init has not started the shell yet
extern int vfs_nlazy, vfs_lazybytes;   // what the boot left on the host (SYS_LAZYFILE)
void boot_report();            // the machine, into the kernel log (boot.c)
void boot_done();              // the boot is over: how long it took (boot.c)
extern int boot_lastid;        // the last task the boot made: its tasks' exits are logged

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

// ---- the kernel arena (sl4b.c) ----
// What the kernel keeps for as long as it runs -- the slabs and their
// caches, the kernel task's and the idle tasks' stacks, the shared
// libraries' books -- is carved from one block taken at boot, before
// anything a program could have: on libjs, the heap's lowest addresses,
// directly after the kernel image. A program that writes past the end of
// its block runs upwards, into other programs' memory and then the boot
// stack's guard, and never into a task struct or an idle task's frames.
// Nothing carved is given back (kmem_free leaves it be). Past the end,
// kmem_alloc takes from the heap and counts it (kmem_spills).
// KMEM_STACK_BYTES: the kernel task's stack, and each idle task's.
// KMEM_SLAB_BYTES: room for slabs and books, a few times what the desktop uses.
enum { KMEM_STACK_BYTES = 65536, KMEM_SLAB_BYTES = 524288 };
extern int kmem_base, kmem_end, kmem_brk, kmem_spills;
void  kmem_init(int ncpu);
char *kmem_alloc(int bytes);
void  kmem_free(char *p);

// ---- tasks (task.c) ----
// Tasks live on a malloc'd singly linked list -- no fixed table.
// Structs come from an SL4B cache, stacks from malloc (256KB each on a
// 32-bit host).
// task_next() wraps from the tail back to the head: that round-robin
// walk is the loop the scheduler context-switches along.
enum { TASK_NAME_MAX = 16 };
// 256KB on a 32-bit host, C4KE's TASK_STACK_SIZE: C4IX runs C4KE's
// programs. c4sc compiling the larger kernel modules overran 64KB under
// C4KE first (docs/task-memory.md) and then here, where it wrote over
// the heap and took every task down. That was a c4lc bug, g:resolvecode
// recursing once per instruction (29cbac9); c4sc now needs 11KB at most.
// The lowest C4IX_STACK_GUARD words are poison (sched_forge), checked
// at every switch, so an overrun now kills the task that did it.
enum { C4IX_STACK_WORDS = 65536, C4IX_STACK_GUARD = 64, C4IX_STACK_POISON = 0x57415348 };
// A bigger stack for one spawn: SYS_STACK sets it per spawning task (b4ke's
// $(STACK)), and sys.c hands it to sched_forge through this, for one call.
// Up to C4IX_STACK_MAX bytes.
enum { C4IX_STACK_MAX = 16777216 };
extern int sched_next_stack_words;
// TS_WAITING = blocked on another task (wait); TS_BLOCKED = blocked
// on a vnode (an empty pipe); TS_SLEEPING = blocked on the clock.
// All three are woken by the scheduler when their condition clears.
// TS_VFORK = a vfork parent, parked until its child (wait_for) execs or
// exits, because until then the child is using its memory.
enum {
    TS_READY = 1, TS_RUNNING = 2, TS_ZOMBIE = 3,
    TS_WAITING = 4, TS_BLOCKED = 5, TS_SLEEPING = 6, TS_VFORK = 7
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
    int  stack;                // malloc'd stack base, 0 for the kernel task
    int  stack_words;          // its size, C4IX_STACK_WORDS unless SYS_STACK asked
    int  img_bytes;            // what its image took to load: code, data, lists, library data
    int  spawn_stack;          // SYS_STACK: bytes for the tasks this one spawns, 0 default
    int  img_code;             // loaded .c4r segments to free on reap,
    int  img_data;             //   0 for kernel-code tasks
    int  img_cons;             // int * : constructors this task must run
    int  img_ncons;            //   before main, in its OWN context
    int  img_des;              // int * : destructors, run in reverse
    int  img_ndes;             //   after main returns
    int  img_libs;             // int * : shared libraries' code, data and slot, in threes
    int  img_nlibs;            //   (loader.c), freed with the task
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
    // Cycles (sched.c, "accounting"): what the task ran itself, outside
    // the trap handler, and what the handler spent on its behalf -- its
    // syscalls, preemption ticks and switches, which System Monitor shows
    // as System. 64 bits, since one C4IX build passes 2^31 several times
    // over. A trap adds into the int accumulators, and they are folded
    // into the totals before they could overflow and whenever the
    // totals are read, so the hot path does no long long arithmetic.
    long long ucycles;
    long long kcycles;
    int  uacc;                 // not yet in ucycles
    int  kacc;                 // not yet in kcycles
    int  ntraps;               // traps taken on its behalf
    // SYS_EVWAIT: what besides the clock ends this task's sleep -- 1, an
    // event in the GUI ring at ev_ring; 2, data or end of file on a pipe
    // it reads (sched_events)
    int  ev_wait;
    int  ev_ring;
    // C4KE compatibility state (c4ke.c). Zero for every task that
    // never uses it -- sl4b_alloc memsets, so this costs nothing to
    // carry and nothing to initialise.
    int  ck_sigh;              // int * : CK_SIG_MAX triples, 0 = never used
    int  ck_sigpend;           // pending signal count, so the fast path is one test
    int  ck_excl;              // holds the exclusive (preemption-masked) lock
    int  ck_wake;              // TS_SLEEPING: __time() value to wake at
    int  argv_vec;             // char ** owned by this task, 0 if not owned
    int  argv_data;            // char *  backing store for the above
    int  ublocks;              // int * : its SYS_SBRK / malloc blocks (task_ualloc), freed with it
    // The environment SYS_EXECVE gave it, "NAME=VALUE" strings ending in
    // 0, the kernel's copy (SYS_ENVIRON); a spawned child gets a copy.
    int  env_vec;
    int  env_data;
    // vfork (sched_vfork). A child's vfparent is the parent whose image,
    // library data and heap it borrows until it execs or exits: its
    // img_libs are the parent's, and nothing of them is its to free.
    struct task *vfparent;
    int  wait_posix;           // TS_WAITING in SYS_WAITPID: woken to run it again
    int  posix;                // has used vfork or waitpid: its zombies wait for it
    int  killsig;              // the signal that killed it, 0 if it exited
    // Processors (sched.c). oncpu is 1 + the CPU that may still be on its
    // stack: the one running it, or the one that switched it out until
    // that CPU has left the trap it did it in. Until then no other CPU
    // resumes it and nothing frees it. cpuidle is 1 + the CPU whose idle
    // task it is, 0 for every other task. cpumask is the CPUs it may
    // run on, bit k for CPU k, 0 for any (pin, SYS_AFFINITY); a child
    // starts with its parent's.
    int  oncpu;
    int  cpuidle;
    int  cpumask;
    // Priority (sched.c, "nice"): -10, most favoured, to 10, 0 for most.
    // woke is set when a sleep or a wait ends, and cleared when the task
    // next runs: only then may it preempt a less favoured one.
    int  nice;
    int  woke;
    // Under the MMU (mmu.c): the regions of memory it owns, the chunk its
    // small blocks are being cut from, and its slabs of each size
    int  urgns;
    int  uchunk;
    int  uslab[8];
    int  iopm;                 // what of page 0 it asked for (SYS_IOPERM)
    int  ckabi;                // has made a C4KE call: a C4KE program
    int  mmdir;                // its page directory: its address space (0 until it needs one)
    char name[TASK_NAME_MAX];
};

// What SYS_CPUSTAT copies out (sched_cpustat): a header, then one record
// per task, all taken in one trap so that every number describes the
// same instant. Userland's copy is in c4ix_user.h.
struct cpustat {
    int ntasks;                // records that follow, at most the caller's max
    int ms;                    // __time() when it was taken
    int napms;                 // machine ms the idle loop has slept, since boot
    int idle;                  // the idle task's id: its cycles are idle time
    long long gone_user;       // the cycles of tasks already reaped
    long long gone_kernel;
    int heap;                  // the machine's heap, bytes; 0 if the host does not say
    int heapused;              // ... and the bytes of it in use
    // Processors (docs/c4ix-smp.md S4): how many, 1 unless SMP; and for
    // each, its idle task's id, the machine ms it has parked or napped,
    // and its cycles since boot -- busy, every task but its idle task's
    // and the kernel's work for them, and idle, its idle task's
    int ncpu;
    int idles[CPU_MAX];
    int cnap[CPU_MAX];
    long long cbusy[CPU_MAX];
    long long cidle[CPU_MAX];
};
struct cpustat_task {
    int id, parent, state, privs, nsyscalls;
    long long user;            // cycles it ran outside the trap handler
    long long kernel;          // cycles the handler spent on it
    int mem;                   // bytes it holds: image, stack, library data, allocations
    int cpu;                   // the CPU running it, -1 if none is
    char name[TASK_NAME_MAX];
};

// The list head is shared extern data (c4lc L8 extern-data support:
// references resolve through symbol patches at link time).
extern struct task *task_head;
extern int task_last_syscalls;   // syscalls made by the last reaped task

struct task *task_create(char *name, int entry, int argc, int argv);
struct task *task_adopt(char *name);
// A vfork child of P: a stack of its own, P's descriptors (close-on-exec
// ones too), directory, environment and libraries. Its registers are
// sched_vfork's to set.
struct task *task_vchild(struct task *p);
void         task_unlink(struct task *t);
void         task_release(struct task *t);
void         task_reap();      // free tasks released while they ran
int          task_reap_orphans();          // zombies nobody waits on
int          task_ghost(int id, int *pcode);  // exit code of a reaped task
// A child of PARENT (PID, or any for -1) that the idle sweep reaped
// before PARENT's waitpid came: its id, code and signal, collected once
int          task_ghost_child(int parent, int pid, int *pcode, int *psig);
// Copy N strings (N < 0: up to a null pointer) into one block of
// kernel memory: *PVEC the vector, null-terminated, *PDATA the bytes.
// The count, or -1 out of memory.
int          task_strvec(char **v, int n, int *pvec, int *pdata);
void         task_drop_image(struct task *t);   // execve: free its image, libraries and heap
void         task_unborrow(struct task *t);     // a vfork child is done with its parent's memory
struct task *task_get(int id);
int sched_affinity(int id, int mask);
int sched_nice(int id, int nice);
struct task *task_first();
struct task *task_next(struct task *t);
int          task_count();
void         task_shutdown();
// A userland task's memory (SYS_SBRK, and malloc behind protected mode):
// kernel malloc blocks, each on its task's ublocks list behind a header,
// and all freed with the task (task_destroy). task_ufree takes back one,
// or plainly frees a pointer that is not such a block.
int          task_ualloc(struct task *t, int bytes);
void         task_ufree(int p);
int          task_umem(struct task *t);   // the bytes of its blocks, headers too

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
    // C4KE's kernel RAM filesystem, over C4IX's VFS: how c4cc, c4rlink
    // and c4lc write what they build (docs/c4ix-desktop.md, part three)
    CK_VFS_PUT = 156, CK_VFS_GET = 157, CK_VFS_UNLINK = 158,
    CK_VFS_COUNT = 159, CK_VFS_NAME = 160,
    CK_TOP = 161               // exclusive
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
// The machine's TLEV word (libjs's ROM, at TLEV_ADDR): where a signal
// handler returns under the MMU, which lets user mode run nothing else
// of the kernel's
enum { CK_TLEV_ROM = 0x10 };
// u0.h:27 -- note this disagrees with C4IX's own PRIV_*, which is
// PRIV_KERNEL = 0, PRIV_USER = 1. Convert at the boundary.
enum { CK_PRIV_NONE = 0, CK_PRIV_USER = 1, CK_PRIV_KERNEL = 2 };

extern int loader_quiet;   // suppress "not an image" while probing paths

int  ck_dispatch(int num, int *args);
void ck_task_free(struct task *t);   // release compat state on reap
int  ck_argv_copy(struct task *t, int argc, char **argv);   // the task owns a copy of argv
void ck_exec_reset(struct task *t);  // execve: its handlers were the old image's
int  ck_kill(int pid, int sig);      // queue a signal, or apply the default
int  ck_has_handler(struct task *t, int sig);
int  ck_signal(struct task *t, int sig, int handler);   // the old handler
// Deliver one pending signal to the task about to resume, by building
// it a trap frame by hand. MODE and INTERVAL are the context it would
// otherwise have resumed with; they go into the frame so the handler's
// return restores them.
void ck_signal_deliver(struct task *t, int *pa, int *pbp, int *psp,
                       int *ppc, int mode, int interval);

// ---- scheduler (sched.c) ----
// Every switch runs in a trap handler (soft trap for yield, cycle
// interrupt for preemption) that rewrites its own parameters; TLEV
// loads them into the VM registers.
int          sched_ncpus();       // the processors C4IX will use
// Run ENTRY(argc, argv) as the kernel task, on a stack of its own, and
// return what it returns: the boot stack is left until then (sched.c)
int          sched_offboot(int entry, int argc, int argv);
void         sched_init(int interval);
void         sched_run();
int          sched_sleep_due(int *pms);   // earliest sleeper deadline, 0 if none
void         sched_forge_argv(struct task *t, int argv);  // pre-start argv patch
void         sched_stop();
void         sched_yield();
void         sched_new_slice();   // the running task's slice restarts
int          sched_intr_below(struct task *root);   // a terminal's Ctrl-C
void         sched_lock();     // disable preemption (nestable)
void         sched_unlock();
int          sched_forge(struct task *t, int entry, int argc, int argv);
// SYS_VFORK, from the trap handler, which has the registers (bp is the
// frame of the function that executed the syscall: the child returns
// from it). The child's id, or -1.
int          sched_vfork(int bp);
// What each processor keeps for itself (docs/c4ix-smp.md S3). cpu_self
// points at it: at an ordinary struct on one CPU, and at the CPU-local
// window under SMP, the same address on every CPU, through which each
// sees its own copy. It fits below CPU_SLOT0.
struct cpu {
    struct task *cur;          // the running task (sched_current)
    int intrap;                // inside the trap handler: no re-entry (sched_in_trap)
    int lockdepth;             // the running task's preemption mask (sched_lock)
    int id;                    // __c4_cpu_id(), 0 on one CPU
    int resched;               // a service in this trap asked for a switch
    int dying;                 // task_exit outside the handler: die in the next trap
    int dying_code;
    int acct_in;               // __c4_cycles() as this trap began
    int acct_out;              // ... as the last one ended
    int slice0;                // acct_in when the running task's slice began
    int reload;                // SYS_EXECVE: resume the running task from its forged frame
    int stale_stack;           // SYS_EXECVE: the old stack, freed at this CPU's next trap
    struct task *prev;         // switched out here; its oncpu is cleared once this CPU is off its stack
    struct task *idle;         // this CPU's idle task
    int bkl_task;              // the running task holds the kernel lock (a kernel task)
};
extern struct cpu *cpu_self;
extern int smp_ncpu;           // processors in use: 1 unless the host has SMP
void sched_early();            // cpu_self, before anything locks (boot.c)
int  task_busy(struct task *t);   // some CPU may be on its stack: not to be freed yet
int  task_cpu(struct task *t);    // the CPU running it, -1 if none is
int  smp_slot_alloc();         // a library's data slot in the window, 0 if none is left
// A lock: { 1 + the CPU holding it, its holds }, nested per CPU (sched.c)
void klock_take(int *lk);
void klock_drop(int *lk);
// SYS_EXECVE replaced the running task's context: the trap returns into
// the forged one, and the old stack, still under the handler, is freed
// at the next trap.
#define sched_reload (cpu_self->reload)
#define sched_stale_stack (cpu_self->stale_stack)
// The running task, and whether the trap handler is running (no
// re-entry allowed). Read by every module on every syscall, so they are
// macros rather than calls: -minline cannot inline across modules.
#define sched_cur (cpu_self->cur)
#define sched_intrap (cpu_self->intrap)
#define sched_current() sched_cur
#define sched_in_trap() sched_intrap
extern struct task *task_reaplist;  // released while running, to be freed (task.c)
void         task_exit(int code);
void         task_zombify(struct task *t, int code);  // every death: wakes its waiter
int          sched_events(struct task *t);  // SYS_EVWAIT: has what it waits for come?
extern int   sched_nlive;      // tasks still to finish (sched.c)
extern int   sched_nzombies;
int          task_wait(struct task *t);
extern int   sched_switches;   // context switches since boot
long long    task_cycles(struct task *t);  // everything it has run, both kinds
void         sched_retire(struct task *t); // its cycles to the reaped total
int          sched_cpustat(struct cpustat *h, struct cpustat_task *r, int max);

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
    int libs;                  // malloc'd int[3 * nlibs]: each library's code (0 if shared), data, slot
    int nlibs;
    int bytes;                 // all of the above, as the heap counts it (shared code not)
};

int          c4r_load(char *path, struct c4r_image *img);
void         c4r_unload(struct c4r_image *img);   // free all of it
void         loader_switch(struct task *t);       // point -fpic libraries at t's data
struct task *task_spawn(char *path, int argc, int argv);
struct task *task_spawn_priv(char *path, int argc, int argv, int privs);

// ---- init (init.c) ----
int init_main(int argc, int argv);

// ---- the MMU (mmu.c, docs/c4ix-mmu.md) ----
// libjs's, when its host fits one (C4I_MMU). It translates user mode;
// the kernel runs untranslated. Its registers are each CPU's own.
enum { C4IX_I_MMU = 0x80000 };
enum {
    MMU_CTL = 0x500, MMU_PTBR = 0x504, MMU_KSP = 0x508, MMU_FADDR = 0x50c,
    MMU_FINFO = 0x510, MMU_FPC = 0x514, MMU_INVLPG = 0x518, MMU_FLUSH = 0x51c,
    MMU_IOPM = 0x520, MMU_MISSES = 0x524, MMU_FAULTS = 0x528, MMU_MEMSIZE = 0x52c,
    MMU_UWIN = 0x530, MMU_UDIR = 0x534
};
// Where user addresses start: a user page is mapped at its physical
// address plus this, and kernel mode reaches it at either (libjs's user
// window). Memory larger than it keeps the identity map.
enum { MMU_UWIN_BASE = 0x10000000 };
// A page table entry's bit the host sets in the kernel's map (UDIR) on a
// page the kernel or a device has used: it never goes to swap
enum { PTE_PIN = 0x200 };
// ... and the bit, in the kernel's map, of a page put out to swap: not
// present, and the host's slot for it in the entry's upper 20 bits
enum { PTE_SWAP = 0x400 };
// A page; a CPU's stack for the traps that come from user mode
enum { MMU_PAGE = 4096, MMU_KSTACK = 262144 };
// A page table entry: the frame's address and these
enum { PTE_P = 1, PTE_W = 2, PTE_U = 4, PTE_X = 8, PTE_A = 0x20, PTE_D = 0x40 };
// What a fault was (MMU_FINFO)
// DMA: a device's access, at an address the task gave it
enum { MMU_FK_READ = 1, MMU_FK_WRITE = 2, MMU_FK_FETCH = 4, MMU_FK_PRESENT = 8, MMU_FK_DMA = 32 };
// The signal a page fault kills with, as waitpid reports it
enum { C4IX_SIGSEGV = 11 };
extern int mmu_on;
extern int mmu_uoff;                   // the user window's base, or 0: user addresses are physical
int  mmu_uva(int p);                   // a user region's memory as user mode sees it (else P)
int  mmu_phys(int p);                  // ... and as it is in memory
void mmu_init();                       // at boot: tables, and on
int  sched_has_bkl();                  // this CPU holds the kernel lock (sched.c)
void mmu_trap_in();                    // sched_trap's first thing: a shootdown asked of this CPU
void mmu_trap_out(struct task *t);     // ... and its last, T the task it goes back to
void mmu_cpu(int k);                   // another CPU, as it starts
int  mmu_map(int *dir, int va, int pa, int n, int flags);
extern int mmu_short;                  // memory is short: allocation takes the kernel lock (sched_light)
void mmu_fault(struct task *t, int va);   // TRAP_SEGV from a user task: kill it
// What user mode may use of page 0 (MMU_IOPM)
enum { IOPM_ROM = 1, IOPM_DEV = 2, IOPM_GUI = 4, IOPM_SND = 8, IOPM_LOCAL = 16, IOPM_LOCALW = 32 };
// ... every user task's: the ROM, the display, the sound card, reading the window
// A user task's: the display, the sound card, and the CPU-local window to
// read. Not the ROM: a null pointer's read faults (TLEV still runs there).
enum { MMU_IOPM_USER = 4 | 8 | 16 };
int  umalloc_stack(int n);             // umalloc, and a guard page below it
int  mmu_ioperm(struct task *t, int bits);   // SYS_IOPERM
int  mmu_mapfrom(struct task *t, int va, int n);   // SYS_MAPFROM
// A user task's memory. Without the MMU these are malloc and free.
// umalloc: whole pages, the kernel's until mmu_adopt gives them to the
// task they were made for (its stack, image, arguments); ufree gives such
// a region back, or frees a kernel block. umem_alloc and umem_free: the
// blocks a task asks for (SYS_SBRK, malloc behind protected mode), small
// ones from slabs of its own pages; umem_free_all, its heap (ALL:
// everything it owns).
// loader.c: the image being loaded is a user task's, relocated to the
// addresses user mode sees (mmu_uva)
extern int loader_uva;
int  umalloc(int n);
void ufree(int p);
void umem_share(int p);                // a cached library's code: in every space, never freed
void mmu_adopt(struct task *t);        // what was made for T is T's
int  umem_alloc(struct task *t, int n);
void umem_free(struct task *t, int p);
void umem_free_all(struct task *t, int all);
int  umem_bytes(struct task *t);
int  mmu_user_entry(struct task *t);   // a page that runs T's constructors, main and exit
int  mmu_slot();                       // a word user mode may read: a library's data slot
void mmu_switch(struct task *t);       // T is about to run here: its space
void mmu_task_done(struct task *t);    // T is gone: its space too
// Could the running task use the N bytes at P (to write, if W)? And the
// string at S: its length, or -1. Always yes without the MMU.
int  uaccess(int p, int n, int w);
int  ustrlen(char *s);
int  uvecok(char **v, int n);          // N strings (N < 0: up to a null pointer) and their vector
char *ucopy(char *p, int n);           // N bytes at P, copied into the running task's memory

#endif // __C4IX_H
