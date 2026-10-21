// fptest.c -- C4IX libc's fp.c alone, compiled by c4lc, against what
// glibc's printf, strtod and strtof answer (fpconvgen.c's vectors):
// `fptest FILE` prints each line it gets wrong and a count.
int errno;
#include "../fp.c"

char *buf;
int pos, len;

unsigned int hexword() {
    unsigned int v;
    int c;
    while (pos < len && buf[pos] == ' ') ++pos;
    v = 0;
    while (pos < len) {
        c = buf[pos];
        if (c >= '0' && c <= '9') v = (v << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (c - 'a' + 10);
        else break;
        ++pos;
    }
    return v;
}
int decimal() {
    int v, neg;
    while (pos < len && buf[pos] == ' ') ++pos;
    neg = 0;
    if (buf[pos] == '-') { neg = 1; ++pos; }
    v = 0;
    while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') { v = v * 10 + (buf[pos] - '0'); ++pos; }
    return neg ? 0 - v : v;
}
// the rest of the line from pos, NUL-terminated in place
char *rest() {
    char *s;
    s = buf + pos;
    while (pos < len && buf[pos] != 10) ++pos;
    buf[pos] = 0;
    ++pos;
    return s;
}
int same(char *a, char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

int main(int argc, char **argv) {
    int fd, n, bad, cases, prec, alt, plus, space;
    unsigned int hi, lo, w;
    unsigned long long bits, got;
    char kind, conv, *want, *text, *flags, out[2000];
    double d;
    float f;
    if (argc < 2) { printf("usage: fptest FILE\n"); return 2; }
    fd = open(argv[1], 0);
    if (fd < 0) { printf("fptest: cannot open %s\n", argv[1]); return 2; }
    buf = malloc(4000000);
    len = 0;
    while (len < 4000000 - 65536 && (n = read(fd, buf + len, 65536)) > 0) len = len + n;
    if (len >= 4000000 - 65536) { printf("fptest: %s is too big\n", argv[1]); return 2; }
    close(fd);
    pos = 0; bad = 0; cases = 0;
    while (pos < len) {
        kind = buf[pos]; pos = pos + 2;
        if (kind == 'P') {
            conv = buf[pos]; pos = pos + 1;
            prec = decimal();
            ++pos;
            flags = buf + pos;
            alt = 0; plus = 0; space = 0;
            while (buf[pos] != ' ') {
                if (buf[pos] == 'h') alt = 1;
                if (buf[pos] == 'p') plus = 1;
                if (buf[pos] == 's') space = 1;
                ++pos;
            }
            hi = hexword(); lo = hexword();
            ++pos;                                   // the '|'
            want = rest();
            bits = ((unsigned long long)hi << 32) | lo;
            d = *(double *)&bits;
            __c4ix_fmtfp(out, d, conv, prec, alt, plus, space);
            ++cases;
            if (!same(out, want)) {
                if (bad < 25) printf("P %c %d %x %x: want [%s] got [%s]\n", conv, prec, hi, lo, want, out);
                ++bad;
            }
        } else if (kind == 'D') {
            hi = hexword(); lo = hexword();
            ++pos;
            text = rest();
            bits = ((unsigned long long)hi << 32) | lo;
            d = strtod(text, 0);
            got = *(unsigned long long *)&d;
            ++cases;
            if (got != bits) {
                if (bad < 25) printf("D [%s]: want %x %x got %x %x\n", text, hi, lo,
                    (unsigned int)(got >> 32), (unsigned int)got);
                ++bad;
            }
        } else if (kind == 'F') {
            w = hexword();
            ++pos;
            text = rest();
            f = strtof(text, 0);
            ++cases;
            if (*(unsigned int *)&f != w) {
                if (bad < 25) printf("F [%s]: want %x got %x\n", text, w, *(unsigned int *)&f);
                ++bad;
            }
        } else rest();
    }
    printf("fptest: %d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
