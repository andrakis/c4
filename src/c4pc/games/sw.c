/* sw.c -- Shadow Warrior's accelerators (games/sw/c4/accel.mjs) in C:
 * JFBuild's innermost loops (a-c.c) and its 64-bit helpers (pragmas.h).
 * c4/c4accel.h is the game's side (SA_*); each does what accel.mjs does,
 * the same pixels in the same order. */
#include "../accel.h"
#if WB == 4                            /* the 32-bit machine's games (c4pc.h) */

/* The translucent loops blend what is on the screen: every pixel worked
 * out first, every page they will write made sure of as it is first met,
 * and only then written (as duke3d.c's) */
typedef struct { machine *m; u32 page; } later_t;
static void later_init(later_t *l, machine *m) { l->m = m; l->page = 0xffffffffu; hold_begin(&acc_hold); }
static inline void later_put(later_t *l, i32 a, i32 b) {
  if (((u32)a >> 12) != l->page) { acc_probe(l->m, a, 1, 1); l->page = (u32)a >> 12; }
  hold_put(&acc_holds[l->m->acc_k], a, b);
}
static void later_commit(later_t *l) { hold_write8(l->m, &acc_holds[l->m->acc_k]); }
/* transmode 1: trans[*p + (c << 8)]; 0: trans[(*p << 8) + c] */
static inline i32 tidx(i32 trans, i32 d, i32 c, i32 mode) { return mode ? trans + d + (c << 8) : trans + (d << 8) + c; }

/* SA_VLINE, SA_MVLINE (vinc, pal, cnt, vplc, buf, p, logy, bpl):
 * vlineasm1, mvlineasm1, for (; cnt >= 0; cnt--) */
static i32 vline(machine *m, int masked) {
  i32 a[8]; acc_args(m, 8, a);
  i32 inc = a[0], pal = a[1], n = a[2], v = a[3], buf = a[4], p = a[5], logy = a[6], bpl = a[7];
  RW(rt); RW(rp); RW(wr);
  for (; n >= 0; n--, p += bpl, v += inc) {
    i32 c = rd8(&rt, buf + (i32)LSR(v, logy));
    if (!masked || c != 255) wr8(&wr, p, rd8(&rp, pal + c));
  }
  return 0;
}
static i32 vline0(machine *m) { return vline(m, 0); }
static i32 vline1(machine *m) { return vline(m, 1); }

/* SA_TVLINE (vinc, pal, cnt, vplc, buf, p, logy, bpl, trans, transmode) */
static i32 tvline(machine *m) {
  i32 a[10]; acc_args(m, 10, a);
  i32 inc = a[0], pal = a[1], n = a[2], v = a[3], buf = a[4], p = a[5], logy = a[6], bpl = a[7], trans = a[8],
      mode = a[9];
  RW(rt); RW(rp); RW(rd); RW(rl);
  later_t out; later_init(&out, m);
  for (; n >= 0; n--, p += bpl, v += inc) {
    i32 c = rd8(&rt, buf + (i32)LSR(v, logy));
    if (c != 255) later_put(&out, p, rd8(&rl, tidx(trans, rd8(&rd, p), rd8(&rp, pal + c), mode)));
  }
  later_commit(&out);
  return 0;
}

/* SA_HLINE (cnt, pal, by, bx, buf, p, logx, logy, bxinc, byinc): hlineasm4,
 * right to left */
static i32 hline(machine *m) {
  i32 a[10]; acc_args(m, 10, a);
  i32 n = a[0], pal = a[1], by = a[2], bx = a[3], buf = a[4], p = a[5], logx = a[6], logy = a[7], bxinc = a[8],
      byinc = a[9];
  RW(rt); RW(rp); RW(wr);
  for (; n >= 0; n--, p--, bx -= bxinc, by -= byinc)
    wr8(&wr, p, rd8(&rp, pal + rd8(&rt, buf + (i32)((u32)SHL(LSR(bx, 32 - logx), logy) + LSR(by, 32 - logy)))));
  return 0;
}

/* SA_MHLINE, SA_THLINE (buf, bx, cnt, by, p, logx, logy, pal, asm1, asm2,
 * trans, transmode): mhline, thline, left to right */
static i32 mhline(machine *m, int t) {
  i32 a[12]; acc_args(m, 12, a);
  i32 buf = a[0], bx = a[1], n = a[2], by = a[3], p = a[4], logx = a[5], logy = a[6], pal = a[7], ax = a[8], ay = a[9],
      trans = a[10], mode = a[11];
  RW(rt); RW(rp); RW(rd); RW(rl); RW(wr);
  later_t out; if (t) later_init(&out, m);
  for (; n > 0; n--, p++, bx += ax, by += ay) {
    i32 c = rd8(&rt, buf + (i32)((u32)SHL(LSR(bx, 32 - logx), logy) + LSR(by, 32 - logy)));
    if (c == 255) continue;
    if (t) later_put(&out, p, rd8(&rl, tidx(trans, rd8(&rd, p), rd8(&rp, pal + c), mode)));
    else wr8(&wr, p, rd8(&rp, pal + c));
  }
  if (t) later_commit(&out);
  return 0;
}
static i32 mhline0(machine *m) { return mhline(m, 0); }
static i32 mhline1(machine *m) { return mhline(m, 1); }

