// bare.c -- libc's system layer (sys.h) with no C4IX under it: on the
// bare machine (libjs booting the program itself), or as a C4DOS transient.
//
// libc is built on libc4ix's syscall functions. A program linked
// statically with libc and this file instead runs where there is no
// kernel to call: what the machine does itself, it does with its own
// opcodes -- OPEN, READ and CLOS on its drives, PUTC to the console, USLP,
// EXIT -- and what it cannot, it says so (-1). Under C4DOS, which writes
// its API table into __c4dos_api when it loads the program (include/
// c4dos.h), files are written to DOS's RAM disk and read back from it, and
// exit returns to the A> prompt instead of stopping the machine.
//
// DOS's functions are called through the table as function pointers:
// c4dos.h's slot-and-stub calls are for c4cc, which has none.
//
// Compiled WITHOUT -fno-builtin, as opcodes.c is, so open, read, close,
// putchar and exit here are the opcodes. Everything here is the subset the
// whole machine family has: no file the machine itself can write, no
// directories but the one, no processes.

int *__c4dos_api;        // the loader's, under C4DOS; 0 on the bare machine

enum {
    DOS_MAGIC_SLOT = 0, DOS_EXIT = 1, DOS_CREATE = 2, DOS_WRITE = 3, DOS_CLOSE = 4,
    DOS_OPEN = 6, DOS_READ = 7, DOS_RCLOSE = 8, DOS_MAGIC = 4404292,   // include/c4dos.h's
    FD_MAX = 32,
    F_HOST = 1, F_DOSR = 2, F_DOSW = 3
};

// a descriptor past the console's three: what it is, and its handle
static int fd_kind[FD_MAX];
static int fd_h[FD_MAX];
static char *empty_env[1];

static int dos() { return __c4dos_api && __c4dos_api[DOS_MAGIC_SLOT] == DOS_MAGIC; }
static int dos1(int slot, int a) {
    int (*f)(int);
    if (!__c4dos_api[slot]) return 0 - 1;
    f = (int (*)(int))__c4dos_api[slot];
    return f(a);
}
static int dos3(int slot, int a, int b, int c) {
    int (*f)(int, int, int);
    if (!__c4dos_api[slot]) return 0 - 1;
    f = (int (*)(int, int, int))__c4dos_api[slot];
    return f(a, b, c);
}

static int fd_new(int kind, int h) {
    int fd;
    fd = 3;
    while (fd < FD_MAX && fd_kind[fd]) ++fd;
    if (fd == FD_MAX) return 0 - 1;
    fd_kind[fd] = kind;
    fd_h[fd] = h;
    return fd;
}

// open(2)'s flags as libc.h has them: the low two bits are the access
int uopen(char *path, int flags) {
    int h;
    if ((flags & 3) != 0) {
        // writing: only DOS's RAM disk takes it
        if (!dos() || (h = dos1(DOS_CREATE, (int)path)) < 0) return 0 - 1;
        return fd_new(F_DOSW, h);
    }
    // a file written this session is on the RAM disk; the rest on the drive
    if (dos() && __c4dos_api[DOS_OPEN] && (h = dos1(DOS_OPEN, (int)path)) >= 0) return fd_new(F_DOSR, h);
    if ((h = open(path, 0)) < 0) return 0 - 1;
    return fd_new(F_HOST, h);
}

int uread(int fd, char *buf, int len) {
    if (fd == 0) return read(0, buf, len);
    if (fd < 3 || fd >= FD_MAX || !fd_kind[fd]) return 0 - 1;
    if (fd_kind[fd] == F_HOST) return read(fd_h[fd], buf, len);
    if (fd_kind[fd] == F_DOSR) return dos3(DOS_READ, fd_h[fd], (int)buf, len);
    return 0 - 1;
}

int write(int fd, char *buf, int len) {
    int i;
    if (fd == 1 || fd == 2) {
        i = 0;
        while (i < len) { putchar(buf[i] & 255); ++i; }
        return len;
    }
    if (fd < 3 || fd >= FD_MAX || fd_kind[fd] != F_DOSW) return 0 - 1;
    return dos3(DOS_WRITE, fd_h[fd], (int)buf, len);
}

int uclose(int fd) {
    if (fd < 3 || fd >= FD_MAX || !fd_kind[fd]) return 0 - 1;
    if (fd_kind[fd] == F_HOST) close(fd_h[fd]);
    else if (fd_kind[fd] == F_DOSR) dos1(DOS_RCLOSE, fd_h[fd]);
    else dos1(DOS_CLOSE, fd_h[fd]);
    fd_kind[fd] = 0;
    return 0;
}

// exit: back to DOS's prompt, or the machine stops
int uexit(int code) {
    if (dos() && __c4dos_api[DOS_EXIT]) dos1(DOS_EXIT, code);
    exit(code);
    return 0;
}

// stat: a file is there if it opens, and its size is what it reads
int ustat2(char *path, int *out, int n) {
    char buf[4096];
    int fd, got, size, i;
    if ((fd = uopen(path, 0)) < 0) return 0 - 1;
    size = 0;
    while ((got = uread(fd, buf, 4096)) > 0) size = size + got;
    uclose(fd);
    i = 0;
    while (i < n) { out[i] = 0; ++i; }
    if (n > 0) out[0] = 2;               // a file (ST_TYPE)
    if (n > 1) out[1] = size;            // ST_SIZE
    if (n > 6) out[6] = 420;             // ST_MODE: 0644
    if (n > 5) out[5] = 1;               // ST_NLINK
    return 0;
}
int ustat(char *path, int *out) { return ustat2(path, out, 3); }
int ufstat(int fd, int *out, int n) {
    int i;
    if (fd < 0 || fd >= FD_MAX || (fd >= 3 && !fd_kind[fd])) return 0 - 1;
    i = 0;
    while (i < n) { out[i] = 0; ++i; }
    if (n > 0) out[0] = fd < 3 ? 3 : 2;  // the console, or a file
    return 0;
}

int umsleep(int ms) { if (ms > 0) __c4_usleep(ms * 1000); return 0; }
int ugetcwd(char *buf, int len) { if (len < 2) return 0 - 1; buf[0] = '/'; buf[1] = 0; return 1; }
int uchdir(char *path) { return (path[0] == '/' || path[0] == '.') && !path[1] ? 0 : 0 - 1; }
int getpid() { return 1; }
char **uenviron() { return empty_env; }

// what the machine has no way to do
int ureaddir(char *path, int index, char *name) { return 0 - 1; }
int umkdir(char *path) { return 0 - 1; }
int uunlink(char *path) { return 0 - 1; }
int urename(char *path, char *newname) { return 0 - 1; }
int udup(int fd) { return 0 - 1; }
int udup2(int oldfd, int newfd) { return 0 - 1; }
int upipe(int *fds) { return 0 - 1; }
int uexecve(char *path, char **argv, char **envp) { return 0 - 1; }
int uwaitpid(int pid, int *st, int opts) { return 0 - 1; }
int ulseek(int fd, int off, int whence) { return 0 - 1; }
int ulink(char *path, char *newpath) { return 0 - 1; }
int uftruncate(int fd, int len) { return 0 - 1; }
int vfork() { return 0 - 1; }
