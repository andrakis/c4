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

static __attribute__((unused)) i32 to_int(double v) {
  if (v >= -2147483648.0 && v < 2147483648.0) return (i32)v;   /* toward zero */
  return (i32)0x80000000;
}
/* ... into the machine's word: on the 64-bit machine softfp.c's
 * __c4f_to_ll / __c4d_to_ll, the most negative for what does not fit */
static word to_word(double v) {
#if WB == 8
  if (v >= -9223372036854775808.0 && v < 9223372036854775808.0) return (word)v;
  return INT64_MIN;
#else
  return to_int(v);
#endif
}
/* a word as a float's bits, once rounded (the 64-bit word's is not exact in double) */
static i32 itof(word a) {
#if WB == 8
  return bits_of_f32((float)a);
#else
  return bits_of_f32(fround((double)a));
#endif
}
/* a float's bits as the word: zero above them (c4m_float.c) */
#define FW(bits) ((word)(u32)(bits))
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

word fpu_float(word op, word a, word b) {
  if (op <= 5) return FW(farith((i32)op, (u32)a, (u32)b));
  if (op == 6) return cmp(f32_of((i32)a), f32_of((i32)b));
  if (op == 7) return FW(itof(a));
  return to_word(f32_of((i32)a));
}

/* the double sub-ops on plain memory: a, b, r byte addresses in mem (a
 * the operand itself for 23 and 25), each 8 bytes, the low word first.
 * fpu.js fpuDouble. */
static void mrd(const u8 *mem, uword p, du *x) { memcpy(x, mem + p, 8); }
static void mwr(u8 *mem, uword p, u32 lo, u32 hi) { memcpy(mem + p, &lo, 4); memcpy(mem + p + 4, &hi, 4); }
int fpu_double_plain(u8 *mem, word op, word a, uword ib, uword ir, word *res) {
  du x, y, c;
  *res = 0;
  if (op == 23) { c.d = (double)a; mwr(mem, ir, c.w[0], c.w[1]); return 1; }
  if (op == 25) {
    u32 u = (u32)a;
    if (fnan(u)) {
      u32 sig = u & 0x7fffff;
      mwr(mem, ir, (sig & 7) << 29, (u & 0x80000000) | 0x7ff80000 | (sig >> 3));
    } else { c.d = f32_of((i32)a); mwr(mem, ir, c.w[0], c.w[1]); }
    return 1;
  }
  mrd(mem, (uword)a, &x);
  if (op == 24) { *res = to_word(x.d); return 1; }
  if (op == 26) {
    if (dnan(x.w[0], x.w[1])) *res = FW((x.w[1] & 0x80000000) | 0x7fc00000 | ((x.w[1] & 0xfffff) << 3) | (x.w[0] >> 29));
    else *res = FW(bits_of_f32(fround(x.d)));
    return 1;
  }
  mrd(mem, ib, &y);
  if (op == 22) { *res = cmp(x.d, y.d); return 1; }
  if (dnan(x.w[0], x.w[1])) { mwr(mem, ir, x.w[0], x.w[1] | 0x00080000); return 1; }
  if (dnan(y.w[0], y.w[1])) { mwr(mem, ir, y.w[0], y.w[1] | 0x00080000); return 1; }
  c.d = op == 17 ? x.d + y.d : op == 18 ? x.d - y.d : op == 19 ? x.d * y.d : x.d / y.d;
  if (c.d != c.d) { mwr(mem, ir, 0, 0xfff80000); return 1; }
  mwr(mem, ir, c.w[0], c.w[1]);
  return 1;
}

/* through the machine's loads and stores, for any address (fpu.js fpu):
 * a double is two words on the 32-bit machine, one on the 64-bit */
static void dload(machine *m, word p, du *d) {
#if WB == 8
  uword v = (uword)m_ldw(m, p); d->w[0] = (u32)v; d->w[1] = (u32)(v >> 32);
#else
  d->w[0] = (u32)m_ldw(m, p); d->w[1] = (u32)m_ldw(m, p + 4);
#endif
}
static void dstore(machine *m, word p, u32 lo, u32 hi) {
#if WB == 8
  m_stw(m, p, (word)((uword)hi << 32 | lo));
#else
  m_stw(m, p, (i32)lo); m_stw(m, p + 4, (i32)hi);
#endif
}

int fpu_op(machine *m, uword sp, word *res) {
  word op = m_ldw(m, (word)sp), a = m_ldw(m, (word)sp + WB), b = m_ldw(m, (word)sp + 2 * WB), r = m_ldw(m, (word)sp + 3 * WB);
  du x, y, c;
  *res = 0;
  switch (op) {
    case 1: case 2: case 3: case 4: case 5: *res = FW(farith((i32)op, (u32)a, (u32)b)); return 1;
    case 6: *res = cmp(f32_of((i32)a), f32_of((i32)b)); return 1;
    case 7: *res = FW(itof(a)); return 1;
    case 8: *res = to_word(f32_of((i32)a)); return 1;
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
    case 24: dload(m, a, &x); *res = to_word(x.d); return 1;
    case 25: {
      u32 u = (u32)a;
      if (fnan(u)) { u32 sig = u & 0x7fffff; dstore(m, r, (sig & 7) << 29, (u & 0x80000000) | 0x7ff80000 | (sig >> 3)); }
      else { c.d = f32_of((i32)a); dstore(m, r, c.w[0], c.w[1]); }
      return 1;
    }
    case 26:
      dload(m, a, &x);
      if (dnan(x.w[0], x.w[1])) *res = FW((x.w[1] & 0x80000000) | 0x7fc00000 | ((x.w[1] & 0xfffff) << 3) | (x.w[0] >> 29));
      else *res = FW(bits_of_f32(fround(x.d)));
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
word flt_op(machine *m, uword sp) {
  word op = m_ldw(m, (word)sp);
  word ia = m_ldw(m, (word)sp + WB), ib = m_ldw(m, (word)sp + 2 * WB);
  double a = f32_of((i32)ia), b = f32_of((i32)ib), c;
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
#if WB == 8
    case 9: return FW(itof(ia));
    case 10: return to_word(a);                    /* native c4m's (int)a */
#else
    case 9: c = (double)ia; break;
    case 10: { double t = a; if (t != t) return 0; if (t >= 2147483648.0 || t <= -2147483649.0) { /* JS: trunc then |0 wraps */
        double q = t - 4294967296.0 * (double)(i64)(t / 4294967296.0); return (i32)(u32)(i64)q; }
      return (i32)t; }
#endif
    default:
      machine_emits(m, "c4m fatal error: unsupported floating point instruction ");
      c = 0;
      break;
  }
  return FW(bits_of_f32(fround(c)));
}
