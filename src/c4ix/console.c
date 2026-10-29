//
// C4IX console: kputc, kputs, kprintf, klog, the kernel log, and
// console input.
//
// All kernel output funnels through kputc, which uses only the PUTC
// opcode -- implemented by c4m and plain c4 alike. A task's output
// (con_write_task) goes a run of bytes at a time through the host's
// printf("%s"), as one PRTF; nothing else in C4IX calls it. Userland
// printf is a library over write(fd), and this is the kernel-side sink.
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
// /log/kernel (below): the file, what waits for it, and whether tasks'
// output is still logged -- declared up here, ahead of con_init
enum { CON_PRE = 2048, CON_LOG_MAX = 262144 };
static struct vnode *con_log;
static char con_pre[CON_PRE];
static int  con_npre;
static int  con_booting;

// Called once at boot. If the reopen fails -- no /dev/stdin, or a
// host that will not give us one -- con_fd stays -1 and everything
// below falls back to a blocking read on fd 0, which is what C4IX
// did before. Losing responsiveness is better than losing input.
void con_init() {
    con_booting = 1;
    con_fd = open("/dev/stdin", CON_O_RDONLY | CON_O_NONBLOCK);
    con_len = 0;
    con_pos = 0;
    con_eof = 0;
    con_gate = 0;
    if (con_fd >= 0)
        klog("console: the host's terminal, read without blocking (fd %d), polled every %d cycles\n",
            con_fd, CON_POLL_CYCLES);
    else klog("console: the host's terminal, read on fd 0, which stops the machine while it waits\n");
}

// Booting: until init starts the shell (con_booted)
int con_is_booting() { return con_booting; }

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

// /log/kernel: what the console said. The kernel's own lines always;
// the tasks' only while booting (until init starts the shell), or top's
// redraws would fill it within minutes. Lines from before the VFS exists
// wait in con_pre. It stops at CON_LOG_MAX: a log, not a ring.

static void con_logc(int c) {
    if (con_log) vfs_logc(con_log, c, CON_LOG_MAX);
    else if (con_npre < CON_PRE) { con_pre[con_npre] = c; ++con_npre; }
}
// ---- the kernel log (dmesg, /proc/kmsg) ----
//
// Every line the kernel says -- kprintf's, kputs', klog's -- goes into a
// ring of KLOG_BYTES with the time it was said, Linux's way:
//
//     [    0.012345] sl4b: cache 'task', 376-byte objects, 16 a slab
//
// Once the ring is full the oldest lines go. A task's own output does
// not come here (that is /log/kernel's, while booting). klog's lines go
// only here, unless the kernel was booted with -v; kprintf's go to the
// console as well, as they always did.
//
// The stamp is the machine's clock, seconds and microseconds since the
// kernel first looked: TIME's milliseconds, and the cycles since the
// last millisecond began for the rest, at the rate klog_calibrate
// measured. Before that, and on a host whose clock does not move, the
// microseconds are the millisecond's.
enum { KLOG_BYTES = 65536 };
static char klog_ring[KLOG_BYTES];
static int  klog_n;            // bytes ever written: the ring holds the last KLOG_BYTES
static int  klog_mid;          // inside a line: the next byte gets no stamp
int  klog_verbose;             // -v: klog's lines on the console too
int  klog_cpms;                // cycles in a millisecond of the machine's clock, once measured
static int klog_t0, klog_started;   // __time() at the first stamp
static int klog_ms, klog_cyc;  // the last millisecond seen, and the cycle count then
static int kout_con = 1;       // where con_out sends a byte: the console too (klog clears it)

static void klog_put(int c) {
    klog_ring[klog_n & (KLOG_BYTES - 1)] = c;
    ++klog_n;
}

// "[SSSSS.UUUUUU] ", the digits of v in width w, zeros if z
static void klog_num(int v, int w, int z) {
    char d[12];
    int n;
    n = 0;
    d[n] = '0' + v % 10; ++n; v = v / 10;
    while (v) { d[n] = '0' + v % 10; ++n; v = v / 10; }
    while (n < w) { d[n] = z ? '0' : ' '; ++n; }
    while (n) { --n; klog_put(d[n]); }
}

