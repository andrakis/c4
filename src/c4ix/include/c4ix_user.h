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
    SYS_DUP = 210, SYS_DUP2 = 211, SYS_PIPE = 212
};
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
int *ualloc(int bytes);
int  getpid();

int   ustrlen(char *s);
int   ustrcmp(char *a, char *b);
char *ustrcpy(char *dst, char *src);   // returns just past the terminator

int  uputchar(int c);
int  uputs(char *s);
int  uprintf(char *fmt, ...);
int  ufprintf(int fd, char *fmt, ...);

#endif // __C4IX_USER_H
