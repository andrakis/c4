#include "fpu.h"

// See fpu.h for the shared contract (raw float32 bit patterns in/out).

#ifdef C4OR1K_NATIVE_FLOAT
// ---------------------------------------------------------------------
// Native build: real host floats. `int` is `long` here (native.h), so
// the bit-reinterpret union deliberately uses `unsigned`/`float` (both
// genuinely 32-bit -- the `#define int long` only rewrites the `int`
// token, not `unsigned`). No libm: ftoi's floor is done with a compare
// so the native link needs no -lm.

union fpu_bits { float f; unsigned u; };

static float fpu_b2f(int bits) { union fpu_bits x; x.u = (unsigned)(bits & 0xFFFFFFFF); return x.f; }
static int   fpu_f2b(float f)  { union fpu_bits x; x.f = f; return (int)(x.u); }

int fpu_add(int a, int b) { return fpu_f2b(fpu_b2f(a) + fpu_b2f(b)); }
int fpu_sub(int a, int b) { return fpu_f2b(fpu_b2f(a) - fpu_b2f(b)); }
int fpu_mul(int a, int b) { return fpu_f2b(fpu_b2f(a) * fpu_b2f(b)); }
int fpu_div(int a, int b) { return fpu_f2b(fpu_b2f(a) / fpu_b2f(b)); }
int fpu_madd(int d, int a, int b) { return fpu_f2b(fpu_b2f(d) + fpu_b2f(a) * fpu_b2f(b)); }

int fpu_itof(int i) { return fpu_f2b((float)i); } // i is already sign-extended by cpu.c

int fpu_ftoi(int a) {
    float f;
    int t;
    f = fpu_b2f(a);
    t = (int)f;                     // truncate toward zero
    if (f < (float)t) t = t - 1;    // -> floor, matching jor1k's floor()
    return t;
}

int fpu_eq(int a, int b) { return fpu_b2f(a) == fpu_b2f(b); }
int fpu_ne(int a, int b) { return fpu_b2f(a) != fpu_b2f(b); }
int fpu_gt(int a, int b) { return fpu_b2f(a) >  fpu_b2f(b); }
int fpu_ge(int a, int b) { return fpu_b2f(a) >= fpu_b2f(b); }
int fpu_lt(int a, int b) { return fpu_b2f(a) <  fpu_b2f(b); }
int fpu_le(int a, int b) { return fpu_b2f(a) <= fpu_b2f(b); }

#else
// ---------------------------------------------------------------------
// Hosted build: no float type in the c4lc dialect. FP is native-only
// for now; a lf.* reached under a hosted image warns once and yields 0.
// This never fires on the tested hosted paths.

int fpu_hosted_warned;

int fpu_hosted_stub() {
    if (!fpu_hosted_warned) {
        fpu_hosted_warned = 1;
        printf("c4or1k: floating point (lf.*) is native-only in this build; hosted result is 0\n");
    }
    return 0;
}

int fpu_add(int a, int b) { return fpu_hosted_stub(); }
int fpu_sub(int a, int b) { return fpu_hosted_stub(); }
int fpu_mul(int a, int b) { return fpu_hosted_stub(); }
int fpu_div(int a, int b) { return fpu_hosted_stub(); }
int fpu_madd(int d, int a, int b) { return fpu_hosted_stub(); }
int fpu_itof(int i) { return fpu_hosted_stub(); }
int fpu_ftoi(int a) { return fpu_hosted_stub(); }
int fpu_eq(int a, int b) { return fpu_hosted_stub(); }
int fpu_ne(int a, int b) { return fpu_hosted_stub(); }
int fpu_gt(int a, int b) { return fpu_hosted_stub(); }
int fpu_ge(int a, int b) { return fpu_hosted_stub(); }
int fpu_lt(int a, int b) { return fpu_hosted_stub(); }
int fpu_le(int a, int b) { return fpu_hosted_stub(); }

#endif
