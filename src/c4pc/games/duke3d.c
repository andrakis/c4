/* duke3d.c -- Duke Nukem 3D's accelerators (games/duke3d/c4/accel.mjs) in
 * C: the engine's innermost loops and its 64-bit helpers. c4/c4accel.h is
 * the game's side (DA_*). */
#include "../accel.h"

/* DA_TCOL, DA_TCOLM (count, v, inc, sh, tex, pal, dest, stride): a wall's
 * column, vlineasm1/4; gives v. The masked one leaves texel 255 out. */
static i32 tcol(machine *m, int masked) {
  i32 a[8]; acc_args(m, 8, a);
  i32 n = a[0], v = a[1], inc = a[2], sh = a[3] & 31, tex = a[4], pal = a[5], dest = a[6], stride = a[7];
  RW(rt); RW(rp); RW(wr);
  for (; n > 0; n--, v += inc, dest += stride) {
    i32 c = rd8(&rt, tex + (i32)((u32)v >> sh));
    if (!masked || c != 255) wr8(&wr, dest, rd8(&rp, pal + c));
  }
  return v;
}
static i32 tcol0(machine *m) { return tcol(m, 0); }
static i32 tcol1(machine *m) { return tcol(m, 1); }

/* DA_HLINE (count, a, b, da, db, sh1, bits, sh2, tex, pal, dest): hlineasm4 */
static i32 hline(machine *m) {
  i32 x[11]; acc_args(m, 11, x);
  i32 n = x[0], a = x[1], b = x[2], da = x[3], db = x[4], sh1 = x[5], bits = x[6], sh2 = x[7], tex = x[8], pal = x[9],
      dest = x[10];
  RW(rt); RW(rp); RW(wr);
  for (; n > 0; n--, dest--, a -= da, b -= db)
    wr8(&wr, dest, rd8(&rp, pal + rd8(&rt, tex + (SHL(LSR(a, sh1), bits) | (i32)LSR(b, sh2)))));
  return 0;
}

/* DA_RHLINE (n, tex, pal, p, c, s, x, y, da, db, ax, bx): rhlineasm4 */
static i32 rhline(machine *m) {
  i32 v[12]; acc_args(m, 12, v);
  i32 n = v[0], tex = v[1], pal = v[2], p = v[3], c = v[4], s = v[5], x = v[6], y = v[7], da = v[8], db = v[9],
      ax = v[10], bx = v[11];
  RW(rt); RW(rp); RW(wr);
  for (; n > 0; n--) {
    wr8(&wr, p, rd8(&rp, pal + rd8(&rt, tex)));
    tex -= c;
    if (y < bx) tex--;
    if (x < ax) tex -= s;
    x -= da;
    y -= db;
    p--;
  }
  return 0;
}

/* DA_SLOPE (dest, z, pals, left, esi, edi, sh2, sh1, mask, bpl, tex, abits,
 * stepbits, gx3, gy3, recip, pfpuasm, pasm4): slopevlin from its first
 * eight on, 1/z from a float's bits every eight */
static i32 slope(machine *m) {
  i32 v[18]; acc_args(m, 18, v);
  i32 dest = v[0], z = v[1], pals = v[2], left = v[3], esi = v[4], edi = v[5], sh2 = v[6] & 31, sh1 = v[7] & 31,
      mask = v[8], bpl = v[9], tex = v[10], abits = v[11], stepbits = v[12], gx3 = v[13], gy3 = v[14], recip = v[15],
      pfpu = v[16], pasm4 = v[17];
  double step = fval(stepbits), a = fval(abits);
  RW(rt); RW(rp); RW(wr); RW(r32); RW(w32);
  i32 u = 0, asm4 = 0;
  do {
    u = fbits(a);
    i32 sgn = u >> 31;
    i32 k = (((u >> 23) & 255) - 2) & 31;
    i32 r = (i32)LSR(rd32(&r32, recip + 4 * ((u >> 12) & 0x7ff)), k) ^ sgn;
    i32 d = r - z;
    i32 xs = IMUL(gx3, d) & ~0xff;
    i32 ystep = IMUL(gy3, d);
    i32 yhi = ystep & ~0xff;
    z = r;
    a = fr(a + step);
    asm4 = left;
    for (i32 n = left >= 8 ? 8 : (left & 0xff); n != 0; n--) {
      i32 bx = (esi >> sh2) & mask;
      esi += xs | n;
      i32 vy = (i32)((u32)edi >> sh1);
      edi += ystep;
      i32 pal = rd32(&r32, pals);
      pals -= 4;
      i32 pix = rd8(&rp, pal + ((vy & ~0xff) | rd8(&rt, tex + bx + vy)));
      ystep = yhi | pix;
      dest += bpl;
      wr8(&wr, dest, pix);
    }
    left = asm4 - 8;
  } while (left > 0);
  wr32(&w32, pfpu, u);
  wr32(&w32, pasm4, asm4);
  return 0;
}

