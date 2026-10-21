// fp.c -- C4IX libc's floating-point conversions (docs/c4-float.md):
// the text printf writes for %f %e %g, and strtod, strtof and atof.
//
// Both directions are EXACT: the value a double holds is m * 2^e, which
// is a whole number times a power of ten, and big integers carry it the
// whole way -- so printf's digits are the true ones, rounded once to
// nearest with ties to even (glibc's answer, character for character),
// and strtod's double is the one nearest the decimal it read. Slower
// than an approximate algorithm by a long way for tiny or huge numbers,
// and fast enough for everything else; a C4IX program prints few floats.
//
// No syscalls and nothing from the rest of libc but errno, so the tests
// compile this file alone (src/c4ix/libc/tests/fptest.c).

extern int errno;

// ---- printing: the decimal digits of a double --------------------------
//
// A big integer in base 10^9 limbs, lowest first, so the digits come
// straight out. The largest is m * 5^1074 for the smallest subnormal,
// about 770 digits: 86 limbs.
static unsigned int fp_dl[100];
static int fp_dn;
// The digits, most significant first, and how many
static char fp_dig[800];
static int fp_nd;

static void fp_dmul(unsigned int m) {
    unsigned long long x, carry;
    int i;
    carry = 0;
    i = 0;
    while (i < fp_dn) {
        x = (unsigned long long)fp_dl[i] * m + carry;
        fp_dl[i] = (unsigned int)(x % 1000000000);
        carry = x / 1000000000;
        ++i;
    }
    while (carry) {
        fp_dl[fp_dn] = (unsigned int)(carry % 1000000000);
        carry = carry / 1000000000;
        ++fp_dn;
    }
}

// The digits of f * 2^e (f > 0): fp_dig and fp_nd, trailing zeros
// dropped; answers E10, with the value 0.DIGITS * 10^E10
static int fp_digits(unsigned long long f, int e) {
    int k, j, i, e10;
    unsigned int p5, v;
    char t[9];
    fp_dl[0] = (unsigned int)(f % 1000000000);
    fp_dl[1] = (unsigned int)(f / 1000000000);
    fp_dn = fp_dl[1] ? 2 : 1;
    if (e >= 0) {
        k = e;
        while (k >= 29) { fp_dmul(1 << 29); k = k - 29; }
        if (k > 0) fp_dmul(1 << k);
    } else {
        // f / 2^-e is f * 5^-e / 10^-e
        k = 0 - e;
        while (k >= 13) { fp_dmul(1220703125); k = k - 13; }
        p5 = 1;
        while (k > 0) { p5 = p5 * 5; --k; }
        if (p5 > 1) fp_dmul(p5);
    }
    fp_nd = 0;
    i = fp_dn - 1;
    v = fp_dl[i];
    j = 0;
    while (v) { t[j] = '0' + v % 10; v = v / 10; ++j; }
    while (j > 0) { --j; fp_dig[fp_nd] = t[j]; ++fp_nd; }
    --i;
    while (i >= 0) {
        v = fp_dl[i];
        j = 9;
        while (j > 0) { --j; t[j] = '0' + v % 10; v = v / 10; }
        while (j < 9) { fp_dig[fp_nd] = t[j]; ++fp_nd; ++j; }
        --i;
    }
    e10 = fp_nd;
    if (e < 0) e10 = e10 + e;
    while (fp_nd > 1 && fp_dig[fp_nd - 1] == '0') --fp_nd;
    return e10;
}

// Keep N significant digits, rounded to nearest, ties to even. Answers
// 1 when the rounding carried into a new leading digit (the exponent
// grows by one). N <= 0 leaves nothing, or a 1 when what was dropped
// was more than half.
static int fp_round(int n) {
    int up, i, j;
    if (n >= fp_nd) return 0;
    if (n < 0) { fp_nd = 0; return 0; }
    if (fp_dig[n] > '5') up = 1;
    else if (fp_dig[n] < '5') up = 0;
    else {
        up = 0;
        i = n + 1;
        while (i < fp_nd) { if (fp_dig[i] != '0') up = 1; ++i; }
        if (!up && n > 0) up = (fp_dig[n - 1] - '0') & 1;
    }
    fp_nd = n;
    if (!up) return 0;
    i = n - 1;
    while (i >= 0 && fp_dig[i] == '9') { fp_dig[i] = '0'; --i; }
    if (i >= 0) { fp_dig[i] = fp_dig[i] + 1; return 0; }
    // 99.9 -> 100: one digit more in front
    fp_dig[0] = '1';
    j = 1;
    while (j < n) { fp_dig[j] = '0'; ++j; }
    fp_nd = n > 0 ? n : 1;
    return 1;
}

