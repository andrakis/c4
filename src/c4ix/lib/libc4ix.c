//
// libc4ix: the C4IX userland C library, linked into every user
// program as a .c4l archive.
//
// Every service goes through one gate, __c4ix_syscall, which picks
// its door once: on c4m it executes a custom opcode c4m does not
// implement, raising TRAP_ILLOP into the kernel -- the real syscall
// path, and the one that works from behind protected mode. On plain
// c4, where no trap machinery exists, it calls the kernel dispatcher
// through the systable slot the loader filled in at load time.
//
// printf lives here as a formatter over write(fd). Userland never
// executes PRTF; the kernel decides where fd 1 goes.
//
// This is one translation unit on purpose: the va area below is
// static, and statics never merge across objects, so make_va and its
// consumers must share a unit (the lesson from the kernel's va.c).
//

#include "c4ix_user.h"

int __c4ix_systable;      // filled by the kernel loader on plain c4
static int uva_use_trap;  // 1 = trap gateway, 0 = direct call

// ---- the gate ----

static int __c4ix_syscall(int num, int a, int b, int c) {
    int args[3];
    int f;

    if (uva_use_trap) {
        // __c4_opcode pushes left to right and OPCD reads the opcode
        // number from *sp, so the arguments are listed in REVERSE --
        // then sp[1] is the first argument, which is the shape
        // sched_trap hands to sys_dispatch. (u0.h's wrappers do the
        // same thing for the same reason.)
        return __c4_opcode(c, b, a, num);
    }
    args[0] = a; args[1] = b; args[2] = c;
    f = __c4ix_systable;
    return f(num, args);
}

int write(int fd, char *buf, int len) { return __c4ix_syscall(SYS_WRITE, fd, (int)buf, len); }
int read(int fd, char *buf, int len)  { return __c4ix_syscall(SYS_READ, fd, (int)buf, len); }
int uopen(char *path, int flags)      { return __c4ix_syscall(SYS_OPEN, (int)path, flags, 0); }
int uclose(int fd)                    { return __c4ix_syscall(SYS_CLOSE, fd, 0, 0); }
int uexit(int code)                   { return __c4ix_syscall(SYS_EXIT, code, 0, 0); }
int uyield()                          { return __c4ix_syscall(SYS_YIELD, 0, 0, 0); }
int uwait(int id)                     { return __c4ix_syscall(SYS_WAIT, id, 0, 0); }
int getpid()                          { return __c4ix_syscall(SYS_GETPID, 0, 0, 0); }
int *ualloc(int bytes)                { return (int *)__c4ix_syscall(SYS_SBRK, bytes, 0, 0); }

int spawn(char *path, int argc, char **argv) {
    return __c4ix_syscall(SYS_SPAWN, (int)path, argc, (int)argv);
}

// ---- varargs, private to this image ----

enum { UVA_STACK = 128 };
static int *uva_stack;
static int  uva_vptr;
static int *uva_m_ptr;
static int *uva_m_arg;
static int  uva_m_n;

int *__c4cc_make_va(int count) {
    uva_m_n   = count;
    uva_m_arg = &count + count;
    uva_m_ptr = &uva_stack[uva_vptr];

    uva_stack[uva_vptr] = count; ++uva_vptr;
    while (uva_m_n--) {
        uva_stack[uva_vptr] = *uva_m_arg; ++uva_vptr;
        --uva_m_arg;
    }
    return uva_m_ptr;
}

void __c4ix_uva_adj(int n) {
    uva_vptr = uva_vptr - n;
}

// ---- stdio over write() ----

int uputchar(int c) {
    char b[2];
    b[0] = c;
    write(STDOUT, b, 1);
    return c;
}

static int ustrlen(char *s) {
    int n;
    n = 0;
    while (s[n]) ++n;
    return n;
}

int uputs(char *s) {
    if (!s) s = "(null)";
    return write(STDOUT, s, ustrlen(s));
}

// Formatting accumulates into this buffer and flushes as one write:
// each character must NOT cost a syscall -- through the trap gateway
// that would be a kernel round trip per letter. Static is fine: one
// task per image.
enum { UFMT_BUF = 256 };
static char ufmt_buf[UFMT_BUF];
static int  ufmt_n;
static int  ufmt_fd;

static void uflush() {
    if (ufmt_n) write(ufmt_fd, ufmt_buf, ufmt_n);
    ufmt_n = 0;
}

static void uputb(int c) {
    ufmt_buf[ufmt_n] = c; ++ufmt_n;
    if (ufmt_n == UFMT_BUF) uflush();
}

static void uputnum(int v, int base) {
    char buf[24];
    char *digits;
    int i;

    digits = "0123456789abcdef";
    i = 0;
    if (v < 0 && base == 10) { uputb('-'); v = -v; }
    if (v == 0) { buf[i] = '0'; ++i; }
    while (v) {
        buf[i] = digits[v - (v / base) * base]; ++i;
        v = v / base;
    }
    while (i) { --i; uputb(buf[i]); }
}

// %d %x %s %c %% -- the same subset the kernel's kprintf and its
// PRTF emulation accept, so a program's output looks identical
// whichever path it takes.
//
// va_end must see the va_list exactly where va_start left it (it
// re-reads the count slot), so the format loop walks a copy.
// Returns characters written.
static int uformat(int fd, char *fmt, int *walk) {
    char *s;
    int n, c;

    ufmt_fd = fd;
    n = 0;
    while (*fmt) {
        c = *fmt; ++fmt;
        if (c != '%') { uputb(c); ++n; }
        else if (*fmt == 0) { uputb('%'); ++n; }
        else {
            c = *fmt; ++fmt;
            if (c == 'd')      uputnum(va_arg(walk, int), 10);
            else if (c == 'x') uputnum(va_arg(walk, int), 16);
            else if (c == 's') {
                s = va_arg(walk, char *);
                if (!s) s = "(null)";
                while (*s) { uputb(*s); ++s; ++n; }
            }
            else if (c == 'c') { uputb(va_arg(walk, int)); ++n; }
            else if (c == '%') { uputb('%'); ++n; }
            else { uputb('%'); uputb(c); n = n + 2; }
        }
    }
    uflush();
    return n;
}

int uprintf(char *fmt, ...) {
    va_list ap;
    va_list walk;
    int n;

    va_start(ap, fmt);
    walk = ap;
    n = uformat(STDOUT, fmt, walk);
    va_end(ap);
    return n;
}

int ufprintf(int fd, char *fmt, ...) {
    va_list ap;
    va_list walk;
    int n;

    va_start(ap, fmt);
    walk = ap;
    n = uformat(fd, fmt, walk);
    va_end(ap);
    return n;
}

// ---- startup ----
//
// Runs before main (the loader executes constructors at load time,
// after it has injected the systable). Picking the door here means
// the stubs never branch on host detection again.
static void __attribute__((constructor)) libc4ix_init() {
    uva_stack = malloc(sizeof(int) * UVA_STACK);
    memset(uva_stack, 0, sizeof(int) * UVA_STACK);
    uva_vptr = 0;
    // A host with no systable must have trap machinery; a host with
    // one is plain c4, where traps do not exist.
    uva_use_trap = !__c4ix_systable;
}