/* (int64_t)a * b as its two words, as accel.mjs's mul64 (m1's and m2's
 * low 32 bits shifted) */
static void mul64(i32 a, i32 b, i32 *phi, u32 *plo) {
  i32 ah = a >> 16, al = a & 0xffff, bh = b >> 16, bl = b & 0xffff;
  i64 m1 = (i64)ah * bl, m2 = (i64)al * bh;
  u32 lo = (u32)al * (u32)bl;
  i32 hi = (i32)(u32)((i64)ah * bh + ((i32)(u32)m1 >> 16) + ((i32)(u32)m2 >> 16));
  u32 t = lo + ((u32)m1 << 16);
  if (t < lo) hi++;
  lo = t + ((u32)m2 << 16);
  if (lo < t) hi++;
  *phi = hi; *plo = lo;
}
/* DA_MULSCALE (a, b, s): (int32_t)(((int64_t)a * b) >> s) */
static i32 mulscale(machine *m) {
  i32 a[3]; acc_args(m, 3, a);
  i32 hi; u32 lo; mul64(a[0], a[1], &hi, &lo);
  return (i32)(LSR(lo, a[2]) | (u32)SHL(hi, 32 - a[2]));
}

/* accel.mjs's qdiv: x / b rounded down, b may be negative */
static i64 qdiv(i64 x, i64 b) {
  i64 q = x / b;
  if ((x % b) != 0 && ((x < 0) != (b < 0))) q--;      /* Math.floor(x / b) */
  if (q * b > x) q--; else if ((q + 1) * b <= x) q++;  /* its corrections, as accel.mjs */
  return q;
}
/* DA_UDIV64 (hi, lo, b), 0 < b < 2^30: hi:lo / b, the quotient's low 32 bits */
static i32 udiv64(machine *m) {
  i32 a[3]; acc_args(m, 3, a);
  i64 b = a[2];
  if (!b) return 0;
  i64 r = (i64)(u32)a[0] % b;
  i64 x = r * 65536 + ((u32)a[1] >> 16);
  i64 q1 = qdiv(x, b);
  r = x - q1 * b;
  x = r * 65536 + (a[1] & 0xffff);
  i64 q0 = qdiv(x, b);
  return (i32)(((u32)q1 << 16) | (u32)q0);
}

/* DA_QINTERP16 (buf, num, val, add): qinterpolatedown16short */
static i32 qinterp16(machine *m) {
  i32 a[4]; acc_args(m, 4, a);
  i32 p = a[0], n = a[1], val = a[2], add = a[3];
  RW(wr);
  for (; n > 0; n--, p += 2, val += add) {
    i32 h = val >> 16;
    wr8(&wr, p, h & 255);
    wr8(&wr, p + 1, (h >> 8) & 255);
  }
  return 0;
}

/* DA_CLEARB (dest, c, a): clearbufbyte */
static i32 clearb(machine *m) {
  i32 v[3]; acc_args(m, 3, v);
  i32 p = v[0], c = v[1], a = v[2];
  RW(wr);
  for (i32 z = 0; c > 0; c--, p++, z = (z + 1) & 3) wr8(&wr, p, (i32)(((u32)a >> (8 * z)) & 255));
  return 0;
}

/* DA_COPYB (src, dest, c): copybufbyte where the two do not overlap, a
 * page at a time, as one move where both sides are plain bytes */
static i32 copyb(machine *m) {
  i32 v[3]; acc_args(m, 3, v);
  i32 s = v[0], d = v[1], c = v[2];
  while (c > 0) {
    i32 n = c, k = 4096 - (s & 4095);
    if (k < n) n = k;
    k = 4096 - (d & 4095);
    if (k < n) n = k;
    u8 *a = acc_span(m, s, n, 0), *b = acc_span(m, d, n, 1);
    if (a && b) memmove(b, a, (size_t)n);
    else {
      RW(rs); RW(wr);
      for (i32 j = 0; j < n; j++) wr8(&wr, d + j, rd8(&rs, s + j));
    }
    s += n; d += n; c -= n;
  }
  return 0;
}

/* DA_COPYR (src, dest, c): copybufreverse, read down from src, written up
 * from dest */
static i32 copyr(machine *m) {
  i32 v[3]; acc_args(m, 3, v);
  i32 s = v[0], d = v[1], c = v[2];
  while (c > 0) {
    i32 n = c, k = (s & 4095) + 1;
    if (k < n) n = k;
    k = 4096 - (d & 4095);
    if (k < n) n = k;
    u8 *a = acc_span(m, s - n + 1, n, 0), *b = acc_span(m, d, n, 1);
    if (a && b) for (i32 j = 0, i = n - 1; j < n; j++, i--) b[j] = a[i];
    else {
      RW(rs); RW(wr);
      for (i32 j = 0; j < n; j++) wr8(&wr, d + j, rd8(&rs, s - j));
    }
    s -= n; d += n; c -= n;
  }
  return 0;
}

