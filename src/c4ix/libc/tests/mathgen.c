// mathgen.c -- test vectors for C4IX libc's math.c, from glibc's libm.
//   gcc -O2 -o mathgen mathgen.c -lm && ./mathgen N > math.txt
// Lines: FN AHI ALO BHI BLO RHI RLO (B is 0 0 for one-argument functions).
// Function numbers are mathtest.c's table.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef unsigned long long u64;
typedef unsigned int u32;
static u64 rs = 0xBB67AE8584CAA73Bull;
static u64 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static double db(u64 x) { double d; memcpy(&d, &x, 8); return d; }
static u64 bd(double d) { u64 x; memcpy(&x, &d, 8); return x; }
static double uni(double lo, double hi) { return lo + (hi - lo) * ((rnd() >> 11) * (1.0 / 9007199254740992.0)); }
static double any(void) {
    switch (rnd() % 5) {
    case 0: return db(rnd());
    case 1: return uni(-10, 10);
    case 2: return uni(-1e6, 1e6);
    case 3: return ldexp(uni(-1, 1), (int)(rnd() % 200) - 100);
    default: return (double)((long long)(rnd() % 2000) - 1000) / 8;
    }
}
static void out(int fn, double a, double b, double r) {
    printf("%d %x %x %x %x %x %x\n", fn, (u32)(bd(a) >> 32), (u32)bd(a), (u32)(bd(b) >> 32), (u32)bd(b), (u32)(bd(r) >> 32), (u32)bd(r));
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 400, i, e;
    double a, b, ip;
    static const double sp[] = { 0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 1e-310, 1e308, INFINITY, -INFINITY, NAN, 3.0, 0.1 };
    for (i = 0; i < n; i++) {
        a = any();
        out(1, a, 0, floor(a)); out(2, a, 0, ceil(a)); out(3, a, 0, trunc(a)); out(4, a, 0, round(a));
        out(5, fabs(a), 0, sqrt(fabs(a))); out(6, a, 0, frexp(a, &e)); out(7, a, 0, ldexp(a, (int)(rnd() % 400) - 200));
        b = any(); out(8, a, b, fmod(a, b));
        out(9, a, 0, modf(a, &ip));
        a = uni(-700, 700); out(10, a, 0, exp(a));
        a = fabs(any()); out(11, a, 0, log(a)); out(12, a, 0, log10(a)); out(13, a, 0, log2(a));
        a = uni(-100, 100); out(14, a, 0, sin(a)); out(15, a, 0, cos(a)); out(16, a, 0, tan(a));
        a = any(); if (isfinite(a)) { out(14, a, 0, sin(a)); out(15, a, 0, cos(a)); out(16, a, 0, tan(a)); }
        a = ldexp(uni(1, 2), (int)(rnd() % 1024)); out(14, a, 0, sin(a)); out(15, a, 0, cos(a)); out(16, a, 0, tan(a));
        a = uni(-1e12, 1e12); b = uni(-300, 300); out(25, fabs(a), b, pow(fabs(a), b));
        a = any(); out(17, a, 0, atan(a)); b = any(); out(18, a, b, atan2(a, b));
        a = uni(-1, 1); out(19, a, 0, asin(a)); out(20, a, 0, acos(a));
        a = uni(-20, 20); out(21, a, 0, sinh(a)); out(22, a, 0, cosh(a)); out(23, a, 0, tanh(a));
        a = uni(0, 100); b = (double)((int)(rnd() % 40) - 20); out(24, a, b, pow(a, b));   // whole exponents
        a = uni(0.01, 100); b = uni(-8, 8); out(25, a, b, pow(a, b));                        // the rest, moderate
        a = any(); b = any(); out(26, a, b, hypot(a, b));
        a = any(); out(27, a, 0, cbrt(a));
        a = uni(-0.9, 10); out(28, a, 0, log1p(a)); a = uni(-10, 10); out(29, a, 0, expm1(a));
    }
    for (i = 0; i < (int)(sizeof sp / sizeof sp[0]); i++) {
        a = sp[i];
        out(1, a, 0, floor(a)); out(5, a, 0, sqrt(a)); out(10, a, 0, exp(a)); out(11, a, 0, log(a));
        out(14, a, 0, sin(a)); out(17, a, 0, atan(a)); out(24, a, 2, pow(a, 2)); out(24, a, -1, pow(a, -1));
        out(24, a, 0.5, pow(a, 0.5)); out(18, a, -1, atan2(a, -1)); out(8, a, 0.75, fmod(a, 0.75));
    }
    return 0;
}
