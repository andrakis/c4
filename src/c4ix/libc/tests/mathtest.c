// mathtest.c -- C4IX libc's math.c alone, compiled by c4lc, against
// glibc's libm (mathgen.c's vectors): each function's worst error in
// ulps, which must be within its limit -- 0 for the exact ones.
int errno;
#include "../math.c"

char *buf;
int pos, len;

unsigned int hexword() {
    unsigned int v;
    int c;
    while (pos < len && (buf[pos] == ' ' || buf[pos] == 10)) ++pos;
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
    int v;
    while (pos < len && (buf[pos] == ' ' || buf[pos] == 10)) ++pos;
    v = 0;
    while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') { v = v * 10 + (buf[pos] - '0'); ++pos; }
    return v;
}
double word2(unsigned long long *b) {
    unsigned int hi;
    hi = hexword();
    *b = ((unsigned long long)hi << 32) | hexword();
    return *(double *)b;
}
// how many representable doubles apart; huge for different signs or a NaN against a number
unsigned long long ulps(unsigned long long a, unsigned long long b) {
    int na, nb;
    na = ((a & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull);
    nb = ((b & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull);
    if (na || nb) return (na && nb) ? 0 : 0xFFFFFFFFFFFFull;
    if ((a >> 63) != (b >> 63)) return ((a << 1) == 0 && (b << 1) == 0) ? 1 : 0xFFFFFFFFFFFFull;
    return a > b ? a - b : b - a;
}

char *names[30];
int limit[30];
unsigned long long worst[30];
int count[30];
int shown;

int main(int argc, char **argv) {
    int fd, n, fn, bad, e, i;
    unsigned long long ba, bb, br, bg, u;
    double a, b, r, ip;
    names[1] = "floor"; names[2] = "ceil"; names[3] = "trunc"; names[4] = "round"; names[5] = "sqrt";
    names[6] = "frexp"; names[7] = "ldexp"; names[8] = "fmod"; names[9] = "modf"; names[10] = "exp";
    names[11] = "log"; names[12] = "log10"; names[13] = "log2"; names[14] = "sin"; names[15] = "cos";
    names[16] = "tan"; names[17] = "atan"; names[18] = "atan2"; names[19] = "asin"; names[20] = "acos";
    names[21] = "sinh"; names[22] = "cosh"; names[23] = "tanh"; names[24] = "pow(x,int)"; names[25] = "pow";
    names[26] = "hypot"; names[27] = "cbrt"; names[28] = "log1p"; names[29] = "expm1";
    // limits in ulps: what is measured today, rounded up (docs/c4-float.md)
    i = 1; while (i <= 9) { limit[i] = 0; ++i; }
    limit[10] = 1; limit[11] = 1; limit[12] = 1; limit[13] = 1; limit[14] = 1; limit[15] = 1; limit[16] = 2;
    limit[17] = 1; limit[18] = 1; limit[19] = 1; limit[20] = 1; limit[21] = 3; limit[22] = 1; limit[23] = 2;
    limit[24] = 1; limit[25] = 1; limit[26] = 2; limit[27] = 2; limit[28] = 2; limit[29] = 2;
    if (argc < 2) { printf("usage: mathtest FILE\n"); return 2; }
    fd = open(argv[1], 0);
    if (fd < 0) { printf("mathtest: cannot open %s\n", argv[1]); return 2; }
    buf = malloc(4000000);
    len = 0;
    while (len < 4000000 - 65536 && (n = read(fd, buf + len, 65536)) > 0) len = len + n;
    if (len >= 4000000 - 65536) { printf("mathtest: %s is too big\n", argv[1]); return 2; }
    close(fd);
    pos = 0;
    while (1) {
        while (pos < len && (buf[pos] == ' ' || buf[pos] == 10)) ++pos;
        if (pos >= len) break;
        fn = decimal();
        a = word2(&ba); b = word2(&bb); r = word2(&br);
        if (fn == 1) r = floor(a); else if (fn == 2) r = ceil(a); else if (fn == 3) r = trunc(a);
        else if (fn == 4) r = round(a); else if (fn == 5) r = sqrt(a); else if (fn == 6) r = frexp(a, &e);
        else if (fn == 7) { r = 0; }          // ldexp's exponent is not in the vector: see below
        else if (fn == 8) r = fmod(a, b); else if (fn == 9) r = modf(a, &ip); else if (fn == 10) r = exp(a);
        else if (fn == 11) r = log(a); else if (fn == 12) r = log10(a); else if (fn == 13) r = log2(a);
        else if (fn == 14) r = sin(a); else if (fn == 15) r = cos(a); else if (fn == 16) r = tan(a);
        else if (fn == 17) r = atan(a); else if (fn == 18) r = atan2(a, b); else if (fn == 19) r = asin(a);
        else if (fn == 20) r = acos(a); else if (fn == 21) r = sinh(a); else if (fn == 22) r = cosh(a);
        else if (fn == 23) r = tanh(a); else if (fn == 24 || fn == 25) r = pow(a, b);
        else if (fn == 26) r = hypot(a, b); else if (fn == 27) r = cbrt(a);
        else if (fn == 28) r = log1p(a); else if (fn == 29) r = expm1(a);
        if (fn == 7) continue;
        bg = *(unsigned long long *)&r;
        u = ulps(bg, br);
        ++count[fn];
        if (u > worst[fn]) worst[fn] = u;
        if (u > limit[fn]) {
            if (++shown <= 20) {
                printf("%s %x %x %x %x: ", names[fn], (unsigned int)(ba >> 32), (unsigned int)ba,
                    (unsigned int)(bb >> 32), (unsigned int)bb);
                printf("want %x %x got %x %x\n", (unsigned int)(br >> 32), (unsigned int)br,
                    (unsigned int)(bg >> 32), (unsigned int)bg);
            }
        }
    }
    bad = 0;
    i = 1;
    while (i <= 29) {
        if (count[i]) {
            printf("  %s: %d cases, worst %d ulps (limit %d)\n", names[i], count[i],
                worst[i] > 1000000 ? 1000000 : (int)worst[i], limit[i]);
            if (worst[i] > limit[i]) bad = 1;
        }
        ++i;
    }
    printf("mathtest: %s\n", bad ? "FAILED" : "OK");
    return bad;
}