/* The translucent loops blend what is on the screen: every pixel worked
 * out first, every page they will write made sure of as it is first met,
 * and only then written */
typedef struct { machine *m; u32 page; } later_t;
static void later_init(later_t *l, machine *m) { l->m = m; l->page = 0xffffffffu; hold_begin(&acc_hold); }
static inline void later_put(later_t *l, i32 a, i32 b) {
  if (((u32)a >> 12) != l->page) { acc_probe(l->m, a, 1, 1); l->page = (u32)a >> 12; }
  hold_put(&acc_hold, a, b);
}
static void later_commit(later_t *l) { hold_write8(l->m, &acc_hold); }
/* a blend's index, the new colour over the old, swapped when transrev */
static inline i32 blend(i32 c, i32 old, i32 rev) {
  i32 v = (c | (old << 8)) & 0xffff;
  if (rev) v = ((v >> 8) | (v << 8)) & 0xffff;
  return v;
}

/* DA_TVLINE1 (inc, pal, n, v, tex, dest, sh, bpl, transrev, transluc) */
static i32 tvline1(machine *m) {
  i32 a[10]; acc_args(m, 10, a);
  i32 inc = a[0], pal = a[1], n = a[2], v = a[3], tex = a[4], dest = a[5], sh = a[6] & 31, bpl = a[7], rev = a[8], tl = a[9];
  RW(rt); RW(rp); RW(rd); RW(rl);
  later_t out; later_init(&out, m);
  for (; n != 0; n--, v += inc, dest += bpl) {
    i32 t = rd8(&rt, tex + (i32)((u32)v >> sh));
    if (t != 255) later_put(&out, dest, rd8(&rl, tl + blend(rd8(&rp, pal + t), rd8(&rd, dest), rev)));
  }
  later_commit(&out);
  return v;
}

/* DA_TVLINE2 (ebp, inca, bufa, bufb, i5, i6, incb, edi, sh, pala, palb,
 * bpl, transrev, transluc, pasm1, pasm2): tvlineasm2, two columns */
static i32 tvline2(machine *m) {
  i32 x[16]; acc_args(m, 16, x);
  i32 ebp = x[0], inca = x[1], bufa = x[2], bufb = x[3], i5 = x[4], i6 = x[5], incb = x[6], edi = x[7], sh = x[8] & 31,
      pala = x[9], palb = x[10], bpl = x[11], rev = x[12], tl = x[13], pasm1 = x[14], pasm2 = x[15];
  RW(ra); RW(rb); RW(rp); RW(rd); RW(rl); RW(w32);
  later_t out; later_init(&out, m);
  i32 both = 0;
  i6 = i6 - edi;
  do {
    i32 a = rd8(&ra, bufa + (i32)((u32)i5 >> sh)), b = rd8(&rb, bufb + (i32)((u32)ebp >> sh));
    i5 += inca;
    ebp += incb;
    i32 p0 = i6 + edi, p1 = p0 + 1;
    if (a == 255) {
      if (b != 255) later_put(&out, p1, rd8(&rl, tl + blend(rd8(&rp, palb + b), rd8(&rd, p1), rev)));
    } else if (b == 255) {
      later_put(&out, p0, rd8(&rl, tl + blend(rd8(&rp, pala + a), rd8(&rd, p0), rev)));
    } else {
      i32 l = blend(rd8(&rp, pala + a), rd8(&rd, p0), rev);
      i32 r = blend(rd8(&rp, palb + b), rd8(&rd, p1), rev);
      later_put(&out, p0, rd8(&rl, tl + l));
      later_put(&out, p1, rd8(&rl, tl + r));
      both++;
    }
    i6 += bpl;
  } while ((u32)i6 > (u32)(i6 - bpl));
  acc_probe(m, pasm1, 4, 1);
  acc_probe(m, pasm2, 4, 1);
  later_commit(&out);
  wr32(&w32, pasm1, i5);
  wr32(&w32, pasm2, ebp);
  return both;
}

/* DA_TSPRITE (i2, n, i4, tex, dest, pal, eax1, eax3, ecx, padder, bpl,
 * transrev, transluc): DrawSpriteVerticalLine, translucent */
