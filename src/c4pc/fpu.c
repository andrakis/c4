/* fpu.c -- the FPU opcode (89, F3; docs/c4-float.md) and the old FLT
 * opcode: src/c4bb/sim/fpu.js and c4m.js flt() in C.
 *
 * The answers are softfp.c's to the bit. A float's arithmetic is done in
 * double and rounded to single, which is exact for + - * / and sqrt. On
 * the PC the x87 is set to round to double (pc/pc.c), so a double's are
 * IEEE's too; NaNs follow SSE, as fpu.js has them.
 */
#include "c4pc.h"
#include "clib.h"

typedef union { float f; u32 u; } fu;
typedef union { double d; u32 w[2]; } du;   /* w[0] the low word: little-endian */

float f32_of(i32 bits) { fu x; x.u = (u32)bits; return x.f; }
i32 bits_of_f32(float f) { volatile fu x; x.f = f; return (i32)x.u; }
static float fround(double d) { volatile float f = (float)d; return f; }

static int fnan(u32 u) { return (u & 0x7fffffff) > 0x7f800000; }
static int dnan(u32 lo, u32 hi) { return (hi & 0x7fffffff) > 0x7ff00000 || ((hi & 0x7fffffff) == 0x7ff00000 && lo != 0); }
#define FDEF ((i32)0xffc00000)

static i32 to_int(double v) {
  if (v >= -2147483648.0 && v < 2147483648.0) return (i32)v;   /* toward zero */
  return (i32)0x80000000;
}
static double dsqrt(double x) { return __builtin_sqrt(x); }

static i32 farith(i32 op, u32 ua, u32 ub) {
  if (fnan(ua)) return (i32)(ua | 0x00400000);
  if (op != 5 && fnan(ub)) return (i32)(ub | 0x00400000);
  double a = f32_of((i32)ua), b = f32_of((i32)ub), c;
  if (op == 1) c = a + b;
  else if (op == 2) c = a - b;
  else if (op == 3) c = a * b;
  else if (op == 4) c = a / b;
  else c = dsqrt(a);
  if (c != c) return FDEF;
  return bits_of_f32(fround(c));
}
static int cmp(double a, double b) { return (a != a || b != b) ? 2 : a < b ? -1 : a > b ? 1 : 0; }

i32 fpu_float(i32 op, i32 a, i32 b) {
  if (op <= 5) return farith(op, (u32)a, (u32)b);
  if (op == 6) return cmp(f32_of(a), f32_of(b));
  if (op == 7) return bits_of_f32(fround((double)a));
  return to_int(f32_of(a));
}

/* the double sub-ops on words already found: w the arena's words, a, ib,
 * ir word indexes (a the word itself for 23 and 25). fpu.js fpuDouble. */
int fpu_double_plain(i32 *w, i32 op, i32 a, i32 ib, i32 ir, i32 *res) {
  du x, y, c;
  *res = 0;
  if (op == 23) { c.d = (double)a; w[ir] = (i32)c.w[0]; w[ir + 1] = (i32)c.w[1]; return 1; }
  if (op == 25) {
    u32 u = (u32)a;
    if (fnan(u)) {
      u32 sig = u & 0x7fffff;
      w[ir] = (i32)((sig & 7) << 29);
      w[ir + 1] = (i32)((u & 0x80000000) | 0x7ff80000 | (sig >> 3));
    } else { c.d = f32_of(a); w[ir] = (i32)c.w[0]; w[ir + 1] = (i32)c.w[1]; }
    return 1;
  }
  x.w[0] = (u32)w[a]; x.w[1] = (u32)w[a + 1];
  if (op == 24) { *res = to_int(x.d); return 1; }
  if (op == 26) {
    if (dnan(x.w[0], x.w[1])) *res = (i32)((x.w[1] & 0x80000000) | 0x7fc00000 | ((x.w[1] & 0xfffff) << 3) | (x.w[0] >> 29));
    else *res = bits_of_f32(fround(x.d));
    return 1;
  }
  y.w[0] = (u32)w[ib]; y.w[1] = (u32)w[ib + 1];
  if (op == 22) { *res = cmp(x.d, y.d); return 1; }
  if (dnan(x.w[0], x.w[1])) { w[ir] = (i32)x.w[0]; w[ir + 1] = (i32)(x.w[1] | 0x00080000); return 1; }
  if (dnan(y.w[0], y.w[1])) { w[ir] = (i32)y.w[0]; w[ir + 1] = (i32)(y.w[1] | 0x00080000); return 1; }
  c.d = op == 17 ? x.d + y.d : op == 18 ? x.d - y.d : op == 19 ? x.d * y.d : x.d / y.d;
  if (c.d != c.d) { w[ir] = 0; w[ir + 1] = (i32)0xfff80000; return 1; }
  w[ir] = (i32)c.w[0]; w[ir + 1] = (i32)c.w[1];
  return 1;
}

