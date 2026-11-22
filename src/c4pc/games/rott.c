/* rott.c -- Rise of the Triad's accelerators (games/rott/c4/accel.mjs) in
 * C: the renderer's innermost loops and the 64-bit fixed-point helpers.
 * c4/c4accel.h is the game's side (RA_*). */
#include "../accel.h"

/* RA_COL (n, frac, step, sh, sgn, src, shade, dest, stride): a column */
static i32 col(machine *m) {
  i32 a[9]; acc_args(m, 9, a);
  i32 n = a[0], frac = a[1], step = a[2], sh = a[3] & 31, sgn = a[4], src = a[5], shade = a[6], dest = a[7], stride = a[8];
  RW(rs); RW(rp); RW(wr);
  for (; n > 0; n--, dest += stride, frac += step)
    wr8(&wr, dest, rd8(&rp, shade + rd8(&rs, src + (sgn ? frac >> sh : (i32)((u32)frac >> sh)))));
  return 0;
}

/* RA_TRANS (n, shade, dest, stride): R_TransColumn, *dest = shade[*dest],
 * every pixel worked out first and its pages made sure of */
static i32 trans(machine *m) {
  i32 a[4]; acc_args(m, 4, a);
  i32 n = a[0], shade = a[1], dest = a[2], stride = a[3];
  if (n <= 0) return 0;
  RW(rd); RW(rp);
  hold_t *h = &acc_hold; hold_begin(h);
  i32 d = dest;
  for (i32 i = 0; i < n; i++, d += stride) hold_put(h, d, rd8(&rp, shade + rd8(&rd, d)));
  hold_probe(m, h, 1);
  hold_write8(m, h);
  return 0;
}

/* RA_ROW (n, frac, step, src, shade, dest): DrawRow, a 128x128 tile */
static i32 row(machine *m) {
  i32 a[6]; acc_args(m, 6, a);
  i32 n = a[0], frac = a[1], step = a[2], src = a[3], shade = a[4], dest = a[5];
  RW(rs); RW(rp); RW(wr);
  for (; n > 0; n--, dest++, frac += step)
    wr8(&wr, dest, rd8(&rp, shade + rd8(&rs, src + (i32)((((u32)frac >> 25) | (((u32)frac >> 9) << 7)) & 16383))));
  return 0;
}

/* RA_ROTROW (n, xfrac, yfrac, xstep, ystep, src, dest, masked, xlim, ylim, sh) */
static i32 rotrow(machine *m) {
  i32 a[11]; acc_args(m, 11, a);
  i32 n = a[0], xf = a[1], yf = a[2], xs = a[3], ys = a[4], src = a[5], dest = a[6], masked = a[7], xlim = a[8],
      ylim = a[9], sh = a[10] & 31;
  i32 yl = (16 - sh) & 31, yr = (32 - sh) & 31;
  RW(rs); RW(wr);
  for (; n > 0; n--, dest++, xf += xs, yf += ys) {
    i32 x = (i32)((u32)xf >> 16);
    i32 e = x < xlim && (i32)((u32)yf >> 16) < ylim ? (i32)(((u32)x << sh) | (((u32)yf << yl) >> yr)) : 0;
    i32 c = rd8(&rs, src + e);
    if (!masked || c != 255) wr8(&wr, dest, c);
  }
  return 0;
}

/* RA_SKY (n, src, shade, dest, stride, orgh): DrawSkyPost */
static i32 sky(machine *m) {
  i32 a[6]; acc_args(m, 6, a);
  i32 n = a[0], src = a[1], shade = a[2], dest = a[3], stride = a[4], orgh = a[5];
  RW(rs); RW(rp); RW(wr);
  while (n-- > 0) {
    if (orgh > 0 && n < orgh - 400) src -= 2;
    wr8(&wr, dest, rd8(&rp, shade + rd8(&rs, src)));
    dest += stride;
    src++;
  }
  return 0;
}

/* RA_TILE (start, width, height, offx, offy, source, sw, sh, stride) */
static i32 tile(machine *m) {
  i32 a[9]; acc_args(m, 9, a);
  i32 start = a[0], width = a[1], height = a[2], offx = a[3], offy = a[4], source = a[5], sw = a[6], sh = a[7],
      stride = a[8];
  RW(rs); RW(wr);
  i32 startoffset = IMUL(offy, sw), planesize = IMUL(sw, sh);
  for (i32 plane = 4; plane > 0; plane--) {
    i32 origdest = start + (4 - plane);
    i32 sy = offy, soff = source + startoffset;
    for (i32 h = height; h > 0; h--) {
      i32 dest = origdest, sx = offx;
      for (i32 w = width; w > 0; w--) {
        wr8(&wr, dest, rd8(&rs, soff + sx));
        dest += 4;
        if (++sx >= sw) sx = 0;
      }
      origdest += stride;
      soff += sw;
      if (++sy >= sh) { sy = 0; soff = source; }
    }
    source += planesize;
  }
  return 0;
}

