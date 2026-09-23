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

// Am I running under C4DOS? Same trick as the systable above, one rung
// down: C4DOS's loader scans every image for this symbol and writes
// its API table's address into it before it runs the constructors
// (inject_api, src/c4dos/c4dos.c), so a non-zero here means DOS and
// nothing else. Costs no opcode -- a plain global read -- which is the
// point, because the alternative is finding out via the first write(),
// and that is an OPCD into a kernel that is not there.
//
// Magic is ('C'<<16)+('4'<<8)+'D', matching dos_present() in
// include/c4dos.h and the copy in include/u0.h.
int *__c4dos_api;         // patched by the C4DOS loader; 0 when not under DOS
static int uva_use_trap;  // 1 = trap gateway, 0 = direct call
static int uva_ready;     // library set up? see libc4ix_ready()

static void libc4ix_ready();

// ---- the gate ----

static int __c4ix_syscall(int num, int a, int b, int c) {
    int args[3];
    int f;

    if (!uva_ready) libc4ix_ready();
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
int uread(int fd, char *buf, int len) { return __c4ix_syscall(SYS_READ, fd, (int)buf, len); }
int uopen(char *path, int flags)      { return __c4ix_syscall(SYS_OPEN, (int)path, flags, 0); }
int uclose(int fd)                    { return __c4ix_syscall(SYS_CLOSE, fd, 0, 0); }
int uexit(int code)                   { return __c4ix_syscall(SYS_EXIT, code, 0, 0); }
int uyield()                          { return __c4ix_syscall(SYS_YIELD, 0, 0, 0); }
int uwait(int id)                     { return __c4ix_syscall(SYS_WAIT, id, 0, 0); }
int getpid()                          { return __c4ix_syscall(SYS_GETPID, 0, 0, 0); }
int *ualloc(int bytes)                { return (int *)__c4ix_syscall(SYS_SBRK, bytes, 0, 0); }

int udup(int fd)                      { return __c4ix_syscall(SYS_DUP, fd, 0, 0); }
int udup2(int oldfd, int newfd)       { return __c4ix_syscall(SYS_DUP2, oldfd, newfd, 0); }
int upipe(int *fds)                   { return __c4ix_syscall(SYS_PIPE, (int)fds, 0, 0); }
int ucycles()                         { return __c4ix_syscall(SYS_CYCLES, 0, 0, 0); }
int utaskinfo(int i, int *out)        { return __c4ix_syscall(SYS_TASKINFO, i, (int)out, 0); }
int uchdir(char *path)                { return __c4ix_syscall(SYS_CHDIR, (int)path, 0, 0); }
int umkdir(char *path)                { return __c4ix_syscall(SYS_MKDIR, (int)path, 0, 0); }
int ugetcwd(char *buf, int len)       { return __c4ix_syscall(SYS_GETCWD, (int)buf, len, 0); }
int ureaddir(char *path, int i, char *nm) { return __c4ix_syscall(SYS_READDIR, (int)path, i, (int)nm); }
int ukill(int pid, int sig)           { return __c4ix_syscall(SYS_KILL, pid, sig, 0); }
int umsleep(int ms)                   { return __c4ix_syscall(SYS_SLEEP, ms, 0, 0); }
int uavail(int fd)                    { return __c4ix_syscall(SYS_AVAIL, fd, 0, 0); }
int uintr(int pid)                    { return __c4ix_syscall(SYS_INTR, pid, 0, 0); }
int ucloexec(int fd, int on)          { return __c4ix_syscall(SYS_CLOEXEC, fd, on, 0); }
int ustat(char *path, int *out)       { return __c4ix_syscall(SYS_STAT, (int)path, (int)out, 0); }
int uunlink(char *path)               { return __c4ix_syscall(SYS_UNLINK, (int)path, 0, 0); }
int urename(char *path, char *nn)     { return __c4ix_syscall(SYS_RENAME, (int)path, (int)nn, 0); }

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
    // &count is a fixed frame slot, so the call below cannot disturb
    // the argument walk that follows it.
    if (!uva_ready) libc4ix_ready();
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

// ---- column output ----
//
// libc4ix's printf takes no width specifiers -- a deliberate limit --
// so anything that wants columns pads explicitly. ps and top both do,
// and both want it to look the same.
// These build into the shared format buffer and flush once per
// call. Writing a character at a time would be one kernel round trip
// per character -- fine on a host, ruinous through a trap gateway.
static void upad_emit(char *s, int len, int width, int right) {
    char line[64];
    int n, i;

    n = 0;
    if (right) { while (n + len < width) { line[n] = ' '; ++n; } }
    i = 0;
    while (i < len && n < 63) { line[n] = s[i]; ++n; ++i; }
    if (!right) { while (n < width && n < 63) { line[n] = ' '; ++n; } }
    write(STDOUT, line, n);
}

// A column HEADING for a right-aligned numeric column: the label
// right-aligned in WIDTH, then the same trailing space upadnum and
// upadcycles emit. Headings built with this cannot drift out of step
// with their data, because both are given the same width.
int upadhdr(char *s, int width) {
    char line[64];
    int n, len, i;

    len = ustrlen(s);
    n = 0;
    while (n + len < width && n < 63) { line[n] = ' '; ++n; }
    i = 0;
    while (i < len && n < 63) { line[n] = s[i]; ++n; ++i; }
    if (n < 63) { line[n] = ' '; ++n; }
    write(STDOUT, line, n);
    return n;
}

int upadstr(char *s, int width) {
    int n;
    n = ustrlen(s);
    upad_emit(s, n, width, 0);
    return (n > width) ? n : width;
}

// Right-aligned integer in WIDTH columns, followed by one space.
// digits of V into BUF, most significant first; returns the length
static int upad_digits(int v, char *buf) {
    char tmp[24];
    int n, i, neg;

    neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    n = 0;
    if (v == 0) { tmp[0] = '0'; n = 1; }
    while (v) { tmp[n] = '0' + v - (v / 10) * 10; ++n; v = v / 10; }
    if (neg) { tmp[n] = '-'; ++n; }
    i = 0;
    while (n) { --n; buf[i] = tmp[n]; ++i; }
    return i;
}

int upadnum(int v, int width) {
    char buf[24];
    int n;
    n = upad_digits(v, buf);
    buf[n] = ' ';
    upad_emit(buf, n + 1, width + 1, 1);
    return (n > width ? n : width) + 1;
}

// Cycle counts get large fast, so scale them the way C4KE's ps does.
//
// The two-word form: hi counts BILLIONS (sched.c's sched_add_cycles),
// so a value with a high word is already at least giga and is scaled
// from there. Nothing here is wider than an int, which is the point --
// one word overflowed at 2^31 and printed a negative number of cycles.
int upadcycles2(int hi, int lo, int width) {
    char buf[32];
    int whole, frac, n;
    char suffix;

    if (!hi) return upadcycles(lo, width);

    whole = hi;
    frac = lo / 1000000;                   // three decimals of the billions
    suffix = 'G';
    if (hi >= 1000000) { whole = hi / 1000000; frac = (hi / 1000) - whole * 1000; suffix = 'P'; }
    else if (hi >= 1000) { whole = hi / 1000; frac = hi - whole * 1000; suffix = 'T'; }

    n = upad_digits(whole, buf);
    buf[n] = '.'; ++n;
    buf[n] = '0' + frac / 100; ++n;
    buf[n] = '0' + (frac / 10) - (frac / 100) * 10; ++n;
    buf[n] = '0' + frac - (frac / 10) * 10; ++n;
    buf[n] = suffix; ++n;
    buf[n] = ' '; ++n;
    upad_emit(buf, n, width + 1, 1);
    return (n > width) ? n : width;
}

int upadcycles(int v, int width) {
    char buf[32];
    int whole, frac, n;

    whole = v;
    frac = -1;
    if (v >= 1000000) { whole = v / 1000000; frac = (v / 1000) - whole * 1000; }
    else if (v >= 1000) { whole = v / 1000; frac = v - whole * 1000; }

    n = upad_digits(whole, buf);
    if (frac >= 0) {
        buf[n] = '.'; ++n;
        buf[n] = '0' + frac / 100; ++n;
        buf[n] = '0' + (frac / 10) - (frac / 100) * 10; ++n;
        buf[n] = '0' + frac - (frac / 10) * 10; ++n;
        buf[n] = (v >= 1000000) ? 'M' : 'k'; ++n;
    }
    buf[n] = ' '; ++n;
    upad_emit(buf, n, width + 1, 1);
    return (n > width) ? n : width;
}

int uputchar(int c) {
    char b[2];
    b[0] = c;
    write(STDOUT, b, 1);
    return c;
}

int ustrlen(char *s) {
    int n;
    n = 0;
    while (s[n]) ++n;
    return n;
}

int ustrcmp(char *a, char *b) {
    int i;
    i = 0;
    while (a[i] && a[i] == b[i]) ++i;
    return a[i] - b[i];
}

// Copy src into dst and return the byte after the terminator, so
// callers can pack several strings into one buffer.
char *ustrcpy(char *dst, char *src) {
    int i;
    i = 0;
    while (src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
    return dst + i + 1;
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

static void upadch(int c, int n) {
    while (n > 0) { uputb(c); --n; }
}

// Digits of V in BASE, most significant first, into BUF; returns the
// length. Base 16 is UNSIGNED -- C4 has no unsigned type, so the
// shift is masked back to 60 bits rather than sign-extending.
static int udigits(char *buf, int v, int base) {
    char tmp[72];
    char *ds;
    int n, i, mask;

    ds = "0123456789abcdef";
    mask = (1 << 60) - 1;
    n = 0;
    if (!v) { tmp[0] = '0'; n = 1; }
    while (v) {
        if (base == 16) { tmp[n] = ds[v & 15]; v = (v >> 4) & mask; }
        else { tmp[n] = ds[v - (v / base) * base]; v = v / base; }
        ++n;
    }
    i = 0;
    while (n) { --n; buf[i] = tmp[n]; ++i; }
    return i;
}

// Flags '-' and '0' ('+', ' ', '#' accepted and ignored), a width or
// '*', a precision or '.*', the length modifiers l/ll/h/z (every
// integer here is one machine word), and d i u x X c s %.
//
// This is deliberately the SAME parser as the kernel's sys_vprintf,
// which services printf for programs that never heard of C4IX. A
// program's output must not depend on which path it took, so the two
// are tested against one format string -- keep them in step.
//
// An unrecognised conversion prints verbatim and consumes NOTHING,
// so the remaining arguments stay aligned.
//
// va_end must see the va_list exactly where va_start left it (it
// re-reads the count slot), so the format loop walks a copy.
// Returns characters written.
static int uformat(int fd, char *fmt, int *walk) {
    char num[80];
    char *s;
    int n, c, v, width, prec, left, zero, len, neg, i, base, upper;

    ufmt_fd = fd;
    n = 0;
    while (*fmt) {
        c = *fmt; ++fmt;
        if (c != '%') { uputb(c); ++n; continue; }
        if (!*fmt) { uputb('%'); ++n; break; }

        left = 0; zero = 0;
        while (1) {
            c = *fmt;
            if (c == '-') { left = 1; ++fmt; }
            else if (c == '0') { zero = 1; ++fmt; }
            else if (c == '+' || c == ' ' || c == '#') { ++fmt; }
            else break;
        }

        width = 0;
        if (*fmt == '*') {
            ++fmt;
            width = va_arg(walk, int);
            if (width < 0) { left = 1; width = -width; }
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0'); ++fmt;
            }
        }

        prec = -1;
        if (*fmt == '.') {
            ++fmt; prec = 0;
            if (*fmt == '*') {
                ++fmt;
                prec = va_arg(walk, int);
                if (prec < 0) prec = -1;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    prec = prec * 10 + (*fmt - '0'); ++fmt;
                }
            }
        }

        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') ++fmt;

        c = *fmt;
        if (!c) { uputb('%'); ++n; break; }
        ++fmt;

        if (c == '%') {
            if (!left) { upadch(' ', width - 1); n = n + width - 1; }
            uputb('%'); ++n;
            if (left) { upadch(' ', width - 1); n = n + width - 1; }
            continue;
        }

        if (c != 'd' && c != 'i' && c != 'u' && c != 'x' && c != 'X'
            && c != 'c' && c != 's') {
            uputb('%'); uputb(c); n = n + 2;
            continue;
        }

        if (c == 'c') {
            v = va_arg(walk, int);
            if (!left) { upadch(' ', width - 1); if (width > 1) n = n + width - 1; }
            uputb(v); ++n;
            if (left) { upadch(' ', width - 1); if (width > 1) n = n + width - 1; }
            continue;
        }

        if (c == 's') {
            s = va_arg(walk, char *);
            if (!s) s = "(null)";
            len = 0;
            while (s[len] && (prec < 0 || len < prec)) ++len;
            if (!left) { upadch(' ', width - len); if (width > len) n = n + width - len; }
            i = 0;
            while (i < len) { uputb(s[i]); ++i; ++n; }
            if (left) { upadch(' ', width - len); if (width > len) n = n + width - len; }
            continue;
        }

        v = va_arg(walk, int);
        upper = (c == 'X');
        base = (c == 'x' || c == 'X') ? 16 : 10;
        neg = 0;
        if (base == 10 && v < 0) { neg = 1; v = -v; }
        len = udigits(num, v, base);
        if (upper) {
            i = 0;
            while (i < len) {
                if (num[i] >= 'a' && num[i] <= 'f') num[i] = num[i] - 32;
                ++i;
            }
        }
        // an explicit precision is a minimum digit count and overrides
        // zero padding, as in C
        if (prec >= 0) {
            zero = 0;
            while (len < prec) {
                i = len;
                while (i) { num[i] = num[i - 1]; --i; }
                num[0] = '0';
                ++len;
            }
        }
        i = len + neg;
        if (!left && !zero) { upadch(' ', width - i); if (width > i) n = n + width - i; }
        if (neg) { uputb('-'); ++n; }
        if (!left && zero) { upadch('0', width - i); if (width > i) n = n + width - i; }
        i = 0;
        while (i < len) { uputb(num[i]); ++i; ++n; }
        if (left) { upadch(' ', width - len - neg); if (width > len + neg) n = n + width - len - neg; }
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
// Setup happens ON DEMAND, not only from the constructor, because a
// constructor cannot know it runs first. The program's OWN
// constructors are in the same list as this one, and if one of them
// makes a syscall before this has run, the gate reads uva_use_trap as
// 0 and calls through __c4ix_systable -- which is 0 on c4m, because
// the loader only injects it where traps do not exist. A call to
// address zero, from the first line of a program, for want of an
// ordering guarantee the C4R format does not actually give us (the
// priority field exists but load-c4r.c never reads it).
//
// So every entry point that needs the library checks. The constructor
// stays, because doing it once up front is still cheaper than the
// branch being taken later, and because it keeps the common case
// exactly as it was.
static void libc4ix_ready() {
    if (uva_ready) return;
    uva_ready = 1;
    uva_stack = malloc(sizeof(int) * UVA_STACK);
    memset(uva_stack, 0, sizeof(int) * UVA_STACK);
    uva_vptr = 0;
    // A host with no systable must have trap machinery; a host with
    // one is plain c4, where traps do not exist.
    uva_use_trap = !__c4ix_systable;
}

// Returns C4DOS_REFUSE to decline the program: C4DOS's run_program
// treats that one value as "do not run this", which is the only way a
// transient can decline (there is no exit that does not halt the
// machine). Without it a C4IX binary typed at the A> prompt trap-storms
// on its first write() and takes the session down with it.
static int __attribute__((constructor)) libc4ix_init() {
    // 1127037010 is C4DOS_REFUSE ('C','4','D','R'): a DISTINGUISHED
    // value, because a constructor with no return statement returns
    // whatever was in the accumulator. See src/c4dos/c4dos.c.
    if (__c4dos_api && __c4dos_api[0] == 4404292) {
        printf("C4DOS: This application requires C4IX.\n");
        return 1127037010;
    }
    libc4ix_ready();
    return 0;
}
