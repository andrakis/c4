// libc: stdio.h. libc.h has the interface.
//
// A FILE is a descriptor and a buffer. Output collects in the buffer and
// goes out a write() at a time: when it fills, at a newline on a line-
// buffered stream (stdout when it is a terminal... and C4IX's consoles
// and pipes are all terminals as far as this library can tell, so stdout
// is line-buffered), at fflush and fclose, and at exit. stderr is not
// buffered at all. Input reads a buffer's worth at a time.
//
// printf is the whole of C's integer and string formatting: flags
// - + space 0 #, a width and a precision (either may be *), the length
// modifiers hh h l ll z j t (all one word here), and the conversions
// d i u o x X c s p n %, and f F e E g G, whose digits are fp.c's: the
// exact ones, rounded once, as glibc prints them. stdio.c never says
// double itself -- the value goes to fp.c as its two words -- so that
// this unit does not need c4lc's float helpers.

#include "sys.h"
#include "libc.h"

FILE *stdin;
FILE *stdout;
FILE *stderr;

static FILE *file_new(int fd, int flags) {
    FILE *f;
    if (!(f = (FILE *)malloc(sizeof(FILE)))) return 0;
    f->fd = fd;
    f->flags = flags;
    f->buf = 0;
    f->pos = 0;
    f->len = 0;
    f->ungot = -1;
    return f;
}

// stdin, stdout and stderr exist before main does
void __attribute__((constructor)) __stdio_init() {
    stdin = file_new(0, __F_READ);
    stdout = file_new(1, __F_WRITE | __F_LINE);
    stderr = file_new(2, __F_WRITE | __F_UNBUF);
}

static int file_buf(FILE *f) {
    if (f->buf) return 1;
    if (!(f->buf = malloc(BUFSIZ))) { f->flags = f->flags | __F_ERR; return 0; }
    f->flags = f->flags | __F_OWNBUF;
    return 1;
}

int fflush(FILE *f) {
    int n, w;
    if (!f) { fflush(stdout); fflush(stderr); return 0; }
    if (!(f->flags & __F_WRITE) || f->pos == 0) return 0;
    n = 0;
    while (n < f->pos) {
        w = write(f->fd, f->buf + n, f->pos - n);
        if (w <= 0) { f->flags = f->flags | __F_ERR; f->pos = 0; errno = EIO; return EOF; }
        n = n + w;
    }
    f->pos = 0;
    return 0;
}

int fputc(int c, FILE *f) {
    char one[2];
    if (!(f->flags & __F_WRITE)) { f->flags = f->flags | __F_ERR; errno = EBADF; return EOF; }
    if (f->flags & __F_UNBUF) {
        one[0] = c;
        if (write(f->fd, one, 1) != 1) { f->flags = f->flags | __F_ERR; return EOF; }
        return c & 255;
    }
    if (!file_buf(f)) return EOF;
    f->buf[f->pos++] = c;
    if (f->pos == BUFSIZ || ((f->flags & __F_LINE) && c == 10)) {
        if (fflush(f) == EOF) return EOF;
    }
    return c & 255;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

static int file_write(FILE *f, char *s, int n) {
    int i;
    if ((f->flags & __F_UNBUF) && n > 0) {
        if (write(f->fd, s, n) != n) { f->flags = f->flags | __F_ERR; return EOF; }
        return n;
    }
    i = 0;
    while (i < n) { if (fputc(s[i], f) == EOF) return EOF; ++i; }
    return n;
}

int fputs(char *s, FILE *f) { return file_write(f, s, strlen(s)) == EOF ? EOF : 1; }

int puts(char *s) {
    if (file_write(stdout, s, strlen(s)) == EOF) return EOF;
    return fputc(10, stdout) == EOF ? EOF : 1;
}

int fwrite(char *p, int size, int n, FILE *f) {
    if (size <= 0 || n <= 0) return 0;
    if (file_write(f, p, size * n) == EOF) return 0;
    return n;
}

int fgetc(FILE *f) {
    int c;
    if (f->ungot >= 0) { c = f->ungot; f->ungot = -1; return c; }
    if (!(f->flags & __F_READ)) { f->flags = f->flags | __F_ERR; errno = EBADF; return EOF; }
    if (f->flags & __F_EOF) return EOF;
    // reading the terminal: what was written to it first goes out first
    if (f == stdin) fflush(stdout);
    if (f->pos >= f->len) {
        if (!file_buf(f)) return EOF;
        f->len = uread(f->fd, f->buf, BUFSIZ);
        f->pos = 0;
        if (f->len <= 0) {
            if (f->len < 0) f->flags = f->flags | __F_ERR;
            f->flags = f->flags | __F_EOF;
            f->len = 0;
            return EOF;
        }
    }
    return f->buf[f->pos++] & 255;
}

int getc(FILE *f) { return fgetc(f); }
int getchar() { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
    if (c == EOF || f->ungot >= 0) return EOF;
    f->ungot = c & 255;
    f->flags = f->flags & ~__F_EOF;
    return c & 255;
}

char *fgets(char *s, int n, FILE *f) {
    int i, c;
    if (n <= 0) return 0;
    i = 0;
    while (i < n - 1) {
        c = fgetc(f);
        if (c == EOF) break;
        s[i++] = c;
        if (c == 10) break;
    }
    s[i] = 0;
    return i ? s : 0;
}

// a line of any length into *LINE (grown as needed); its length, or -1
int getline(char **line, int *cap, FILE *f) {
    int n, c;
    char *nb;
    if (!*line || *cap <= 0) { *cap = 128; if (!(*line = malloc(*cap))) return -1; }
    n = 0;
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > *cap) {
            if (!(nb = realloc(*line, *cap * 2))) return -1;
            *line = nb;
            *cap = *cap * 2;
        }
        (*line)[n++] = c;
        if (c == 10) break;
    }
    (*line)[n] = 0;
    return n ? n : -1;
}

