//
// C4IX console: kputc, kputs, kprintf.
//
// All kernel output funnels through kputc, which uses only the PUTC
// opcode -- implemented by c4m and plain c4 alike. Nothing in C4IX
// ever calls the host's printf; when syscalls arrive (X2), userland
// printf becomes a library over write(fd) and this stays the
// kernel-side sink.
//

#include "c4ix.h"

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
int kprintf(char *fmt, ...) {
    va_list ap;    // one declarator per line: va_list is `int *`, so
    va_list walk;  // `va_list a, b` would make b a plain int
    int n, c;

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
    return n;
}
