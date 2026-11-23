/* doom.c -- Doom's accelerators (games/doom/c4/accel.mjs) in C: the
 * renderer's innermost loops, the patches' copy, the timedemo's checksum
 * and m_fixed.c's FixedMul and FixedDiv. c4/c4accel.h is the game's side,
 * with each operation's number (DA_*); each does exactly what the game's C
 * does, as accel.mjs has it line for line. */
#include "../accel.h"
#if WB == 4                            /* the 32-bit machine's games (c4pc.h) */

/* DA_COL (n, frac, step, mask, src, cmap, trans, dest, stride, d2): a
 * column, R_DrawColumn and its low and translated kinds */
static i32 col(machine *m) {
  i32 a[10]; acc_args(m, 10, a);
  i32 n = a[0], frac = a[1], step = a[2], mask = a[3], src = a[4], cmap = a[5], trans = a[6], dest = a[7],
      stride = a[8], d2 = a[9];
  RW(rs); RW(rt); RW(rc); RW(wr);
  for (; n > 0; n--, dest += stride, frac += step) {
    i32 t = rd8(&rs, src + ((frac >> 16) & mask));
    if (trans) t = rd8(&rt, trans + t);
    i32 c = rd8(&rc, cmap + t);
    wr8(&wr, dest, c);
    if (d2) wr8(&wr, dest + d2, c);
  }
  return 0;
}

/* DA_FUZZ (n, dest, stride, cmap, fuzzofs, pfuzzpos, size, d2): the
 * shadow, which reads the rows it may have written: every pixel worked
 * out first, the pages it writes made sure of, then written */
static i32 fuzz(machine *m) {
  i32 a[8]; acc_args(m, 8, a);
  i32 n = a[0], dest = a[1], stride = a[2], cmap = a[3], fofs = a[4], pfp = a[5], size = a[6], d2 = a[7];
  if (n <= 0) return 0;
  RW(rd); RW(rc); RW(r32);
  hold_t *h = &acc_hold; hold_begin(h);
  i32 fp = rd32(&r32, pfp), d = dest, v;
  for (i32 i = 0; i < n; i++, d += stride) {
    i32 off = rd32(&r32, fofs + 4 * fp);
    if (!hold_get(h, d + off, &v)) v = rd8(&rd, d + off);
    hold_put(h, d, rd8(&rc, cmap + v));
    if (d2) {
      i32 e = d + d2;
      if (!hold_get(h, e + off, &v)) v = rd8(&rd, e + off);
      hold_put(h, e, rd8(&rc, cmap + v));
    }
    if (++fp == size) fp = 0;
  }
  hold_probe(m, h, 1);
  acc_probe(m, pfp, 4, 1);
  hold_write8(m, h);
  RW(w32); wr32(&w32, pfp, fp);
  return 0;
}

/* DA_SPAN (n, position, step, src, cmap, dest, dbl): a floor's or
 * ceiling's row out of the 64x64 flat, twice a pixel for dbl */
static i32 spanop(machine *m) {
  i32 a[7]; acc_args(m, 7, a);
  i32 n = a[0], pos = a[1], step = a[2], src = a[3], cmap = a[4], dest = a[5], dbl = a[6];
  RW(rs); RW(rc); RW(wr);
  for (; n > 0; n--, pos += step) {
    i32 c = rd8(&rc, cmap + rd8(&rs, src + (i32)((LSR(pos, 4) & 0xfc0) | LSR(pos, 26))));
    wr8(&wr, dest, c); dest++;
    if (dbl) { wr8(&wr, dest, c); dest++; }
  }
  return 0;
}

/* DA_SUM (s, pal, pixels): dg-sum.c's checksum of a timedemo's frame */
static i32 sum(machine *m) {
  i32 a[3]; acc_args(m, 3, a);
  i32 s = a[0], pal = a[1], pix = a[2];
  RW(r32);
  for (i32 i = 0; i < 256; i++) s = IMUL(s, 31) + rd32(&r32, pal + 4 * i);
  const i32 N = 320 * 200;
  for (i32 i = 0; i < N; ) {
    i32 at = pix + i, k = N - i, room = 4096 - (at & 4095);
    if (room < k) k = room;
    u8 *v = acc_span(m, at, k, 0);
    if (v && (k & 3) == 0) {
      for (i32 j = 0; j < k; j += 4) s = IMUL(s, 31) + (i32)(v[j] | v[j + 1] << 8 | v[j + 2] << 16 | (u32)v[j + 3] << 24);
      i += k;
    } else {
      s = IMUL(s, 31) + rd32(&r32, at);
      i += 4;
    }
  }
  return s;
}

/* DA_FIXMUL (a, b): ((int64_t)a * b) >> 16, as m_fixed.c's 32-bit pieces */
static i32 fixmul(machine *m) {
  i32 x[2]; acc_args(m, 2, x);
  i32 ah = x[0] >> 16, al = x[0] & 0xffff, bh = x[1] >> 16, bl = x[1] & 0xffff;
  return (i32)(((u32)IMUL(ah, bh) << 16) + (u32)IMUL(ah, bl) + (u32)IMUL(al, bh) + (((u32)al * (u32)bl) >> 16));
}

/* DA_FIXDIV (a, b): INT_MAX or INT_MIN where abs(a) >> 14 >= abs(b) (abs
 * as c4's, -INT_MIN being INT_MIN), else ((int64_t)a << 16) / b */
static i32 abs32(i32 x) { return x < 0 ? (i32)(0u - (u32)x) : x; }
static i32 fixdiv(machine *m) {
  i32 x[2]; acc_args(m, 2, x);
  i32 a = x[0], b = x[1];
  if ((abs32(a) >> 14) >= abs32(b)) return (a ^ b) < 0 ? (i32)0x80000000u : 0x7fffffff;
  return (i32)(((i64)a * 65536) / b);
}

/* DA_PATCH (patch, desttop, stride, w, flip): V_DrawPatch's columns */
static i32 patch(machine *m) {
  i32 a[5]; acc_args(m, 5, a);
  i32 p = a[0], desttop = a[1], stride = a[2], w = a[3], flip = a[4];
  RW(rp); RW(rs); RW(r32); RW(wr);
  for (i32 c = 0; c < w; c++, desttop++) {
    i32 column = p + rd32(&r32, p + 8 + 4 * (flip ? w - 1 - c : c)), td;
    while ((td = rd8(&rp, column)) != 0xff) {
      i32 len = rd8(&rp, column + 1);
      i32 src = column + 3, dest = desttop + IMUL(td, stride);
      for (i32 k = len; k > 0; k--, src++, dest += stride) wr8(&wr, dest, rd8(&rs, src));
      column = column + len + 4;
    }
  }
  return 0;
}

const acc_fn acc_doom[] = {col, fuzz, spanop, sum, fixmul, fixdiv, patch};
const int acc_doom_n = sizeof acc_doom / sizeof acc_doom[0];
#endif