static void klog_stamp() {
    int ms, cyc, d, us;
    ms = __time();
    cyc = __c4_cycles();
    if (!klog_started) { klog_started = 1; klog_t0 = ms; }
    if (ms != klog_ms) { klog_ms = ms; klog_cyc = cyc; }
    us = 0;
    if (klog_cpms > 0) {
        d = cyc - klog_cyc;
        if (d > klog_cpms) d = klog_cpms;
        if (d > 0) us = d * 1000 / klog_cpms;
        if (us > 999) us = 999;
    }
    ms = ms - klog_t0;
    if (ms < 0) ms = 0;
    klog_put('[');
    klog_num(ms / 1000, 5, 0);
    klog_put('.');
    klog_num(ms % 1000 * 1000 + us, 6, 1);
    klog_put(']');
    klog_put(' ');
}

static void klog_c(int c) {
    if (!klog_mid) { klog_stamp(); klog_mid = 1; }
    klog_put(c);
    if (c == '\n') klog_mid = 0;
}

// The rate the stamps need, and the cpu line's MHz: the cycles between
// two ticks of the machine's millisecond clock, over four of them. Gives
// up (0) if the clock does not move in a hundred million cycles.
int klog_calibrate() {
    int t, c0, c1, start;
    start = __c4_cycles();
    t = __time();
    while (__time() == t) if (__c4_cycles() - start > 100000000) return 0;
    c0 = __c4_cycles();
    t = __time();
    while (__time() - t < 4) if (__c4_cycles() - start > 100000000) return 0;
    c1 = __c4_cycles();
    klog_cpms = (c1 - c0) / 4;
    return klog_cpms;
}

// The log as it stands, oldest line first, into DST (KLOG_BYTES at least);
// its length. A line the ring has partly lost is left out.
int klog_copy(char *dst) {
    int n, start, k;
    sched_lock();
    n = klog_n;
    start = 0;
    if (n > KLOG_BYTES) {
        start = n - KLOG_BYTES;
        n = KLOG_BYTES;
        while (n && klog_ring[start & (KLOG_BYTES - 1)] != '\n') { ++start; --n; }
        if (n) { ++start; --n; }
    }
    k = KLOG_BYTES - (start & (KLOG_BYTES - 1));
    if (k > n) k = n;
    memcpy(dst, klog_ring + (start & (KLOG_BYTES - 1)), k);
    if (n > k) memcpy(dst + k, klog_ring, n - k);
    sched_unlock();
    return n;
}

static int con_out(int c) {
    if (kout_con) {
        putchar(c);
        con_logc(c);
    }
    klog_c(c);
    return c;
}

// The VFS is up: make /log/kernel and put what was said so far in it.
void con_logopen() {
    int i;
    if (!(con_log = vfs_klog())) return;
    i = 0;
    while (i < con_npre) { vfs_logc(con_log, con_pre[i], CON_LOG_MAX); ++i; }
    con_npre = 0;
}

// Boot is over: tasks' output is no longer logged.
void con_booted() { con_booting = 0; }

// A task's bytes to the console (vfs_write): up to CON_RUN at a time as
// one host printf("%s"), where putchar is one host call a byte. Under
// libjs's parallel CPUs a host call another CPU makes is a request to
// CPU 0, made holding the kernel lock: top on CPU 1 drew its screen a
// byte a request, and a game on CPU 0, which needs the lock for every
// syscall, stalled once a second. A NUL, which would end the string,
// goes by putchar.
enum { CON_RUN = 256 };
void con_write_task(char *buf, int len) {
    char run[CON_RUN + 1];
    int i, n;

    i = 0;
    while (i < len) {
        n = 0;
        while (i < len && n < CON_RUN && buf[i]) { run[n] = buf[i]; ++n; ++i; }
        if (n) { run[n] = 0; printf("%s", run); }
        if (i < len && !buf[i]) { putchar(0); ++i; }
    }
    if (con_booting) { i = 0; while (i < len) { con_logc(buf[i]); ++i; } }
}

int kputc(int c) {
    con_out(c);
    return c;
}

int kputs(char *s) {
    int n;
    n = 0;
    if (!s) s = "(null)";
    while (*s) { con_out(*s); ++s; ++n; }
    return n;
}

