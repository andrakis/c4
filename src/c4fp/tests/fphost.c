// fphost.c -- softfp.c against the host's SSE, bit for bit. gcc only:
//   gcc -O2 -o fphost fphost.c -lm && ./fphost [N]
// Edge values crossed with each other, then N random operand pairs per
// operation, drawn to hit what goes wrong: near-equal exponents
// (cancellation), subnormals, halfway cases, integers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "softfp.c"

typedef unsigned int u32;
typedef unsigned long long u64;

static u64 rs = 0x243F6A8885A308D3ull;
static u64 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static float fb(u32 x) { float f; memcpy(&f, &x, 4); return f; }
static u32 bf(float f) { u32 x; memcpy(&x, &f, 4); return x; }
static double db(u64 x) { double d; memcpy(&d, &x, 8); return d; }
static u64 bd(double d) { u64 x; memcpy(&x, &d, 8); return x; }

static long bad, tried;
static void miss32(const char *op, u32 a, u32 b, u32 want, u32 got) {
    if (bad++ < 25) printf("%s %08x %08x: want %08x got %08x\n", op, a, b, want, got);
}
static void miss64(const char *op, u64 a, u64 b, u64 want, u64 got) {
    if (bad++ < 25) printf("%s %016llx %016llx: want %016llx got %016llx\n", op, a, b, want, got);
}

static u32 edge32[] = { 0, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000,
    0xFFC00000, 0x7F800001, 0x7FA00000, 0x00000001, 0x80000001, 0x007FFFFF, 0x00800000, 0x7F7FFFFF,
    0x3F800001, 0x3F7FFFFF, 0x4B000000, 0x4B7FFFFF, 0x5F000000, 0xDF000000, 0x4EFFFFFF, 0x34000000,
    0x00400000, 0x40000000, 0x3F000000, 0x3EAAAAAB, 0x40490FDB, 0x01000000 };
static u64 edge64[] = { 0, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull,
    0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000000ull,
    0x7FF0000000000001ull, 0x7FF4000000000000ull, 1, 0x8000000000000001ull, 0x000FFFFFFFFFFFFFull,
    0x0010000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x3FF0000000000001ull, 0x3FEFFFFFFFFFFFFFull,
    0x4330000000000000ull, 0x433FFFFFFFFFFFFFull, 0x43E0000000000000ull, 0xC3E0000000000000ull,
    0x43DFFFFFFFFFFFFFull, 0x3CA0000000000000ull, 0x0008000000000000ull, 0x4000000000000000ull,
    0x3FE0000000000000ull, 0x3FD5555555555555ull, 0x400921FB54442D18ull, 0x0020000000000000ull,
    0x41DFFFFFFFC00000ull, 0xC1E0000000000000ull, 0x47EFFFFFE0000000ull, 0x36A0000000000000ull };
#define NE32 (sizeof edge32 / sizeof edge32[0])
#define NE64 (sizeof edge64 / sizeof edge64[0])

// operands with a bias toward the troublesome
static u32 pick32(u32 near) {
    u32 r = (u32)rnd();
    switch (rnd() % 8) {
    case 0: return edge32[rnd() % NE32] ^ (u32)(rnd() % 3);
    case 1: return r & 0x807FFFFF;                       // subnormal
    case 2: return (near & 0xFF800000) | (r & 0x007FFFFF);   // same exponent as the other
    case 3: return (near + (u32)(rnd() % 7) - 3) ^ (r & 0x80000000);  // nearly equal
    case 4: return bf((float)(int)(r >> (rnd() % 31)));   // an integer
    default: return r;
    }
}
static u64 pick64(u64 near) {
    u64 r = rnd();
    switch (rnd() % 8) {
    case 0: return edge64[rnd() % NE64] ^ (rnd() % 3);
    case 1: return r & 0x800FFFFFFFFFFFFFull;
    case 2: return (near & 0xFFF0000000000000ull) | (r & 0x000FFFFFFFFFFFFFull);
    case 3: return (near + rnd() % 7 - 3) ^ (r & 0x8000000000000000ull);
    case 4: return bd((double)(long long)(r >> (rnd() % 63)));
    default: return r;
    }
}

static int hcmp32(u32 a, u32 b) { float x = fb(a), y = fb(b); if (x != x || y != y) return 2; return x < y ? -1 : x > y ? 1 : 0; }
static int hcmp64(u64 a, u64 b) { double x = db(a), y = db(b); if (x != x || y != y) return 2; return x < y ? -1 : x > y ? 1 : 0; }