static int fp_put(char *o, int n, int c) { o[n] = c; return n + 1; }
static char fp_digit(int i) { return (i >= 0 && i < fp_nd) ? fp_dig[i] : '0'; }

// Is the double an infinity or a NaN? Set by __c4ix_fmtfp for printf,
// which pads those with spaces whatever the flags say
int __c4ix_fpspecial;

// x as printf's CONV (f F e E g G) writes it, with PREC (-1: 6) and the
// # + and space flags, into O: the sign and the number, unpadded (the
// caller pads). Answers the length.
int __c4ix_fmtfp(char *o, double x, int conv, int prec, int alt, int plus, int space) {
    unsigned long long bits, f;
    int n, e2, ex, e10, upper, lc, X, P, i, fstyle, mark, ed;
    char eb[8];
    bits = *(unsigned long long *)&x;
    n = 0;
    if (bits >> 63) n = fp_put(o, n, '-');
    else if (plus) n = fp_put(o, n, '+');
    else if (space) n = fp_put(o, n, ' ');
    upper = (conv == 'F' || conv == 'E' || conv == 'G');
    lc = upper ? conv + 32 : conv;
    ex = (int)(bits >> 52) & 2047;
    f = bits & 0x000FFFFFFFFFFFFFull;
    __c4ix_fpspecial = (ex == 2047);
    if (ex == 2047) {
        if (f) { n = fp_put(o, n, upper ? 'N' : 'n'); n = fp_put(o, n, upper ? 'A' : 'a'); n = fp_put(o, n, upper ? 'N' : 'n'); }
        else { n = fp_put(o, n, upper ? 'I' : 'i'); n = fp_put(o, n, upper ? 'N' : 'n'); n = fp_put(o, n, upper ? 'F' : 'f'); }
        o[n] = 0;
        return n;
    }
    if (prec < 0) prec = 6;
    if (ex == 0 && f == 0) { fp_nd = 0; e10 = 1; }
    else {
        if (ex) { f = f | 0x0010000000000000ull; e2 = ex - 1075; } else e2 = 0 - 1074;
        e10 = fp_digits(f, e2);
    }
    fstyle = (lc == 'f');
    if (lc == 'g') {
        // P significant digits; %e's exponent decides which style
        P = prec ? prec : 1;
        e10 = e10 + fp_round(P);
        X = fp_nd ? e10 - 1 : 0;
        if (X < P && X >= 0 - 4) { fstyle = 1; prec = P - 1 - X; }
        else prec = P - 1;
    }
    mark = 0;
    if (fstyle) {
        if (fp_nd) e10 = e10 + fp_round(e10 + prec);
        if (fp_nd == 0) e10 = 1;
        if (e10 > 0) { i = 0; while (i < e10) { n = fp_put(o, n, fp_digit(i)); ++i; } }
        else n = fp_put(o, n, '0');
        if (prec > 0 || alt) { mark = n; n = fp_put(o, n, '.'); }
        i = 0;
        while (i < prec) { n = fp_put(o, n, fp_digit(e10 + i)); ++i; }
    } else {
        if (fp_nd) e10 = e10 + fp_round(prec + 1);
        X = fp_nd ? e10 - 1 : 0;
        n = fp_put(o, n, fp_digit(0));
        if (prec > 0 || alt) { mark = n; n = fp_put(o, n, '.'); }
        i = 1;
        while (i <= prec) { n = fp_put(o, n, fp_digit(i)); ++i; }
    }
    // %g drops the zeros at the end of the fraction, and the point too
    if (lc == 'g' && !alt && mark) {
        while (o[n - 1] == '0') --n;
        if (o[n - 1] == '.') --n;
    }
    if (!fstyle) {
        n = fp_put(o, n, upper ? 'E' : 'e');
        n = fp_put(o, n, X < 0 ? '-' : '+');
        if (X < 0) X = 0 - X;
        ed = 0;
        while (X || ed < 2) { eb[ed] = '0' + X % 10; X = X / 10; ++ed; }
        while (ed > 0) { --ed; n = fp_put(o, n, eb[ed]); }
    }
    o[n] = 0;
    return n;
}

