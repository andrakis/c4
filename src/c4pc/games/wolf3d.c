/* wolf3d.c -- Wolfenstein 3D's accelerators (games/wolf3d/c4/accel.mjs)
 * in C: the walls' and sprites' scalers, the screen's scaled blit, the
 * timedemo's checksum and FixedMul. c4/c4accel.h is the game's side (WA_*). */
#include "../accel.h"

/* WA_POST (wh, vbuf, pitch, postx, centery, vh, src, tsize): ScalePost */
static i32 post(machine *m) {
  i32 a[8]; acc_args(m, 8, a);
  i32 wh = a[0], vbuf = a[1], pitch = a[2], postx = a[3], centery = a[4], vh = a[5], src = a[6], tsize = a[7];
  i32 half = tsize >> 1;
  i32 ywcount = wh >> 3, yd = ywcount;
  if (yd <= 0) yd = 100;
  i32 yoffs = IMUL(centery - ywcount, pitch);
  if (yoffs < 0) yoffs = 0;
  yoffs = yoffs + postx;
  i32 yendoffs = centery + ywcount - 1, yw = tsize - 1;
  while (yendoffs >= vh) {
    ywcount = ywcount - half;
    while (ywcount <= 0) { ywcount = ywcount + yd; yw--; }
    yendoffs--;
  }
  if (yw < 0) return 0;
  RW(rs); RW(wr);
  i32 c = rd8(&rs, src + yw);
  yendoffs = IMUL(yendoffs, pitch) + postx;
  i32 dest = vbuf + yendoffs;
  const i32 top = vbuf + yoffs;
  while (dest >= top) {
    wr8(&wr, dest, c);
    ywcount = ywcount - half;
    if (ywcount <= 0) {
      do { ywcount = ywcount + yd; yw--; } while (ywcount <= 0);
      if (yw < 0) break;
      c = rd8(&rs, src + yw);
    }
    dest = dest - pitch;
  }
  return 0;
}

/* WA_LINE (x, toppix, fracstep, linesrc, linecmds, vbuf, ylookup, pitch,
 * vh): ScaleLine, a sprite's column, every segment of its list */
static i32 line(machine *m) {
  i32 a[9]; acc_args(m, 9, a);
  i32 x = a[0], toppix = a[1], fracstep = a[2], linesrc = a[3], linecmds = a[4], vbuf = a[5], ylookup = a[6],
      pitch = a[7], vh = a[8];
  RW(rc); RW(rs); RW(ry); RW(wr);
#define SHORT(p) (rd8(&rc, (p)) | rd8(&rc, (p) + 1) << 8)
  for (i32 end = SHORT(linecmds) >> 1; end; end = SHORT(linecmds) >> 1) {
    i32 top = (i32)((u32)SHORT(linecmds + 2) << 16) >> 16;
    i32 start = SHORT(linecmds + 4) >> 1;
    i32 frac = IMUL(start, fracstep);
    i32 endpix = (frac >> 16) + toppix;
    for (i32 src = linesrc + top + start; start != end; start++, src++) {
      i32 startpix = endpix;
      if (startpix >= vh) break;
      frac = frac + fracstep;
      endpix = (frac >> 16) + toppix;
      if (endpix < 0) continue;
      if (startpix < 0) startpix = 0;
      if (endpix > vh) endpix = vh;
      if (startpix >= endpix) continue;
      i32 c = rd8(&rs, src);
      i32 dest = vbuf + rd32(&ry, ylookup + 4 * startpix) + x;
      for (; startpix < endpix; startpix++, dest += pitch) wr8(&wr, dest, c);
    }
    linecmds = linecmds + 6;
  }
#undef SHORT
  return 0;
}

/* WA_SUM (sum, pixels, w, h, pitch, pal): c4sdl_present's checksum */
static i32 sum(machine *m) {
  i32 a[6]; acc_args(m, 6, a);
  i32 s = a[0], pix = a[1], w = a[2], h = a[3], pitch = a[4], pal = a[5];
  RW(r32); RW(rb);
  i32 n = w >> 2;
  for (i32 y = 0; y < h; y++) {
    i32 row = pix + IMUL(y, pitch);
    for (i32 x = 0; x < n; x++) s = IMUL(s, 31) + rd32(&r32, row + 4 * x);
  }
  for (i32 x = 0; x < 256; x++) {
    i32 c = pal + 4 * x;
    s = IMUL(s, 31) + rd8(&rb, c) * 65536 + rd8(&rb, c + 1) * 256 + rd8(&rb, c + 2);
  }
  return s;
}

/* WA_FIXMUL (a, b): ((int64_t)a * b + 0x8000) >> 16, its 32-bit pieces */
static i32 fixmul(machine *m) {
  i32 x[2]; acc_args(m, 2, x);
  i32 ah = x[0] >> 16, al = x[0] & 0xffff, bh = x[1] >> 16, bl = x[1] & 0xffff;
  return (i32)(((u32)IMUL(ah, bh) << 16) + (u32)IMUL(ah, bl) + (u32)IMUL(al, bh) + (((u32)al * (u32)bl + 0x8000u) >> 16));
}

/* WA_BLIT (source, origwidth, srcx, srcy, width, height, dest, ylookup,
 * destx, desty, scale): VL_MemToScreenScaledCoord, each pixel a square */
static i32 blit(machine *m) {
  i32 a[11]; acc_args(m, 11, a);
  i32 source = a[0], ow = a[1], srcx = a[2], srcy = a[3], width = a[4], height = a[5], dest = a[6], ylookup = a[7],
      destx = a[8], desty = a[9], sc = a[10];
  RW(rs); RW(ry); RW(wr);
  for (i32 j = 0, scj = 0; j < height; j++, scj += sc) {
    i32 srow = source + IMUL(j + srcy, ow) + srcx;
    for (i32 i = 0, sci = 0; i < width; i++, sci += sc) {
      i32 c = rd8(&rs, srow + i);
      for (i32 mm = 0; mm < sc; mm++) {
        i32 base = dest + rd32(&ry, ylookup + 4 * (scj + mm + desty)) + sci + destx;
        for (i32 nn = 0; nn < sc; nn++) wr8(&wr, base + nn, c);
      }
    }
  }
  return 0;
}

const acc_fn acc_wolf3d[] = {post, line, sum, fixmul, blit};
const int acc_wolf3d_n = sizeof acc_wolf3d / sizeof acc_wolf3d[0];