/* RA_POST (src, buf, top0, inv, vh, tmid, iscale, center, shade, see, ylk,
 * stride, mode, pyl, pyh, psrc): a sprite's post, every run of it */
static i32 post(machine *m) {
  i32 a[16]; acc_args(m, 16, a);
  i32 src = a[0], buf = a[1], top0 = a[2], inv = a[3], vh = a[4], tmid = a[5], iscale = a[6], center = a[7],
      shade = a[8], see = a[9], ylk = a[10], stride = a[11], mode = a[12], pyl = a[13], pyh = a[14], psrc = a[15];
  i32 round = mode & 1 ? 65535 : 65536, clip = mode & 2, transp = mode & 4;
  RW(rs); RW(rx); RW(rp); RW(ry); RW(wr);
  hold_t *h = &acc_hold;
  if (transp) hold_begin(h);
#define PUT(x, v) do { if (transp) hold_put(h, (x), (v)); else wr8(&wr, (x), (v)); } while (0)
  i32 yl = 0, yh = 0, set = 0, dsrc = 0, dset = 0;
  i32 off = rd8(&rs, src); src++;
  while (off != 255) {
    i32 len = rd8(&rs, src); src++;
    i32 top = top0 + IMUL(inv, off);
    i32 bot = top + IMUL(inv, len);
    yl = (top + round) >> 16;
    yh = (bot - 1) >> 16;
    if (yh >= vh) yh = vh - 1;
    if (yl < 0) yl = 0;
    set = 1;
    if (transp && rd8(&rs, src) == 254) {
      if (yl <= yh) {
        i32 d = buf + rd32(&ry, ylk + 4 * yl), c;
        for (i32 n = yh - yl + 1; n > 0; n--, d += stride) {
          if (!hold_get(h, d, &c)) c = rd8(&rx, d);
          PUT(d, rd8(&rp, see + c));
        }
      }
      src++;
      off = rd8(&rs, src); src++;
    } else {
      if (yl <= yh) {
        dsrc = src - off; dset = 1;
        i32 d = buf + rd32(&ry, ylk + 4 * yl);
        i32 frac = tmid + IMUL(yl - center, iscale);
        for (i32 n = yh - yl + 1; n > 0; n--, d += stride, frac += iscale)
          PUT(d, rd8(&rp, shade + rd8(&rs, dsrc + (clip ? (i32)((u32)frac >> 16) : frac >> 16))));
      }
      src += len;
      off = rd8(&rs, src); src++;
    }
  }
#undef PUT
  if (transp) {
    hold_probe(m, h, 1);
    if (set) { acc_probe(m, pyl, 4, 1); acc_probe(m, pyh, 4, 1); }
    if (dset) acc_probe(m, psrc, 4, 1);
    hold_write8(m, h);
  }
  RW(w32);
  if (set) { wr32(&w32, pyl, yl); wr32(&w32, pyh, yh); }
  if (dset) wr32(&w32, psrc, dsrc);
  return 0;
}

/* (int64_t)a * b as its two words, hi signed and lo unsigned: the 16-bit
 * digits, the high ones signed, as watcom.c's C4_MUL64 (and accel.mjs,
 * whose m1 >> 16 and m1 << 16 take m1's low 32 bits) */
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

/* RA_FIXMUL (a, b): (int32_t)(((int64_t)a * b + 0x8000) >> 16) */
static i32 fixmul(machine *m) {
  i32 a[2]; acc_args(m, 2, a);
  i32 hi; u32 lo; mul64(a[0], a[1], &hi, &lo);
  u32 t = lo + 0x8000u;
  if (t < lo) hi++;
  return (i32)((t >> 16) | ((u32)hi << 16));
}

/* RA_FIXMULSHIFT (a, b, s): (int32_t)((uint64_t)((int64_t)a * b) >> s) */
static i32 fixmulshift(machine *m) {
  i32 a[3]; acc_args(m, 3, a);
  i32 hi; u32 lo; mul64(a[0], a[1], &hi, &lo);
  return (i32)(LSR(lo, a[2]) | (u32)SHL(hi, 32 - a[2]));
}

/* RA_FIXDIV2 (a, b): (int32_t)((int64_t)a * 65536 / b); RA_FIXSCALE (a,
 * b, c): (int32_t)((int64_t)a * b / c), toward zero */
static i32 fixdiv2(machine *m) {
  i32 a[2]; acc_args(m, 2, a);
  if (!a[1]) return 0;
  return (i32)(u32)(((i64)a[0] * 65536) / a[1]);
}
static i32 fixscale(machine *m) {
  i32 a[3]; acc_args(m, 3, a);
  if (!a[2]) return 0;
  return (i32)(u32)(((i64)a[0] * a[1]) / a[2]);
}

const acc_fn acc_rott[] = {col, trans, row, rotrow, sky, fixmul, fixmulshift, fixdiv2, fixscale, tile, post};
const int acc_rott_n = sizeof acc_rott / sizeof acc_rott[0];