// stdio's way in: the double as its two words, so that stdio.c need not
// say double (and so need not carry c4lc's float helpers)
int __c4ix_fmtfpw(char *o, unsigned int hi, unsigned int lo, int conv, int prec, int alt, int plus, int space) {
    unsigned long long b;
    b = ((unsigned long long)hi << 32) | lo;
    return __c4ix_fmtfp(o, *(double *)&b, conv, prec, alt, plus, space);
}

// ---- reading: the double nearest a decimal --------------------------
//
// A big integer in base 2^32, lowest first. The digits of a long
// decimal times 10^400 is about 4,000 bits: 130 limbs, and room to shift.
enum { FP_NB = 160 };
static unsigned int fp_a[FP_NB], fp_b[FP_NB];
static int fp_an, fp_bn;

static int fp_bset(unsigned int *a, unsigned int v) { a[0] = v; return 1; }
static int fp_bmul(unsigned int *a, int n, unsigned int m, unsigned int c) {
    unsigned long long x;
    int i;
    i = 0;
    while (i < n) {
        x = (unsigned long long)a[i] * m + c;
        a[i] = (unsigned int)x;
        c = (unsigned int)(x >> 32);
        ++i;
    }
    if (c && n < FP_NB) { a[n] = c; ++n; }
    return n;
}
static int fp_bshl(unsigned int *a, int n, int s) {
    int w, b, i;
    w = s / 32; b = s - w * 32;
    if (n + w + 1 > FP_NB) return n;      // cannot happen within the clamps
    if (b) {
        a[n] = 0;
        i = n;
        while (i > 0) { a[i] = (a[i] << b) | (a[i - 1] >> (32 - b)); --i; }
        a[0] = a[0] << b;
        ++n;
    }
    if (w) {
        i = n - 1;
        while (i >= 0) { a[i + w] = a[i]; --i; }
        i = 0;
        while (i < w) { a[i] = 0; ++i; }
        n = n + w;
    }
    while (n > 1 && a[n - 1] == 0) --n;
    return n;
}
static int fp_bshr1(unsigned int *a, int n) {
    int i;
    i = 0;
    while (i < n - 1) { a[i] = (a[i] >> 1) | (a[i + 1] << 31); ++i; }
    a[n - 1] = a[n - 1] >> 1;
    while (n > 1 && a[n - 1] == 0) --n;
    return n;
}
static int fp_bcmp(unsigned int *a, int an, unsigned int *b, int bn) {
    int i;
    if (an != bn) return an < bn ? 0 - 1 : 1;
    i = an - 1;
    while (i >= 0) {
        if (a[i] != b[i]) return a[i] < b[i] ? 0 - 1 : 1;
        --i;
    }
    return 0;
}
static int fp_bsub(unsigned int *a, int an, unsigned int *b, int bn) {
    unsigned long long x;
    unsigned int brw;
    int i;
    brw = 0;
    i = 0;
    while (i < an) {
        x = (unsigned long long)a[i] - (i < bn ? b[i] : 0) - brw;
        a[i] = (unsigned int)x;
        brw = (unsigned int)(x >> 63);
        ++i;
    }
    while (an > 1 && a[an - 1] == 0) --an;
    return an;
}
static int fp_bbits(unsigned int *a, int n) {
    unsigned int v;
    int b;
    v = a[n - 1];
    b = 0;
    while (v) { ++b; v = v >> 1; }
    return (n - 1) * 32 + b;
}

// M * 2^E2, with STICKY saying something below M's last bit was lost,
// rounded to the format of P significant bits and exponents EMIN..EMAX:
// the bits of a double (P 53) or a float (P 24)
static unsigned long long fp_make(unsigned long long m, int sticky, int e2, int p, int emin, int emax) {
    int L, sh, e, drop, fb;
    unsigned long long keep, half, rest;
    if (!m) return 0;
    L = 0;
    keep = m;
    while (keep) { ++L; keep = keep >> 1; }
    // P+2 bits: the significand, a guard bit, a round bit; then sticky
    if (L > p + 2) {
        sh = L - (p + 2);
        if (m & ((1ull << sh) - 1)) sticky = 1;
        m = m >> sh;
        e2 = e2 + sh;
    } else {
        m = m << (p + 2 - L);
        e2 = e2 - (p + 2 - L);
    }
    e = e2 + p + 1;                         // the exponent of the leading bit
    fb = p == 53 ? 52 : 23;
    if (e > emax) return (unsigned long long)(2 * emax + 1) << fb;
    drop = 2;
    if (e < emin) drop = 2 + (emin - e);
    if (drop > 62) return 0;                // below half the smallest subnormal
    keep = m >> drop;
    half = 1ull << (drop - 1);
    rest = m & ((1ull << drop) - 1);
    if (rest > half || (rest == half && (sticky || (keep & 1)))) ++keep;
    if (e < emin) {
        // a subnormal, or the smallest normal if the rounding carried
        if (keep >> (p - 1)) return 1ull << fb;
        return keep;
    }
    if (keep >> p) { keep = keep >> 1; ++e; if (e > emax) return (unsigned long long)(2 * emax + 1) << fb; }
    return ((unsigned long long)(e + emax) << fb) | (keep & ((1ull << fb) - 1));
}