// Digits of v in base: decimal signed, with a minus sign; hex unsigned,
// as printf's %x (the shift's top nibble masked, as kputptr does, so a
// word above 0x80000000 is its digits, not a minus sign). WIDTH pads to
// that many characters: with zeros after the sign when ZERO, with spaces
// on the right when LEFT, else spaces on the left. Returns the character
// count. (Most-negative int wraps on negation; kernel prints don't go
// there.)
static int kputnum(int v, int base, int width, int zero, int left) {
    char buf[24];
    char *digits;
    int i, n, neg, mask;

    digits = "0123456789abcdef";
    i = 0;
    neg = 0;
    if (base == 16) {
        mask = ~(15 << (sizeof(int) * 8 - 4));
        if (v == 0) { buf[i] = '0'; ++i; }
        while (v) { buf[i] = digits[v & 15]; ++i; v = (v >> 4) & mask; }
    } else {
        if (v < 0) { neg = 1; v = -v; }
        if (v == 0) { buf[i] = '0'; ++i; }
        while (v) {
            buf[i] = digits[v % base]; ++i;
            v = v / base;
        }
    }
    n = i + neg;
    if (!left && !zero) while (n < width) { con_out(' '); ++n; }
    if (neg) con_out('-');
    if (!left && zero) while (n < width) { con_out('0'); ++n; }
    while (i) { --i; con_out(buf[i]); }
    if (left) while (n < width) { con_out(' '); ++n; }
    return n;
}

// s, padded to WIDTH with spaces, on the right when LEFT
static int kputpad(char *s, int width, int left) {
    int n, len;
    len = 0;
    while (s[len]) ++len;
    n = len;
    if (!left) while (n < width) { con_out(' '); ++n; }
    kputs(s);
    if (left) while (n < width) { con_out(' '); ++n; }
    return n;
}

// A pointer, as glibc's %p prints it and c4m's PRTF with it: 0x and
// its hex digits, unsigned, or (nil). The shift's top nibble is masked
// off so a pointer above 0x80000000 does not sign-extend forever.
static int kputptr(int v) {
    char buf[24];
    char *digits;
    int i, n, mask;

    if (!v) return kputs("(nil)");
    digits = "0123456789abcdef";
    mask = ~(15 << (sizeof(int) * 8 - 4));
    i = 0;
    while (v) { buf[i] = digits[v & 15]; ++i; v = (v >> 4) & mask; }
    con_out('0'); con_out('x');
    n = i + 2;
    while (i) { --i; con_out(buf[i]); }
    return n;
}

// %d %x %p %s %c %%, with printf's width and its 0 and - flags on %d,
// %x and %s: "0x%08x" is eight hex digits, zeros first; "%-12s" a name
// in twelve columns. Anything else prints verbatim. Returns characters
// written, printf-style.
//
// va_end must see ap exactly where va_start left it -- it re-reads
// the count slot from there to pop the va area. So the format loop
// walks a copy and ap itself never moves.
//
// The body runs under sched_lock: a line is atomic against
// preemption, so no cycle interrupt can interleave two tasks' lines.
static int kformat(char *fmt, int *walk) {
    int n, c, width, zero, left;

    n = 0;
    while (*fmt) {
        c = *fmt; ++fmt;
        if (c != '%') { con_out(c); ++n; }
        else if (*fmt == 0) { con_out('%'); ++n; }
        else {
            // flags and a width: %08x, %-12s
            zero = 0; left = 0; width = 0;
            while (*fmt == '0' || *fmt == '-') { if (*fmt == '0') zero = 1; else left = 1; ++fmt; }
            while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); ++fmt; }
            if (left) zero = 0;
            c = *fmt;
            if (c) ++fmt;
            if (c == 'd')      n = n + kputnum(va_arg(walk, int), 10, width, zero, left);
            else if (c == 'x') n = n + kputnum(va_arg(walk, int), 16, width, zero, left);
            else if (c == 'p') n = n + kputptr(va_arg(walk, int));
            else if (c == 's') n = n + kputpad(va_arg(walk, char *), width, left);
            else if (c == 'c') { con_out(va_arg(walk, int)); ++n; }
            else if (c == '%') { con_out('%'); ++n; }
            else if (c) { con_out('%'); con_out(c); n = n + 2; }
            else { con_out('%'); ++n; }
        }
    }
    return n;
}

int kprintf(char *fmt, ...) {
    va_list ap;    // one declarator per line: va_list is `int *`, so
    va_list walk;  // `va_list a, b` would make b a plain int
    int n;

    sched_lock();
    va_start(ap, fmt);
    walk = ap;
    n = kformat(fmt, walk);
    va_end(ap);
    sched_unlock();
    return n;
}

// kprintf's, into the kernel log only: on the console as well with -v
int klog(char *fmt, ...) {
    va_list ap;
    va_list walk;
    int n;

    sched_lock();
    va_start(ap, fmt);
    walk = ap;
    kout_con = klog_verbose;
    n = kformat(fmt, walk);
    kout_con = 1;
    va_end(ap);
    sched_unlock();
    return n;
}
