//
// libc4ix: the C4IX userland C library.
//
// Programs include this and link against libc4ix.c4l. Nothing here
// touches a host opcode: printf is a library over write(fd), and
// write is a syscall. That is the whole point of the boundary --
// the kernel decides where fd 1 goes, so redirection is possible.
//

#ifndef __C4IX_USER_H
#define __C4IX_USER_H 1

// Syscall numbers, shared with the kernel (src/c4ix/c4ix.h).
enum {
    SYS_WRITE = 200, SYS_READ = 201, SYS_OPEN = 202, SYS_CLOSE = 203,
    SYS_EXIT = 204, SYS_YIELD = 205, SYS_SPAWN = 206, SYS_WAIT = 207,
    SYS_SBRK = 208, SYS_GETPID = 209,
    SYS_DUP = 210, SYS_DUP2 = 211, SYS_PIPE = 212,
    SYS_CYCLES = 213, SYS_TASKINFO = 214,
    SYS_CHDIR = 215, SYS_MKDIR = 216, SYS_GETCWD = 217, SYS_READDIR = 218,
    SYS_KILL = 219, SYS_SLEEP = 220,
    SYS_AVAIL = 221, SYS_INTR = 222, SYS_CLOEXEC = 223,
    SYS_STAT = 224, SYS_UNLINK = 225, SYS_RENAME = 226, SYS_LAZYFILE = 227,
    SYS_STACK = 228, SYS_CPUSTAT = 229, SYS_EVWAIT = 230, SYS_DISCARD = 231,
    SYS_VFORK = 232, SYS_EXECVE = 233, SYS_WAITPID = 234, SYS_LSEEK = 235,
    SYS_FSTAT = 236, SYS_LINK = 237, SYS_FTRUNCATE = 238, SYS_ENVIRON = 239
};
// What ustat2 and ufstat fill, a word each (the kernel's ST_* record)
enum { ST_TYPE = 0, ST_SIZE = 1, ST_PROG = 2, ST_MTIME = 3, ST_INO = 4,
       ST_NLINK = 5, ST_MODE = 6, ST_DEV = 7, ST_WORDS = 8 };
enum { UWNOHANG = 1 };
// taskinfo record, in order: id, parent, state, privs, nsyscalls,
// ntraps, cycles, then a 16-byte name
// One task's worth of what utaskinfo reports: eight integers, then the
// name as TASK_NAME_MAX bytes. Sized generously and in WORDS, because
// how many words sixteen bytes is depends on the machine -- at 9 this
// was two words short on a 32-bit host and utaskinfo wrote past the
// caller's array.
enum { TASKINFO_WORDS = 24 };
enum { TS_READY = 1, TS_RUNNING = 2, TS_ZOMBIE = 3, TS_WAITING = 4, TS_BLOCKED = 5, TS_SLEEPING = 6, TS_VFORK = 7 };
enum { STDIN = 0, STDOUT = 1, STDERR = 2 };
// open() flags; the low two bits match the host's.
enum {
    O_RD = 0, O_WR = 1, O_RDWR = 2,
    O_CREATE = 256, O_TRUNCATE = 512
};

// varargs, libc4ix's own copy (the kernel's lives in its own image)
#define va_list int *
#define va_arg(AP, TYPE)   (AP = AP + 1, *((TYPE *) (AP - 1)))
#define va_start(AP, LAST) (AP = *(&LAST - 1), va_arg(AP, int))
#define va_end(AP)         (AP = AP - 1, __c4ix_uva_adj(1 + va_arg(AP, int)))
int *__c4cc_make_va(int count);
void __c4ix_uva_adj(int n);

// NOTE the u-prefixes. A prototype cannot shadow a c4lc builtin:
// "read", "open", "close", "printf", "malloc", "free" and "exit" are
// OPCODES, and a call to one compiles to the opcode no matter what
// you declared. Naming these uread/uopen/... is what makes them
// syscalls instead. ("write" needs no prefix -- the VM has no such
// opcode, which is the whole reason the kernel must provide it.)
int  write(int fd, char *buf, int len);
int  uread(int fd, char *buf, int len);
int  uopen(char *path, int flags);
int  uclose(int fd);
int  uexit(int code);
int  uyield();
int  spawn(char *path, int argc, char **argv);
int  uwait(int id);
int  udup(int fd);
int  udup2(int oldfd, int newfd);
int  upipe(int *fds);            // fds[0] read end, fds[1] write end
int  ucycles();                  // VM cycles executed since boot
int  utaskinfo(int index, int *out);   // 1 if that task slot exists
int  ucpustat(int *hdr, int *recs, int max);  // cpustat.h: one snapshot of every task
// Sleep ms milliseconds, or less: until an event lands in the GUI ring at
// ring (0: none), or, with pipes set, data or end of file on a pipe this
// task reads. 1 if one already had.
int  uevwait(int ms, int *ring, int pipes);
int  uchdir(char *path);
int  umkdir(char *path);
int  ugetcwd(char *buf, int len);
int  ureaddir(char *path, int index, char *name);   // 1 dir, 0 file, -1 end
int  ukill(int pid, int sig);   // SIGTERM/SIGINT/SIGKILL end a task
int  umsleep(int ms);            // sleep; other tasks run meanwhile
int  uavail(int fd);             // bytes waiting; 0 empty (never blocks), -1 end of file
int  uintr(int pid);             // Ctrl-C for the job running under task pid
int  udiscard(int fd);           // drop what a pipe holds unread, as a tty's input on Ctrl-C
int  ucloexec(int fd, int on);   // 1: spawned children do not inherit fd
int  ustat(char *path, int *out); // out[0] 1 dir, 2 file, 3 other; out[1] bytes; out[2] 1 program;
                                  // out[3] mtime, an order of changes (0 = still on the host); -1 none
int  uunlink(char *path);         // remove a file, or an empty directory
int  urename(char *path, char *newname);  // a new name in the same directory
int  ulazyfile(char *path, char *host, int size); // a RAM file read from the host on first use
int  ustack(int bytes);           // stack size for tasks this one spawns from now on; 0 the default
// The POSIX process and file calls (docs/busybox-port.md §4). vfork's
// child shares this task's memory and must leave by uexecve or uexit,
// never by returning from the function that called vfork; the parent
// waits until it has.
int  vfork();
int  uexecve(char *path, char **argv, char **envp);   // -1 if it would not load
int  uwaitpid(int pid, int *st, int opts);  // st[0] exit code, st[1] signal; 0 none yet with UWNOHANG
int  ulseek(int fd, int off, int whence);  // RAM files only; the new position
int  ustat2(char *path, int *out, int n);  // the first n words of ST_*
int  ufstat(int fd, int *out, int n);
int  ulink(char *path, char *newpath);     // a second name for a file
int  uftruncate(int fd, int len);
char **uenviron();                         // the environment uexecve gave this task, or 0
int *ualloc(int bytes);
int  getpid();

int   ustrlen(char *s);
int   ustrcmp(char *a, char *b);
char *ustrcpy(char *dst, char *src);   // returns just past the terminator

int  upadstr(char *s, int width);
int  upadhdr(char *s, int width);   // heading for a right-aligned column
int  upadnum(int v, int width);
int  upadcycles(int v, int width);
int  upadcycles2(int hi, int lo, int width);

int  uputchar(int c);
int  uputs(char *s);
int  uprintf(char *fmt, ...);
int  ufprintf(int fd, char *fmt, ...);

#endif // __C4IX_USER_H