// The double (P 53) or float (P 24) nearest DIGITS * 10^E10, exactly
static double fp_powten[23] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
static unsigned long long fp_decimal(char *ds, int nd, int e10, int p) {
    unsigned long long q, v;
    int i, s, k, emin, emax, an, bn, c;
    double d;
    float fl;
    emin = p == 53 ? 0 - 1022 : 0 - 126;
    emax = p == 53 ? 1023 : 127;
    // nothing but zeros
    i = 0;
    while (i < nd && ds[i] == '0') ++i;
    if (i == nd) return 0;
    ds = ds + i; nd = nd - i;
    if (e10 + nd > 400) return (unsigned long long)(2 * emax + 1) << (p == 53 ? 52 : 23);
    if (e10 + nd < 0 - 400) return 0;
    // the easy, common case: the digits and the power of ten are both
    // exact, so one correctly rounded operation is the answer
    if (nd <= 15 && e10 <= 22 && e10 >= 0 - 22 && (p == 53 || (nd <= 7 && e10 <= 10 && e10 >= 0 - 10))) {
        v = 0;
        i = 0;
        while (i < nd) { v = v * 10 + (ds[i] - '0'); ++i; }
        if (p == 53) {
            d = (double)v;
            if (e10 >= 0) d = d * fp_powten[e10]; else d = d / fp_powten[0 - e10];
            return *(unsigned long long *)&d;
        }
        fl = (float)v;
        if (e10 >= 0) fl = fl * (float)fp_powten[e10]; else fl = fl / (float)fp_powten[0 - e10];
        return (unsigned long long)*(unsigned int *)&fl;
    }
    // A = the digits, nine at a time
    an = fp_bset(fp_a, 0);
    i = 0;
    while (i < nd) {
        k = 0; c = 0;
        while (k < 9 && i < nd) { c = c * 10 + (ds[i] - '0'); ++k; ++i; }
        s = 1;
        while (k > 0) { s = s * 10; --k; }
        an = fp_bmul(fp_a, an, s, c);
    }
    bn = fp_bset(fp_b, 1);
    k = e10 >= 0 ? e10 : 0 - e10;
    while (k > 0) {
        if (e10 >= 0) an = fp_bmul(fp_a, an, k >= 9 ? 1000000000 : (k >= 1 ? 10 : 1), 0);
        else bn = fp_bmul(fp_b, bn, k >= 9 ? 1000000000 : 10, 0);
        k = k - (k >= 9 ? 9 : 1);
    }
    // scale so that A / B has P+2 bits: then 2^(P+1) <= A/B < 2^(P+2)
    s = (p + 1) - (fp_bbits(fp_a, an) - fp_bbits(fp_b, bn));
    if (s > 0) an = fp_bshl(fp_a, an, s); else if (s < 0) bn = fp_bshl(fp_b, bn, 0 - s);
    bn = fp_bshl(fp_b, bn, p + 1);
    if (fp_bcmp(fp_a, an, fp_b, bn) < 0) { an = fp_bshl(fp_a, an, 1); ++s; }
    // the quotient, a bit at a time: B is B*2^(P+1) and halves each step
    q = 0;
    i = p + 1;
    while (i >= 0) {
        q = q << 1;
        if (fp_bcmp(fp_a, an, fp_b, bn) >= 0) { an = fp_bsub(fp_a, an, fp_b, bn); q = q | 1; }
        bn = fp_bshr1(fp_b, bn);
        --i;
    }
    return fp_make(q, !(an == 1 && fp_a[0] == 0), 0 - s, p, emin, emax);
}

static int fp_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int fp_word(char *s, char *w) {
    int i;
    i = 0;
    while (w[i]) { if (fp_lower(s[i]) != w[i]) return 0; ++i; }
    return i;
}
static int fp_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = fp_lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0 - 1;
}
static char fp_ds[800];

