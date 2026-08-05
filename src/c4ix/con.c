//
// C4IX console: kputc, kputs, kprintf, and console input.
//
// All kernel output funnels through kputc, which uses only the PUTC
// opcode -- implemented by c4m and plain c4 alike. Nothing in C4IX
// ever calls the host's printf; when syscalls arrive (X2), userland
// printf becomes a library over write(fd) and this stays the
// kernel-side sink.
//
// Input does not go through fd 0. A read on fd 0 blocks the HOST,
// which means it blocks the whole virtual machine: every task stops
// while one of them waits for a keystroke, no signal is noticed, and
// the scheduler cannot run. Instead the kernel opens a second
// descriptor on the same terminal with O_NONBLOCK, and a read that
// finds nothing simply says so. c4sh does this too, and it works on
// both hosts -- measured on a delayed pipe, native c4m spun two
// million times without blocking, and the c4-hosted chain spun a
// quarter million before the data arrived.
//

#include "c4ix.h"

// Flag values as the host sees them. O_NONBLOCK is 0x800 on Linux;
// this is the same pair of constants c4sh defines for itself,
// because neither compiler has a header to get them from.
enum { CON_O_RDONLY = 0, CON_O_NONBLOCK = 0x800 };

enum { CON_BUF = 256 };

// Polling costs a host read() every time it is asked, and the
// scheduler asks -- once per blocked reader, at every scheduling
// decision. A compute-bound task traps thousands of times a second,
// so an ungated poll turns one shell sitting at a prompt into
// thousands of host syscalls a second, waiting on input that arrives
// at human speed. The gate allows at most one host read per
// CON_POLL_CYCLES of virtual time. Buffered bytes are always served
// first, so nothing already read is ever held back, and the worst
// case a reader can see is half a millisecond of extra latency.
enum { CON_POLL_CYCLES = 100000 };

static int  con_fd;             // the non-blocking descriptor, -1 = none
static char con_buf[CON_BUF];   // bytes read but not yet consumed
static int  con_len;
static int  con_pos;
static int  con_eof;
static int  con_gate;           // cycle count of the last host read

// Called once at boot. If the reopen fails -- no /dev/stdin, or a
// host that will not give us one -- con_fd stays -1 and everything
// below falls back to a blocking read on fd 0, which is what C4IX
// did before. Losing responsiveness is better than losing input.
void con_init() {
    con_fd = open("/dev/stdin", CON_O_RDONLY | CON_O_NONBLOCK);
    con_len = 0;
    con_pos = 0;
    con_eof = 0;
    con_gate = 0;
}

// Open the gate. Virtual cycles are the right clock while tasks are
// running and exactly the wrong one when the machine is asleep -- the
// counter stops during the idle nap, so a purely cycle-based gate
// would never reopen and a prompt with nothing else running would
// never see a keystroke. The idle loop calls this after each nap.
void con_wake() {
    con_gate = 0;
}

// 1 if a console read can be answered without stopping the machine.
// End of file counts: a read there returns 0 immediately.
//
// Polling has to do the reading, because a non-blocking read is the
// only way to ask "is there anything?" and it consumes what it finds.
// So what it finds is buffered here and con_read serves from that.
int con_poll() {
    int n, now;

    if (con_pos < con_len) return 1;
    if (con_eof) return 1;
    if (con_fd < 0) return 1;      // blocking fallback: pretend, then block

    now = __c4_cycles();
    if (con_gate && now - con_gate < CON_POLL_CYCLES) return 0;
    con_gate = now ? now : 1;      // 0 means "open", so never store it

    con_pos = 0;
    con_len = 0;
    n = read(con_fd, con_buf, CON_BUF);
    if (n > 0) { con_len = n; return 1; }
    if (n == 0) { con_eof = 1; return 1; }
    return 0;                      // nothing there yet
}

int con_read(char *buf, int len) {
    int n, i;

    if (con_fd < 0) return read(0, buf, len);
    if (con_pos >= con_len) {
        if (con_eof) return 0;
        if (!con_poll()) return 0;
        if (con_pos >= con_len) return 0;
    }
    n = con_len - con_pos;
    if (n > len) n = len;
    i = 0;
    while (i < n) { buf[i] = con_buf[con_pos + i]; ++i; }
    con_pos = con_pos + n;
    return n;
}

int kputc(int c) {
    putchar(c);
    return c;
}

int kputs(char *s) {
    int n;
    n = 0;
    if (!s) s = "(null)";
    while (*s) { putchar(*s); ++s; ++n; }
    return n;
}

// Digits of v in base, minus sign for negative decimal. Returns the
// character count. (Most-negative int wraps on negation; kernel
// prints don't go there.)
static int kputnum(int v, int base) {
    char buf[24];
    char *digits;
    int i, n;

    digits = "0123456789abcdef";
    i = 0;
    n = 0;
    if (v < 0) { putchar('-'); ++n; v = -v; }
    if (v == 0) { buf[i] = '0'; ++i; }
    while (v) {
        buf[i] = digits[v % base]; ++i;
        v = v / base;
    }
    n = n + i;
    while (i) { --i; putchar(buf[i]); }
    return n;
}

// %d %x %s %c %%. Anything else prints verbatim. Returns characters
// written, printf-style.
//
// va_end must see ap exactly where va_start left it -- it re-reads
// the count slot from there to pop the va area. So the format loop
// walks a copy and ap itself never moves.
//
// The body runs under sched_lock: a line is atomic against
// preemption, so no cycle interrupt can interleave two tasks' lines.
int kprintf(char *fmt, ...) {
    va_list ap;    // one declarator per line: va_list is `int *`, so
    va_list walk;  // `va_list a, b` would make b a plain int
    int n, c;

    sched_lock();
    va_start(ap, fmt);
    walk = ap;
    n = 0;
    while (*fmt) {
        c = *fmt; ++fmt;
        if (c != '%') { putchar(c); ++n; }
        else if (*fmt == 0) { putchar('%'); ++n; }
        else {
            c = *fmt; ++fmt;
            if (c == 'd')      n = n + kputnum(va_arg(walk, int), 10);
            else if (c == 'x') n = n + kputnum(va_arg(walk, int), 16);
            else if (c == 's') n = n + kputs(va_arg(walk, char *));
            else if (c == 'c') { putchar(va_arg(walk, int)); ++n; }
            else if (c == '%') { putchar('%'); ++n; }
            else { putchar('%'); putchar(c); n = n + 2; }
        }
    }
    va_end(ap);
    sched_unlock();
    return n;
}