static i32 tsprite(machine *m) {
  i32 x[13]; acc_args(m, 13, x);
  i32 i2 = x[0], n = x[1], i4 = x[2], tex = x[3], dest = x[4], pal = x[5], eax1 = x[6], eax3 = x[7], ecx = x[8],
      padder = x[9], bpl = x[10], rev = x[11], tl = x[12];
  RW(r32); RW(w32); RW(rt); RW(rp); RW(rd); RW(rl);
  later_t out; later_init(&out, m);
  i32 adder = rd32(&r32, padder);
  while (n != 0) {
    n--;
    if (n != 0) {
      i4 += ecx;
      if ((u32)i4 < (u32)(i4 - ecx)) adder = eax3;
      i32 c = rd8(&rt, tex);
      i2 += eax1;
      if ((u32)i2 < (u32)(i2 - eax1)) tex++;
      tex += adder;
      if (c != 255) later_put(&out, dest, rd8(&rl, tl + blend(rd8(&rp, pal + c), rd8(&rd, dest), rev)));
      dest += bpl;
    }
  }
  acc_probe(m, padder, 4, 1);
  later_commit(&out);
  wr32(&w32, padder, adder);
  return 0;
}

/* DA_THLINE (i2, i3, i5, i6, tex, pal, inc2, inc5, al, bl, transrev,
 * transluc): thlineskipmodify, a translucent floor sprite's row */
static i32 thline(machine *m) {
  i32 x[12]; acc_args(m, 12, x);
  i32 i2 = x[0], i3 = x[1], i5 = x[2], i6 = x[3], tex = x[4], pal = x[5], inc2 = x[6], inc5 = x[7], al = x[8],
      bl = x[9], rev = x[10], tl = x[11];
  RW(rt); RW(rp); RW(rd); RW(rl);
  later_t out; later_init(&out, m);
  for (i32 counter = (i32)((u32)i3 >> 16); counter >= 0; counter--, i2 += inc2, i5 += inc5, i6++) {
    u32 ebx = LSR(i5, 32 - bl) | (u32)SHL(LSR(i2, al), bl);
    i32 t = rd8(&rt, tex + (i32)ebx);
    if (t != 255) later_put(&out, i6, rd8(&rl, tl + blend(rd8(&rp, pal + t), rd8(&rd, i6), rev)));
  }
  later_commit(&out);
  return 0;
}

/* DA_SCALE (a, b, c), c not 0: (int32_t)((int64_t)a * b / c), toward zero,
 * its low word, for any a, b and c: the product's magnitude (below 2^62)
 * over |c|, the sign put back */
static i32 scale(machine *m) {
  i32 v[3]; acc_args(m, 3, v);
  i32 a = v[0], b = v[1], c = v[2];
  if (!c) return 0;
  u64 ua = a < 0 ? (u64)(-(i64)a) : (u64)a, ub = b < 0 ? (u64)(-(i64)b) : (u64)b, uc = c < 0 ? (u64)(-(i64)c) : (u64)c;
  u32 q = (u32)((ua * ub) / uc);
  return ((a < 0) ^ (b < 0) ^ (c < 0)) ? (i32)(0u - q) : (i32)q;
}

/* DA_DMULSCALE (a, b, c, d, s): (int32_t)(((int64_t)a * b + (int64_t)c * d) >> s) */
static i32 dmulscale(machine *m) {
  i32 v[5]; acc_args(m, 5, v);
  i32 h1, h2; u32 l1, l2;
  mul64(v[0], v[1], &h1, &l1); mul64(v[2], v[3], &h2, &l2);
  u32 lo = l1 + l2;
  i32 hi = h1 + h2 + (lo < l1 ? 1 : 0);
  return (i32)(LSR(lo, v[4]) | (u32)SHL(hi, 32 - v[4]));
}

/* DA_MHLINE (n, a, b, dest, sh1, bits, tex, pal, da, db): mhlineskipmodify */
static i32 mhline(machine *m) {
  i32 x[10]; acc_args(m, 10, x);
  i32 n = x[0], a = x[1], b = x[2], dest = x[3], sh1 = x[4] & 31, bits = x[5] & 31, tex = x[6], pal = x[7], da = x[8],
      db = x[9];
  i32 sh2 = (32 - x[5]) & 31;
  RW(rt); RW(rp); RW(wr);
  for (; n > 0; n--, dest++, a += da, b += db) {
    i32 c = rd8(&rt, tex + (SHL(LSR(a, sh1), bits) | (i32)LSR(b, sh2)));
    if (c != 255) wr8(&wr, dest, rd8(&rp, pal + c));
  }
  return 0;
}

const acc_fn acc_duke3d[] = {tcol0, tcol1, hline, rhline, slope, mulscale, udiv64, qinterp16, clearb, copyb,
                             tvline1, tvline2, tsprite, thline, scale, dmulscale, copyr, mhline};
const int acc_duke3d_n = sizeof acc_duke3d / sizeof acc_duke3d[0];
