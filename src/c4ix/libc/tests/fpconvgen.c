// fpconvgen.c -- test vectors for C4IX libc's fp.c, from glibc's own
// printf, strtod and strtof (exact, as fp.c must be). gcc only:
//   gcc -O2 -o fpconvgen fpconvgen.c && ./fpconvgen N > fpconv.txt
// Lines:
//   P CONV PREC FLAGS HI LO|TEXT      printf("%<FLAGS>.<PREC><CONV>") of the double
//   D HI LO TEXT                      strtod(TEXT)
//   F BITS TEXT                       strtof(TEXT)
// FLAGS is a word of # + and space spelt h p s, or "-" for none; PREC -1
// is no precision written.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef unsigned long long u64;
typedef unsigned int u32;
static u64 rs = 0x6A09E667F3BCC908ull;
static u64 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static double db(u64 x) { double d; memcpy(&d, &x, 8); return d; }
static u64 bd(double d) { u64 x; memcpy(&x, &d, 8); return x; }
static u32 bf(float f) { u32 x; memcpy(&x, &f, 4); return x; }

static double pick(void) {
    static const double edge[] = { 0.0, -0.0, 1.0, 0.1, 0.5, 1.5, 2.5, 0.25, 0.125, 9.5, 99.5, 999.9999,
        123456.789, 1e-5, 1e-4, 0.0001234, 1e15, 1e16, 1e17, 1e21, 1e22, 1e23, 9007199254740993.0,
        4.9e-324, 2.2250738585072014e-308, 1.7976931348623157e308, 3.141592653589793, 2.718281828459045,
        0.3, 0.7, 1e100, 1e-100, 5e-324, 1.0/3, 2.0/3, 100, 1000000, 0.05, 0.015, 0.0625 };
    u64 r = rnd();
    switch (r % 8) {
    case 0: return edge[rnd() % (sizeof edge / sizeof edge[0])] * ((rnd() & 1) ? -1 : 1);
    case 1: return db(rnd());                                    // any bits
    case 2: return (double)(long long)(rnd() >> (rnd() % 60)) / (double)(1 + rnd() % 1000);
    case 3: return (double)(rnd() % 100000) / 1000.0;            // short decimals
    case 4: return ldexp((double)(rnd() >> 11), (int)(rnd() % 2100) - 1100);
    case 5: return (double)(rnd() % 1000) + 0.5;                 // halfway cases
    case 6: return (double)(rnd() % 20000) / 64.0;
    default: return db(rnd() & 0x000FFFFFFFFFFFFFull);           // subnormal
    }
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 300, i, j, prec;
    static const char convs[] = "feEgGF";
    char fmt[32], out[2000], text[200], fl[8], fw[8];
    double d;
    float f;
    for (i = 0; i < n; i++) {
        d = pick();
        if (d != d) continue;            // NaN payloads: glibc prints "nan"; tested below by bits
        for (j = 0; j < 6; j++) {
            prec = (rnd() % 4 == 0) ? -1 : (int)(rnd() % 20);
            if (convs[j] == 'f' && fabs(d) > 1e30 && prec > 6) prec = 6;
            int fs = (int)(rnd() % 8);
            int k = 0, m = 0;
            fl[0] = 0;
            if (fs & 1) { fl[k++] = '#'; fw[m++] = 'h'; }
            if (fs & 2) { fl[k++] = '+'; fw[m++] = 'p'; }
            if ((fs & 4) && !(fs & 2)) { fl[k++] = ' '; fw[m++] = 's'; }
            fl[k] = 0; fw[m] = 0;
            if (!m) { fw[0] = '-'; fw[1] = 0; }
            if (prec < 0) snprintf(fmt, sizeof fmt, "%%%s%c", fl, convs[j]);
            else snprintf(fmt, sizeof fmt, "%%%s.%d%c", fl, prec, convs[j]);
            snprintf(out, sizeof out, fmt, d);
            printf("P %c %d %s %x %x|%s\n", convs[j], prec, fw, (u32)(bd(d) >> 32), (u32)bd(d), out);
        }
    }
    // infinities and NaNs
    {
        u64 sp[] = { 0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000000ull };
        for (i = 0; i < 4; i++) for (j = 0; j < 6; j++) {
            snprintf(fmt, sizeof fmt, "%%%c", convs[j]);
            snprintf(out, sizeof out, fmt, db(sp[i]));
            printf("P %c -1 - %x %x|%s\n", convs[j], (u32)(sp[i] >> 32), (u32)sp[i], out);
        }
    }
    // strtod and strtof: what printf writes, at every precision, and more
    for (i = 0; i < n; i++) {
        d = pick();
        if (d != d) continue;
        switch (rnd() % 6) {
        case 0: snprintf(text, sizeof text, "%.*e", (int)(rnd() % 20), d); break;
        case 1: snprintf(text, sizeof text, "%.17g", d); break;
        case 2: snprintf(text, sizeof text, "%a", d); break;
        case 3: snprintf(text, sizeof text, "%.*g", (int)(1 + rnd() % 25), d); break;
        case 4: snprintf(text, sizeof text, "%llu.%lluE%d", rnd() % 1000000, rnd() % 1000000000000ull, (int)(rnd() % 700) - 350); break;
        default: snprintf(text, sizeof text, "%.40f", d); if (strlen(text) > 120) text[120] = 0; break;
        }
        d = strtod(text, 0);
        printf("D %x %x %s\n", (u32)(bd(d) >> 32), (u32)bd(d), text);
        f = strtof(text, 0);
        printf("F %x %s\n", bf(f), text);
    }
    {
        static const char *t[] = { "0", "-0", "inf", "-INFINITY", "nan", "1e400", "1e-400", "2.4703282292062327e-324",
            "2.4703282292062328e-324", "9007199254740993", "9007199254740995", "0.1", ".5", "5.", "1e+5",
            "0x1p-1074", "0x1.fffffffffffff8p1023", "0X1.8P3", "1.17549435e-38", "1.4e-45", "7.006492e-46",
            "123456789012345678901234567890", "000000000000000000001.5", "3.40282357e38", "3.40282347e38",
            "1.000000059604644775390625", "1.0000000596046447753906250000000001", "4.9406564584124654e-324",
            "179769313486231580793728971405303415079934132710037826936173778980444968292764750946649017977587207096330286416692887910946555547851940402630657488671505820681908902000708383676273854845817711531764475730270069855571366959622842914819860834936475292719074168444365510704342711559699508093042880177904174497791.0" };
        for (i = 0; i < (int)(sizeof t / sizeof t[0]); i++) {
            d = strtod(t[i], 0); f = strtof(t[i], 0);
            printf("D %x %x %s\n", (u32)(bd(d) >> 32), (u32)bd(d), t[i]);
            printf("F %x %s\n", bf(f), t[i]);
        }
    }
    return 0;
}