static void one32(u32 a, u32 b) {
    volatile float x = fb(a), y = fb(b);
    tried++;
    if (bf(x + y) != __c4f_add(a, b)) miss32("fadd", a, b, bf(x + y), __c4f_add(a, b));
    if (bf(x - y) != __c4f_sub(a, b)) miss32("fsub", a, b, bf(x - y), __c4f_sub(a, b));
    if (bf(x * y) != __c4f_mul(a, b)) miss32("fmul", a, b, bf(x * y), __c4f_mul(a, b));
    if (bf(x / y) != __c4f_div(a, b)) miss32("fdiv", a, b, bf(x / y), __c4f_div(a, b));
    if (bf(sqrtf(x)) != __c4f_sqrt(a)) miss32("fsqrt", a, 0, bf(sqrtf(x)), __c4f_sqrt(a));
    if (hcmp32(a, b) != __c4f_cmp(a, b)) miss32("fcmp", a, b, hcmp32(a, b), __c4f_cmp(a, b));
    if (bd((double)x) != __c4f_to_d(a)) miss64("f2d", a, 0, bd((double)x), __c4f_to_d(a));
    if (!(x != x) && fabsf(x) < 9.2e18f) {
        if ((u64)(long long)x != (u64)__c4f_to_ll(a)) miss64("f2ll", a, 0, (long long)x, __c4f_to_ll(a));
        if ((u32)(int)x != (u32)__c4f_to_i(a)) miss32("f2i", a, 0, (int)x, __c4f_to_i(a));
    }
    if (x >= 0 && x < 1.8e19f && (u64)x != __c4f_to_ull(a)) miss64("f2ull", a, 0, (u64)x, __c4f_to_ull(a));
}

static void one64(u64 a, u64 b) {
    volatile double x = db(a), y = db(b);
    tried++;
    if (bd(x + y) != __c4d_add(a, b)) miss64("dadd", a, b, bd(x + y), __c4d_add(a, b));
    if (bd(x - y) != __c4d_sub(a, b)) miss64("dsub", a, b, bd(x - y), __c4d_sub(a, b));
    if (bd(x * y) != __c4d_mul(a, b)) miss64("dmul", a, b, bd(x * y), __c4d_mul(a, b));
    if (bd(x / y) != __c4d_div(a, b)) miss64("ddiv", a, b, bd(x / y), __c4d_div(a, b));
    if (bd(sqrt(x)) != __c4d_sqrt(a)) miss64("dsqrt", a, 0, bd(sqrt(x)), __c4d_sqrt(a));
    if (hcmp64(a, b) != __c4d_cmp(a, b)) miss64("dcmp", a, b, hcmp64(a, b), __c4d_cmp(a, b));
    if (bf((float)x) != __c4d_to_f(a)) miss32("d2f", (u32)(a >> 32), (u32)a, bf((float)x), __c4d_to_f(a));
    if (!(x != x) && fabs(x) < 9.2e18) {
        if ((u64)(long long)x != (u64)__c4d_to_ll(a)) miss64("d2ll", a, 0, (long long)x, __c4d_to_ll(a));
        if ((u32)(int)x != (u32)__c4d_to_i(a)) miss64("d2i", a, 0, (u32)(int)x, (u32)__c4d_to_i(a));
    }
    if (x >= 0 && x < 1.8e19 && (u64)x != __c4d_to_ull(a)) miss64("d2ull", a, 0, (u64)x, __c4d_to_ull(a));
}

static void ints(u64 r) {
    long long s = (long long)r;
    volatile long long vs = s; volatile u64 vu = r; volatile int vi = (int)r; volatile u32 vw = (u32)r;
    if (bf((float)vs) != __c4f_from_ll(s)) miss64("ll2f", r, 0, bf((float)vs), __c4f_from_ll(s));
    if (bf((float)vu) != __c4f_from_ull(r)) miss64("ull2f", r, 0, bf((float)vu), __c4f_from_ull(r));
    if (bf((float)vi) != __c4f_from_ll((int)r)) miss64("i2f", r, 0, bf((float)vi), __c4f_from_ll((int)r));
    if (bf((float)vw) != __c4f_from_u((u32)r)) miss64("u2f", r, 0, bf((float)vw), __c4f_from_u((u32)r));
    if (bd((double)vs) != __c4d_from_ll(s)) miss64("ll2d", r, 0, bd((double)vs), __c4d_from_ll(s));
    if (bd((double)vu) != __c4d_from_ull(r)) miss64("ull2d", r, 0, bd((double)vu), __c4d_from_ull(r));
    if (bd((double)vw) != __c4d_from_u((u32)r)) miss64("u2d", r, 0, bd((double)vw), __c4d_from_u((u32)r));
}

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 2000000, i;
    unsigned j, k;
    u32 a; u64 c;
    for (j = 0; j < NE32; j++) for (k = 0; k < NE32; k++) one32(edge32[j], edge32[k]);
    for (j = 0; j < NE64; j++) for (k = 0; k < NE64; k++) one64(edge64[j], edge64[k]);
    for (i = 0; i < n; i++) {
        a = pick32((u32)rnd()); one32(a, pick32(a));
        c = pick64(rnd()); one64(c, pick64(c));
        ints(rnd() >> (rnd() % 64));
    }
    for (j = 0; j < 64; j++) { ints((u64)1 << j); ints(((u64)1 << j) - 1); ints(-((u64)1 << j)); ints(((u64)1 << j) + 1); }
    printf("fphost: %ld operand sets, %ld wrong\n", tried, bad);
    return bad != 0;
}