int fread(char *p, int size, int n, FILE *f) {
    int total, got, c;
    if (size <= 0 || n <= 0) return 0;
    total = size * n;
    got = 0;
    while (got < total && (c = fgetc(f)) != EOF) p[got++] = c;
    return got / size;
}

int feof(FILE *f) { return (f->flags & __F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & __F_ERR) != 0; }
void clearerr(FILE *f) { f->flags = f->flags & ~(__F_EOF | __F_ERR); }
int fileno(FILE *f) { return f->fd; }

int setvbuf(FILE *f, char *buf, int mode, int size) {
    f->flags = f->flags & ~(__F_LINE | __F_UNBUF);
    if (mode == _IONBF) f->flags = f->flags | __F_UNBUF;
    else if (mode == _IOLBF) f->flags = f->flags | __F_LINE;
    return 0;
}

// "r" "w" "a" "r+" "w+" "a+", with or without "b"
static int mode_flags(char *mode, int *fflags) {
    int o, plus;
    plus = strchr(mode, '+') != 0;
    if (mode[0] == 'r') { o = plus ? O_RDWR : O_RDONLY; *fflags = plus ? __F_READ | __F_WRITE : __F_READ; }
    else if (mode[0] == 'w') { o = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; *fflags = plus ? __F_READ | __F_WRITE : __F_WRITE; }
    else if (mode[0] == 'a') { o = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; *fflags = plus ? __F_READ | __F_WRITE : __F_WRITE; }
    else { errno = EINVAL; return -1; }
    return o;
}

FILE *fopen(char *path, char *mode) {
    int o, fd, ff;
    FILE *f;
    if ((o = mode_flags(mode, &ff)) < 0) return 0;
    if ((fd = open(path, o, 438)) < 0) return 0;
    if (!(f = file_new(fd, ff))) { close(fd); errno = ENOMEM; return 0; }
    return f;
}

FILE *fdopen(int fd, char *mode) {
    int ff;
    if (mode_flags(mode, &ff) < 0) return 0;
    return file_new(fd, ff);
}

int fclose(FILE *f) {
    int r;
    if (!f) return EOF;
    r = fflush(f);
    if (close(f->fd) < 0) r = EOF;
    if (f->flags & __F_OWNBUF) free(f->buf);
    if (f != stdin && f != stdout && f != stderr) free((char *)f);
    return r;
}

// ---- printf ----

// Where formatted output goes: a FILE, or a buffer of CAP bytes.
struct sink {
    FILE *f;
    char *s;
    int cap;
    int n;              // characters produced, whether or not they fit
};

static void sink_put(struct sink *k, int c) {
    if (k->f) fputc(c, k->f);
    else if (k->n < k->cap - 1) k->s[k->n] = c;
    ++k->n;
}

static void sink_pad(struct sink *k, int c, int n) { while (n > 0) { sink_put(k, c); --n; } }

