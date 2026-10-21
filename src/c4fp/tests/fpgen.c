// fpgen.c -- test vectors for softfp.c run inside the VM (fpvm.c), from
// the host's own SSE results. gcc only:
//   gcc -O2 -o fpgen fpgen.c && ./fpgen [N] > fpvec.txt
// One line per case, hex: OP A B RESULT, each 64-bit value as two
// 32-bit halves (high first). Ops are numbered as fpvm.c's switch.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef unsigned int u32;
typedef unsigned long long u64;

static u64 rs = 0x9E3779B97F4A7C15ull;
static u64 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static float fb(u32 x) { float f; memcpy(&f, &x, 4); return f; }
static u32 bf(float f) { u32 x; memcpy(&x, &f, 4); return x; }
static double db(u64 x) { double d; memcpy(&d, &x, 8); return d; }
static u64 bd(double d) { u64 x; memcpy(&x, &d, 8); return x; }

static u32 edge32[] = { 0, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000,
    0x7F800001, 0x00000001, 0x80000001, 0x007FFFFF, 0x00800000, 0x7F7FFFFF, 0x3F800001, 0x3F7FFFFF,
    0x4B000000, 0x5F000000, 0xDF000000, 0x34000000, 0x40490FDB, 0x3EAAAAAB };
static u64 edge64[] = { 0, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull,
    0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull, 0x7FF0000000000001ull, 1,
    0x8000000000000001ull, 0x000FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x7FEFFFFFFFFFFFFFull,
    0x3FF0000000000001ull, 0x3FEFFFFFFFFFFFFFull, 0x4330000000000000ull, 0x43E0000000000000ull,
    0xC3E0000000000000ull, 0x3CA0000000000000ull, 0x400921FB54442D18ull, 0x3FD5555555555555ull };
#define NE32 (sizeof edge32 / sizeof edge32[0])
#define NE64 (sizeof edge64 / sizeof edge64[0])

static u32 pick32(u32 near) {
    u32 r = (u32)rnd();
    switch (rnd() % 7) {
    case 0: return edge32[rnd() % NE32];
    case 1: return r & 0x807FFFFF;
    case 2: return (near & 0xFF800000) | (r & 0x007FFFFF);
    case 3: return (near + (u32)(rnd() % 7) - 3) ^ (r & 0x80000000);
    case 4: return bf((float)(int)(r >> (rnd() % 31)));
    default: return r;
    }
}
static u64 pick64(u64 near) {
    u64 r = rnd();
    switch (rnd() % 7) {
    case 0: return edge64[rnd() % NE64];
    case 1: return r & 0x800FFFFFFFFFFFFFull;
    case 2: return (near & 0xFFF0000000000000ull) | (r & 0x000FFFFFFFFFFFFFull);
    case 3: return (near + rnd() % 7 - 3) ^ (r & 0x8000000000000000ull);
    case 4: return bd((double)(long long)(r >> (rnd() % 63)));
    default: return r;
    }
}

static void out(int op, u64 a, u64 b, u64 r) {
    printf("%x %x %x %x %x %x %x\n", op, (u32)(a >> 32), (u32)a, (u32)(b >> 32), (u32)b, (u32)(r >> 32), (u32)r);
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 250, i, op;
    u32 a, b; u64 c, d;
    volatile float x, y; volatile double p, q;
    for (op = 1; op <= 30; op++) {
        for (i = 0; i < n; i++) {
            a = pick32((u32)rnd()); b = pick32(a); x = fb(a); y = fb(b);
            c = pick64(rnd()); d = pick64(c); p = db(c); q = db(d);
            switch (op) {
            case 1: out(op, a, b, bf(x + y)); break;
            case 2: out(op, a, b, bf(x - y)); break;
            case 3: out(op, a, b, bf(x * y)); break;
            case 4: out(op, a, b, bf(x / y)); break;
            case 5: out(op, a, 0, bf(sqrtf(x))); break;
            case 6: out(op, a, b, (u64)(long long)(x != x || y != y ? 2 : x < y ? -1 : x > y ? 1 : 0)); break;
            case 7: out(op, a, 0, bd((double)x)); break;
            case 8: if (x == x && fabsf(x) < 9.2e18f) out(op, a, 0, (u64)(long long)x); break;
            case 9: if (x == x && fabsf(x) < 2.1e9f) out(op, a, 0, (u64)(long long)(int)x); break;
            case 10: if (x >= 0 && x < 1.8e19f) out(op, a, 0, (u64)x); break;
            case 11: out(op, c, d, bd(p + q)); break;
            case 12: out(op, c, d, bd(p - q)); break;
            case 13: out(op, c, d, bd(p * q)); break;
            case 14: out(op, c, d, bd(p / q)); break;
            case 15: out(op, c, 0, bd(sqrt(p))); break;
            case 16: out(op, c, d, (u64)(long long)(p != p || q != q ? 2 : p < q ? -1 : p > q ? 1 : 0)); break;
            case 17: out(op, c, 0, bf((float)p)); break;
            case 18: if (p == p && fabs(p) < 9.2e18) out(op, c, 0, (u64)(long long)p); break;
            case 19: if (p == p && fabs(p) < 2.1e9) out(op, c, 0, (u64)(long long)(int)p); break;
            case 20: if (p >= 0 && p < 1.8e19) out(op, c, 0, (u64)p); break;
            case 21: c = rnd() >> (rnd() % 64); { volatile long long v = (long long)c; out(op, c, 0, bf((float)v)); } break;
            case 22: c = rnd() >> (rnd() % 64); { volatile u64 v = c; out(op, c, 0, bf((float)v)); } break;
            case 23: c = (u64)(long long)(int)(rnd() >> (rnd() % 64)); { volatile int v = (int)c; out(op, c, 0, bf((float)v)); } break;
            case 24: c = (u32)(rnd() >> (rnd() % 64)); { volatile u32 v = (u32)c; out(op, c, 0, bf((float)v)); } break;
            case 25: c = rnd() >> (rnd() % 64); { volatile long long v = (long long)c; out(op, c, 0, bd((double)v)); } break;
            case 26: c = rnd() >> (rnd() % 64); { volatile u64 v = c; out(op, c, 0, bd((double)v)); } break;
            case 27: c = (u64)(long long)(int)(rnd() >> (rnd() % 64)); { volatile int v = (int)c; out(op, c, 0, bd((double)v)); } break;
            case 28: c = (u32)(rnd() >> (rnd() % 64)); { volatile u32 v = (u32)c; out(op, c, 0, bd((double)v)); } break;
            case 29: if (i < NE32) out(op, edge32[i], 0, bf(sqrtf(fb(edge32[i])))); break;
            case 30: if (i < NE64) out(op, edge64[i], 0, bd(sqrt(db(edge64[i])))); break;
            }
        }
    }
    return 0;
}