// strtod and strtof's reader: the bits of the double (P 53) or float
// (P 24) the text at S says, the end in *END
static unsigned long long fp_read(char *s, char **end, int p) {
    char *t, *start;
    int neg, nd, e10, ev, eneg, seen, k, h, sticky, e2, any, emin, emax, fb;
    unsigned long long bits, m;
    start = s;
    emin = p == 53 ? 0 - 1022 : 0 - 126;
    emax = p == 53 ? 1023 : 127;
    fb = p == 53 ? 52 : 23;
    while (*s == ' ' || (*s >= 9 && *s <= 13)) ++s;
    neg = 0;
    if (*s == '+' || *s == '-') { neg = *s == '-'; ++s; }
    bits = 0;
    if ((k = fp_word(s, "infinity")) || (k = fp_word(s, "inf"))) {
        bits = (unsigned long long)(2 * emax + 1) << fb;
        s = s + k;
    } else if ((k = fp_word(s, "nan"))) {
        s = s + k;
        if (*s == '(') { t = s + 1; while (*t && *t != ')') ++t; if (*t == ')') s = t + 1; }
        bits = ((unsigned long long)(2 * emax + 1) << fb) | (1ull << (fb - 1));
    } else if (s[0] == '0' && fp_lower(s[1]) == 'x' && (fp_hexval(s[2]) >= 0 || (s[2] == '.' && fp_hexval(s[3]) >= 0))) {
        // hex: the digits into 60 bits, the rest only sticky
        s = s + 2;
        m = 0; sticky = 0; e2 = 0; seen = 0;
        while (1) {
            if (*s == '.' && !seen) { seen = 1; ++s; continue; }
            h = fp_hexval(*s);
            if (h < 0) break;
            if (m >> 56) { if (h) sticky = 1; if (!seen) e2 = e2 + 4; }
            else { m = (m << 4) | h; if (seen) e2 = e2 - 4; }
            ++s;
        }
        if (fp_lower(*s) == 'p') {
            t = s + 1; eneg = 0;
            if (*t == '+' || *t == '-') { eneg = *t == '-'; ++t; }
            if (*t >= '0' && *t <= '9') {
                ev = 0;
                while (*t >= '0' && *t <= '9') { if (ev < 100000) ev = ev * 10 + (*t - '0'); ++t; }
                e2 = e2 + (eneg ? 0 - ev : ev);
                s = t;
            }
        }
        bits = fp_make(m, sticky, e2, p, emin, emax);
    } else {
        // decimal: up to 780 digits kept; past them, only whether any is
        // nonzero matters, and a 1 in the next place says so
        nd = 0; e10 = 0; seen = 0; any = 0; sticky = 0;
        while (1) {
            if (*s == '.' && !seen) { seen = 1; ++s; continue; }
            if (*s < '0' || *s > '9') break;
            any = 1;
            if (nd == 0 && *s == '0') { if (seen) --e10; }
            else if (nd < 780) { fp_ds[nd] = *s; ++nd; if (seen) --e10; }
            else { if (*s != '0') sticky = 1; if (!seen) ++e10; }
            ++s;
        }
        if (!any) { if (end) *end = start; return 0; }
        if (sticky) { fp_ds[nd] = '1'; ++nd; --e10; }
        if (fp_lower(*s) == 'e') {
            t = s + 1; eneg = 0;
            if (*t == '+' || *t == '-') { eneg = *t == '-'; ++t; }
            if (*t >= '0' && *t <= '9') {
                ev = 0;
                while (*t >= '0' && *t <= '9') { if (ev < 100000) ev = ev * 10 + (*t - '0'); ++t; }
                e10 = e10 + (eneg ? 0 - ev : ev);
                s = t;
            }
        }
        bits = fp_decimal(fp_ds, nd, e10, p);
        // out of range: ERANGE, as C says, for an overflow and for an
        // answer that is no longer a normal number
        if (nd && (bits == (unsigned long long)(2 * emax + 1) << fb || (bits >> fb) == 0)) errno = 34;
    }
    if (end) *end = s;
    if (neg) bits = bits | (p == 53 ? 0x8000000000000000ull : 0x80000000ull);
    return bits;
}

double strtod(char *s, char **end) {
    unsigned long long b;
    b = fp_read(s, end, 53);
    return *(double *)&b;
}
float strtof(char *s, char **end) {
    unsigned long long b;
    unsigned int w;
    b = fp_read(s, end, 24);
    w = (unsigned int)b;
    return *(float *)&w;
}
double atof(char *s) { return strtod(s, 0); }