/* through the machine's loads and stores, for any address (fpu.js fpu) */
i32 m_ld32(machine *m, i32 x);
void m_st32(machine *m, i32 x, i32 v);

static void dload(machine *m, i32 p, du *d) { d->w[0] = (u32)m_ld32(m, p); d->w[1] = (u32)m_ld32(m, p + 4); }
static void dstore(machine *m, i32 p, u32 lo, u32 hi) { m_st32(m, p, (i32)lo); m_st32(m, p + 4, (i32)hi); }

int fpu_op(machine *m, u32 sp, i32 *res) {
  i32 op = m_ld32(m, (i32)sp), a = m_ld32(m, (i32)sp + 4), b = m_ld32(m, (i32)sp + 8), r = m_ld32(m, (i32)sp + 12);
  du x, y, c;
  *res = 0;
  switch (op) {
    case 1: case 2: case 3: case 4: case 5: *res = farith(op, (u32)a, (u32)b); return 1;
    case 6: *res = cmp(f32_of(a), f32_of(b)); return 1;
    case 7: *res = bits_of_f32(fround((double)a)); return 1;
    case 8: *res = to_int(f32_of(a)); return 1;
    case 17: case 18: case 19: case 20: case 21:
      dload(m, a, &x);
      if (dnan(x.w[0], x.w[1])) { dstore(m, r, x.w[0], x.w[1] | 0x00080000); return 1; }
      y.d = 0;
      if (op != 21) {
        dload(m, b, &y);
        if (dnan(y.w[0], y.w[1])) { dstore(m, r, y.w[0], y.w[1] | 0x00080000); return 1; }
      }
      c.d = op == 17 ? x.d + y.d : op == 18 ? x.d - y.d : op == 19 ? x.d * y.d : op == 20 ? x.d / y.d : dsqrt(x.d);
      if (c.d != c.d) { dstore(m, r, 0, 0xfff80000); return 1; }
      dstore(m, r, c.w[0], c.w[1]);
      return 1;
    case 22: dload(m, a, &x); dload(m, b, &y); *res = cmp(x.d, y.d); return 1;
    case 23: c.d = (double)a; dstore(m, r, c.w[0], c.w[1]); return 1;
    case 24: dload(m, a, &x); *res = to_int(x.d); return 1;
    case 25: {
      u32 u = (u32)a;
      if (fnan(u)) { u32 sig = u & 0x7fffff; dstore(m, r, (sig & 7) << 29, (u & 0x80000000) | 0x7ff80000 | (sig >> 3)); }
      else { c.d = f32_of(a); dstore(m, r, c.w[0], c.w[1]); }
      return 1;
    }
    case 26:
      dload(m, a, &x);
      if (dnan(x.w[0], x.w[1])) *res = (i32)((x.w[1] & 0x80000000) | 0x7fc00000 | ((x.w[1] & 0xfffff) << 3) | (x.w[0] >> 29));
      else *res = bits_of_f32(fround(x.d));
      return 1;
  }
  return 0;
}

/* sin, cos and tan for FLT: the x87's own on the PC, libm's on Linux */
#ifdef C4PC_BARE
static double xsin(double x) { double r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
static double xcos(double x) { double r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
static double xtan(double x) { double r, one; __asm__("fptan" : "=t"(one), "=u"(r) : "0"(x)); (void)one; return r; }
#else
double sin(double), cos(double), tan(double);
static double xsin(double x) { return sin(x); }
static double xcos(double x) { return cos(x); }
static double xtan(double x) { return tan(x); }
#endif
/* the same for the accelerators (accel.h) */
double fpu_sin(double x) { return xsin(x); }
double fpu_cos(double x) { return xcos(x); }

void machine_emits(machine *m, const char *s);

/* c4_float_instruction (c4m_float.c:31): binary32 values in words */
i32 flt_op(machine *m, u32 sp) {
  i32 op = m_ld32(m, (i32)sp);
  i32 ia = m_ld32(m, (i32)sp + 4), ib = m_ld32(m, (i32)sp + 8);
  double a = f32_of(ia), b = f32_of(ib), c;
  switch (op) {
    case 0: c = a < 0 ? -a : a; break;
    case 1: c = a + b; break;
    case 2: c = a - b; break;
    case 3: c = a * b; break;
    case 4: c = a / b; break;
    case 5: c = -a; break;
    case 6: c = xsin(a); break;
    case 7: c = xcos(a); break;
    case 8: c = xtan(a); break;
    case 9: c = (double)ia; break;
    case 10: { double t = a; if (t != t) return 0; if (t >= 2147483648.0 || t <= -2147483649.0) { /* JS: trunc then |0 wraps */
        double q = t - 4294967296.0 * (double)(i64)(t / 4294967296.0); return (i32)(u32)(i64)q; }
      return (i32)t; }
    default:
      machine_emits(m, "c4m fatal error: unsupported floating point instruction ");
      c = 0;
      break;
  }
  return bits_of_f32(fround(c));
}
