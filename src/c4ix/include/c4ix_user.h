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
    SYS_AVAIL = 221, SYS_INTR = 222, SYS_CLOEXEC = 223
};
// taskinfo record, in order: id, parent, state, privs, nsyscalls,
// ntraps, cycles, then a 16-byte name
// One task's worth of what utaskinfo reports: eight integers, then the
// name as TASK_NAME_MAX bytes. Sized generously and in WORDS, because
// how many words sixteen bytes is depends on the machine -- at 9 this
// was two words short on a 32-bit host and utaskinfo wrote past the
// caller's array.
enum { TASKINFO_WORDS = 24 };
enum { TS_READY = 1, TS_RUNNING = 2, TS_ZOMBIE = 3, TS_WAITING = 4, TS_BLOCKED = 5, TS_SLEEPING = 6 };
enum { STDIN = 0, STDOUT = 1, STDERR = 2 };
// open() flags; the low two bits match the host's.
enum {
    O_RD = 0, O_WR = 1, O_RDWR = 2,
    O_CREATE = 256, O_TRUNCATE = 512
};

// The kernel fills this in at load time when the host has no trap
// machinery (plain c4), so the stubs can call the dispatcher
// directly. Zero means "use the trap gateway".
extern int __c4ix_systable;

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
int  uchdir(char *path);
int  umkdir(char *path);
int  ugetcwd(char *buf, int len);
int  ureaddir(char *path, int index, char *name);   // 1 dir, 0 file, -1 end
int  ukill(int pid, int sig);   // SIGTERM/SIGINT/SIGKILL end a task
int  umsleep(int ms);            // sleep; other tasks run meanwhile
int  uavail(int fd);             // bytes waiting; 0 empty (never blocks), -1 end of file
int  uintr(int pid);             // Ctrl-C for the job running under task pid
int  ucloexec(int fd, int on);   // 1: spawned children do not inherit fd
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