/* SA_SPRV, SA_MSPRV, SA_TSPRV (bx, by, cnt, buf, p, pal, bxinc, byinc, ysiz,
 * bpl, trans, transmode): spritevline, mspritevline, tspritevline */
static i32 sprv(machine *m, int kind) {
  i32 a[12]; acc_args(m, 12, a);
  i32 bx = a[0], by = a[1], n = a[2], buf = a[3], p = a[4], pal = a[5], bxinc = a[6], byinc = a[7], ysiz = a[8],
      bpl = a[9], trans = a[10], mode = a[11];
  RW(rt); RW(rp); RW(rd); RW(rl); RW(wr);
  later_t out; if (kind == 2) later_init(&out, m);
  for (; n > 1; n--, bx += bxinc, by += byinc, p += bpl) {
    i32 c = rd8(&rt, buf + IMUL(bx >> 16, ysiz) + (by >> 16));
    if (kind == 0) wr8(&wr, p, rd8(&rp, pal + c));
    else if (c == 255) continue;
    else if (kind == 1) wr8(&wr, p, rd8(&rp, pal + c));
    else later_put(&out, p, rd8(&rl, tidx(trans, rd8(&rd, p), rd8(&rp, pal + c), mode)));
  }
  if (kind == 2) later_commit(&out);
  return 0;
}
static i32 sprv0(machine *m) { return sprv(m, 0); }
static i32 sprv1(machine *m) { return sprv(m, 1); }
static i32 sprv2(machine *m) { return sprv(m, 2); }

/* SA_SLOPE (p, slopal, cnt, bx, by, bz, bzinc, gx3, gy3, buf, logx, logy,
 * pinc, recip): slopevlin, krecip's reciprocal from the table by (float)i's
 * bits, as c4's FPU rounds it */
static i32 slope(machine *m) {
  i32 a[14]; acc_args(m, 14, a);
  i32 p = a[0], sp = a[1], n = a[2], bx = a[3], by = a[4], bz = a[5], bzinc = a[6], gx3 = a[7], gy3 = a[8], buf = a[9],
      logx = a[10], logy = a[11], pinc = a[12], recip = a[13];
  RW(rt); RW(rp); RW(r32); RW(rr); RW(wr);
  for (; n > 0; n--, sp -= 4, p += pinc) {
    i32 u = fbits((double)(bz >> 6));
    i32 i = (SAR((i32)rd32(&rr, recip + 4 * ((u >> 12) & 2047)), (u - 0x3f800000) >> 23)) ^ (u >> 31);
    bz += bzinc;
    u32 x = (u32)bx + (u32)IMUL(gx3, i), y = (u32)by + (u32)IMUL(gy3, i);
    wr8(&wr, p, rd8(&rp, (i32)rd32(&r32, sp) + rd8(&rt, buf + (i32)((u32)SHL(LSR(x, 32 - logx), logy) + LSR(y, 32 - logy)))));
  }
  return 0;
}

/* the 64-bit helpers, as int64_t: the products' sum in u64, so that no
 * signed overflow is C's undefined behaviour, then shifted */
static inline i32 shr64(u64 v, i32 s) { return (i32)(u32)(u64)((i64)v >> (s & 63)); }
static i32 mulscale(machine *m) {
  i32 a[3]; acc_args(m, 3, a);
  return shr64((u64)((i64)a[0] * a[1]), a[2]);
}
static i32 dmulscale(machine *m) {
  i32 a[5]; acc_args(m, 5, a);
  return shr64((u64)((i64)a[0] * a[1]) + (u64)((i64)a[2] * a[3]), a[4]);
}
static i32 tmulscale(machine *m) {
  i32 a[7]; acc_args(m, 7, a);
  return shr64((u64)((i64)a[0] * a[1]) + (u64)((i64)a[2] * a[3]) + (u64)((i64)a[4] * a[5]), a[6]);
}
/* SA_SCALE (a, b, c): (int32_t)((int64_t)a * b / c), toward zero; 0 for c 0 */
static i32 scale(machine *m) {
  i32 a[3]; acc_args(m, 3, a);
  if (!a[2]) return 0;
  return (i32)(u32)(u64)(((i64)a[0] * a[1]) / a[2]);
}
/* SA_DIVSCALE (a, b, s): (int32_t)(((int64_t)a << s) / b); 0 for b 0 */
static i32 divscale(machine *m) {
  i32 a[3]; acc_args(m, 3, a);
  if (!a[1]) return 0;
  i64 x = (i64)((u64)(i64)a[0] << (a[2] & 63));
  if (a[1] == -1) return (i32)(u32)(0 - (u64)x);
  return (i32)(u32)(u64)(x / a[1]);
}

const acc_fn acc_sw[] = {vline0, vline1, tvline, hline, mhline0, mhline1, sprv0, sprv1, sprv2, slope,
                         mulscale, dmulscale, tmulscale, scale, divscale};
const int acc_sw_n = sizeof acc_sw / sizeof acc_sw[0];
#endif