// fp.c
int __c4ix_fmtfpw(char *o, unsigned int hi, unsigned int lo, int conv, int prec, int alt, int plus, int space);
extern int __c4ix_fpspecial;

static char *conv_digits = "0123456789abcdef";
static char *conv_DIGITS = "0123456789ABCDEF";

// one integer conversion
static void fmt_int(struct sink *k, unsigned int v, int neg, int base, int upper, int left, int zero,
                    int plus, int space, int alt, int width, int prec) {
    char tmp[40];
    char *dig;
    int n, len, pad, signch, prefix;
    dig = upper ? conv_DIGITS : conv_digits;
    n = 0;
    while (v) { tmp[n++] = dig[v % base]; v = v / base; }
    if (prec < 0) prec = 1;
    len = n > prec ? n : prec;
    signch = neg ? '-' : plus ? '+' : space ? ' ' : 0;
    prefix = 0;
    if (alt && base == 16 && n > 0) prefix = 2;
    if (alt && base == 8 && len <= n) { len = n + 1; }
    pad = width - len - prefix - (signch ? 1 : 0);
    if (!left && !(zero && prec == 1)) sink_pad(k, ' ', pad);
    if (signch) sink_put(k, signch);
    if (prefix) { sink_put(k, '0'); sink_put(k, upper ? 'X' : 'x'); }
    if (!left && zero && prec == 1) sink_pad(k, '0', pad);
    sink_pad(k, '0', len - n);
    while (n > 0) { --n; sink_put(k, tmp[n]); }
    if (left) sink_pad(k, ' ', pad);
}

// one floating-point conversion: fp.c writes the sign and the number,
// and the padding is done here -- zeros go after the sign, and never
// pad an inf or a nan. The longest is %.1000f of 1e308: 1311 bytes.
static char fmt_fpbuf[1400];
static void fmt_fp(struct sink *k, unsigned int hi, unsigned int lo, int conv, int left, int zero,
                   int plus, int space, int alt, int width, int prec) {
    int n, i;
    if (prec > 1000) prec = 1000;
    n = __c4ix_fmtfpw(fmt_fpbuf, hi, lo, conv, prec, alt, plus, space);
    if (__c4ix_fpspecial) zero = 0;
    i = 0;
    if (!left && !zero) sink_pad(k, ' ', width - n);
    if (fmt_fpbuf[0] == '-' || fmt_fpbuf[0] == '+' || fmt_fpbuf[0] == ' ') { sink_put(k, fmt_fpbuf[0]); i = 1; }
    if (!left && zero) sink_pad(k, '0', width - n);
    while (i < n) { sink_put(k, fmt_fpbuf[i]); ++i; }
    if (left) sink_pad(k, ' ', width - n);
}

static int fmt_core(struct sink *k, char *fmt, int *ap) {
    int left, zero, plus, space, alt, width, prec, c, v, n;
    unsigned int hi, lo;
    char *s;
    while ((c = *fmt++)) {
        if (c != '%') { sink_put(k, c); continue; }
        left = 0; zero = 0; plus = 0; space = 0; alt = 0; width = 0; prec = -1;
        while (1) {
            c = *fmt;
            if (c == '-') left = 1;
            else if (c == '0') zero = 1;
            else if (c == '+') plus = 1;
            else if (c == ' ') space = 1;
            else if (c == '#') alt = 1;
            else break;
            ++fmt;
        }
        if (*fmt == '*') { width = va_arg(ap, int); ++fmt; if (width < 0) { left = 1; width = -width; } }
        else while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); ++fmt; }
        if (*fmt == '.') {
            ++fmt;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); ++fmt; if (prec < 0) prec = -1; }
            else while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); ++fmt; }
        }
        // every length is one word on this machine
        while (*fmt == 'h' || *fmt == 'l' || *fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'L' || *fmt == 'q') ++fmt;
        c = *fmt++;
        if (c == 'd' || c == 'i') {
            v = va_arg(ap, int);
            if (v < 0) fmt_int(k, -(unsigned int)v, 1, 10, 0, left, zero, plus, space, 0, width, prec);
            else fmt_int(k, v, 0, 10, 0, left, zero, plus, space, 0, width, prec);
        } else if (c == 'u') fmt_int(k, va_arg(ap, unsigned int), 0, 10, 0, left, zero, 0, 0, 0, width, prec);
        else if (c == 'x' || c == 'X') fmt_int(k, va_arg(ap, unsigned int), 0, 16, c == 'X', left, zero, 0, 0, alt, width, prec);
        else if (c == 'o') fmt_int(k, va_arg(ap, unsigned int), 0, 8, 0, left, zero, 0, 0, alt, width, prec);
        else if (c == 'p') {
            // glibc's: %#x of the pointer, or (nil) -- what c4m's PRTF prints
            v = va_arg(ap, int);
            if (v) fmt_int(k, v, 0, 16, 0, left, zero, 0, 0, 1, width, prec);
            else {
                s = "(nil)";
                if (!left) sink_pad(k, ' ', width - 5);
                while (*s) { sink_put(k, *s); ++s; }
                if (left) sink_pad(k, ' ', width - 5);
            }
        }
        else if (c == 'c') {
            v = va_arg(ap, int);
            if (!left) sink_pad(k, ' ', width - 1);
            sink_put(k, v);
            if (left) sink_pad(k, ' ', width - 1);
        } else if (c == 's') {
            s = va_arg(ap, char *);
            if (!s) s = "(null)";
            n = prec >= 0 ? strnlen(s, prec) : strlen(s);
            if (!left) sink_pad(k, ' ', width - n);
            v = 0;
            while (v < n) { sink_put(k, s[v]); ++v; }
            if (left) sink_pad(k, ' ', width - n);
        } else if (c == 'n') *va_arg(ap, int *) = k->n;
        else if (c == '%') sink_put(k, '%');
        else if (c == 'f' || c == 'e' || c == 'g' || c == 'E' || c == 'G' || c == 'F') {
            // a double: two words on a 32-bit machine, the high one first
            if (sizeof(int) == 4) { hi = va_arg(ap, unsigned int); lo = va_arg(ap, unsigned int); }
            else { v = va_arg(ap, int); hi = (unsigned int)(v >> 32); lo = (unsigned int)v; }
            fmt_fp(k, hi, lo, c, left, zero, plus, space, alt, width, prec);
        }
        else if (c == 0) { --fmt; break; }
        else { sink_put(k, '%'); sink_put(k, c); }
    }
    return k->n;
}

int vfprintf(FILE *f, char *fmt, int *ap) {
    struct sink k;
    k.f = f; k.s = 0; k.cap = 0; k.n = 0;
    fmt_core(&k, fmt, ap);
    return ferror(f) ? -1 : k.n;
}

int vsnprintf(char *s, int n, char *fmt, int *ap) {
    struct sink k;
    k.f = 0; k.s = s; k.cap = n; k.n = 0;
    fmt_core(&k, fmt, ap);
    if (n > 0) s[k.n < n - 1 ? k.n : n - 1] = 0;
    return k.n;
}

int vsprintf(char *s, char *fmt, int *ap) { return vsnprintf(s, INT_MAX, fmt, ap); }
int vprintf(char *fmt, int *ap) { return vfprintf(stdout, fmt, ap); }

int printf(char *fmt, ...) {
    va_list ap;
    va_list walk;
    int n;
    va_start(ap, fmt);
    walk = ap;
    n = vfprintf(stdout, fmt, walk);
    va_end(ap);
    return n;
}

int fprintf(FILE *f, char *fmt, ...) {
    va_list ap;
    va_list walk;
    int n;
    va_start(ap, fmt);
    walk = ap;
    n = vfprintf(f, fmt, walk);
    va_end(ap);
    return n;
}

int sprintf(char *s, char *fmt, ...) {
    va_list ap;
    va_list walk;
    int n;
    va_start(ap, fmt);
    walk = ap;
    n = vsnprintf(s, INT_MAX, fmt, walk);
    va_end(ap);
    return n;
}

int snprintf(char *s, int cap, char *fmt, ...) {
    va_list ap;
    va_list walk;
    int n;
    va_start(ap, fmt);
    walk = ap;
    n = vsnprintf(s, cap, fmt, walk);
    va_end(ap);
    return n;
}

int dprintf(int fd, char *fmt, ...) {
    va_list ap;
    va_list walk;
    char buf[512];
    int n;
    va_start(ap, fmt);
    walk = ap;
    n = vsnprintf(buf, 512, fmt, walk);
    va_end(ap);
    write(fd, buf, n < 511 ? n : 511);
    return n;
}

void perror(char *s) {
    if (s && *s) fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else fprintf(stderr, "%s\n", strerror(errno));
}
