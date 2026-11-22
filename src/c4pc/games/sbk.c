/* sbk.c -- Super Blit Kart's accelerators (games/sbk/c4/accel.mjs) in C:
 * the track's mode 7 spans, the blits and their blends, and -demo's frame
 * hash. c4/c4accel.h is the game's side (SA_*). */
#include "../accel.h"

/* C's int division as accel.mjs's (a / b) | 0: toward zero, the quotient's
 * low word, 0 for b 0 (b may be -INT_MIN, so a wide one) */
static i32 jdiv(i32 a, i64 b) { if (!b) return 0; return (i32)(u32)((i64)a / b); }

/* writes of one operation: straight to the screen, or, where it reads the
 * screen, held one a word (a Map) until every page they go to is made
 * sure of */
typedef struct { machine *m; int held; rw_t r32, w32; } sink_t;
static void sink_init(sink_t *o, machine *m, int held) {
  o->m = m; o->held = held; rw_init(&o->r32, m); rw_init(&o->w32, m);
  if (held) hold_begin(&acc_hold);
}
static inline i32 get(sink_t *o, i32 a) {
  i32 v;
  if (o->held && hold_get(&acc_hold, a, &v)) return v;
  return rd32(&o->r32, a);
}
static inline void put(sink_t *o, i32 a, i32 v) { if (o->held) hold_set(&acc_hold, a, v); else wr32(&o->w32, a, v); }
static void done(sink_t *o) {
  if (!o->held) return;
  hold_probe(o->m, &acc_hold, 4);
  hold_write32(o->m, &acc_hold);
}
/* RGBA_RGB's blend of p over v at alpha a, as blit.c has it */
static inline i32 blend(i32 p, i32 v, i32 a) {
  i32 dr = (v >> 16) & 255, dg = (v >> 8) & 255, db = v & 255;
  return ((dr + ((a * (((p >> 16) & 255) - dr) + 127) >> 8)) << 16) |
         ((dg + ((a * (((p >> 8) & 255) - dg) + 127) >> 8)) << 8) |
         (db + ((a * ((p & 255) - db) + 127) >> 8));
}
/* pen_span's work from its blend on */
static void penspan(sink_t *out, i32 pen, i32 d, i32 c, i32 a) {
  if (a >= 255) {
    i32 v = pen & 0xFFFFFF;
    for (; c > 0; c--, d += 4) put(out, d, v);
    return;
  }
  for (; c > 0; c--, d += 4) put(out, d, blend(pen, get(out, d), a));
}

/* SA_HASH (p, n): FNV-1a over the frame's n words */
static i32 hash(machine *m) {
  i32 a[2]; acc_args(m, 2, a);
  RW(r32);
  i32 h = (i32)2166136261u;
  for (i32 i = 0; i < a[1]; i++) h = IMUL(h ^ rd32(&r32, a[0] + 4 * i), 16777619);
  return h;
}

/* SA_PIXELS (world, wshift, row, d, c, wx, wy, dx, dy): span_pixels */
static i32 pixels(machine *m) {
  i32 a[9]; acc_args(m, 9, a);
  i32 world = a[0], wshift = a[1], row = a[2], d = a[3], c = a[4], wx = a[5], wy = a[6], dx = a[7], dy = a[8];
  RW(rw); RW(rr); RW(w32);
  for (; c > 0; c--, d += 4) {
    wr32(&w32, d, rd32(&rr, row + 4 * rd8(&rw, world + (wx >> 16) + SHL(wy >> 16, wshift))));
    wx += dx; wy += dy;
  }
  return 0;
}

/* TileMap::offset's clamp at the map's edge, then the texel */
static i32 clamp(rw_t *rw, i32 world, i32 wshift, i32 w, i32 x, i32 y) {
  i32 cx = x >> 3, cy = y >> 3;
  if (cx != (cx & (w - 1))) cx = cx < 0 ? 0 : w - 1;
  if (cy != (cy & (w - 1))) cy = cy < 0 ? 0 : w - 1;
  return rd8(rw, world + SHL(cx, 3) + (x & 7) + SHL(SHL(cy, 3) + (y & 7), wshift));
}

/* SA_RUNS (world, wshift, w, row, d, c, wx, wy, dx, dy): span_runs */
static i32 runs(machine *m) {
  i32 a[10]; acc_args(m, 10, a);
  i32 world = a[0], wshift = a[1], w = a[2], row = a[3], d = a[4], c = a[5], wx = a[6], wy = a[7], dx = a[8], dy = a[9];
  RW(rw); RW(rr); RW(w32);
  i32 notmask = ~(SHL(w, 3) - 1);
  i32 x = wx >> 16, y = wy >> 16, kx, ky, n, i = 0, v, colr;
  if (dx > 0) kx = jdiv(SHL(x + 1, 16) - wx + dx - 1, dx);
  else if (dx < 0) kx = jdiv(wx - SHL(x, 16), -(i64)dx) + 1;
  else kx = 0x7FFFFFFF;
  if (dy > 0) ky = jdiv(SHL(y + 1, 16) - wy + dy - 1, dy);
  else if (dy < 0) ky = jdiv(wy - SHL(y, 16), -(i64)dy) + 1;
  else ky = 0x7FFFFFFF;
  for (;;) {
    if ((x | y) & notmask) v = clamp(&rw, world, wshift, w, x, y);
    else v = rd8(&rw, world + x + SHL(y, wshift));
    n = kx < ky ? kx : ky;
    if (n > c) n = c;
    c -= n; i += n; kx -= n; ky -= n;
    colr = rd32(&rr, row + 4 * v);
    if (v != 255) for (; n > 0; n--, d += 4) wr32(&w32, d, colr);
    else d += 4 * n;
    if (!c) return 0;
    if (!kx) {
      n = wx + IMUL(i, dx);
      x = n >> 16;
      if (dx > 0) kx = jdiv(SHL(x + 1, 16) - n + dx - 1, dx);
      else kx = jdiv(n - SHL(x, 16), -(i64)dx) + 1;
    }
    if (!ky) {
      n = wy + IMUL(i, dy);
      y = n >> 16;
      if (dy > 0) ky = jdiv(SHL(y + 1, 16) - n + dy - 1, dy);
      else ky = jdiv(n - SHL(y, 16), -(i64)dy) + 1;
    }
  }
}

/* SA_EDGE (world, wshift, w, empty, row, d, c, wx, wy, dx, dy): span_edge */
static i32 edge(machine *m) {
  i32 a[11]; acc_args(m, 11, a);
  i32 world = a[0], wshift = a[1], w = a[2], empty = a[3], row = a[4], d = a[5], c = a[6], wx = a[7], wy = a[8],
      dx = a[9], dy = a[10];
  RW(rw); RW(rr); RW(w32);
  i32 notmask = ~(SHL(w, 3) - 1);
  do {
    i32 x = wx >> 16, y = wy >> 16;
    if ((x | y) & notmask) {
      if (!empty) wr32(&w32, d, rd32(&rr, row + 4 * clamp(&rw, world, wshift, w, x, y)));
    } else wr32(&w32, d, rd32(&rr, row + 4 * rd8(&rw, world + x + SHL(y, wshift))));
    d += 4; wx += dx; wy += dy;
  } while (--c);
  return 0;
}

/* SA_PEN (pen, d, c, a): pen_span from its blend on */
static i32 pen(machine *m) {
  i32 a[4]; acc_args(m, 4, a);
  sink_t out; sink_init(&out, m, a[3] < 255);
  penspan(&out, a[0], a[1], a[2], a[3]);
  done(&out);
  return 0;
}

/* SA_ROW (s, pal, d, cnt, alpha1): row_blit */
static i32 row(machine *m) {
  i32 x[5]; acc_args(m, 5, x);
  i32 s = x[0], pal = x[1], d = x[2], cnt = x[3], alpha1 = x[4];
  RW(rs); RW(rp);
  sink_t out; sink_init(&out, m, 1);
  do {
    i32 p = rd32(&rp, pal + 4 * rd8(&rs, s));
    s++;
    i32 a = IMUL(((p >> 24) & 255) + 1, alpha1) >> 8;
    if (a >= 255) put(&out, d, p & 0xFFFFFF);
    else if (a > 1) put(&out, d, blend(p, get(&out, d), a));
    d += 4;
  } while (--cnt > 0);
  done(&out);
  return 0;
}

/* SA_TINT (src, w, h, d0, stride, rgb, a): scr_blit_tinted, the minimap */
static i32 tint(machine *m) {
  i32 v[7]; acc_args(m, 7, v);
  i32 src = v[0], w = v[1], h = v[2], d0 = v[3], stride = v[4], rgb = v[5], a = v[6];
  RW(rs); RW(rp);
  sink_t out; sink_init(&out, m, 1);
  for (i32 y = 0; y < h; y++) {
    i32 d = d0 + IMUL(y, stride), x = w;
    do {
      i32 c = rd8(&rs, src); src++;
      if (c) put(&out, d, blend(rd32(&rp, rgb + 4 * c), get(&out, d), a));
      d += 4;
    } while (--x > 0);
  }
  done(&out);
  return 0;
}

/* SA_ANY (tiles, trans, pal, sheet, w, d, c, wx, wy, dx, dy, alpha): span_any */
static i32 any(machine *m) {
  i32 x[12]; acc_args(m, 12, x);
  i32 tiles = x[0], trans = x[1], pal = x[2], sheet = x[3], w = x[4], d = x[5], c = x[6], wx = x[7], wy = x[8],
      dx = x[9], dy = x[10], alpha = x[11];
  RW(rt); RW(rx); RW(rh); RW(rp);
  sink_t out; sink_init(&out, m, 1);
  do {
    i32 wcx = wx >> 16, wcy = wy >> 16;
    i32 cx = (wcx >> 3) & (w - 1), cy = (wcy >> 3) & (w - 1);
    if ((wcx >> 3) != cx) cx = (wcx >> 3) < 0 ? 0 : w - 1;
    if ((wcy >> 3) != cy) cy = (wcy >> 3) < 0 ? 0 : w - 1;
    i32 toff = cx + IMUL(cy, w);
    i32 tile = rd8(&rt, tiles + toff);
    if (tile != 0) {
      i32 tr = trans ? rd8(&rx, trans + toff) : 0;
      i32 u = wcx & 7, v = wcy & 7;
      if (tr) {
        v = (tr & 2) ? 7 - v : v;
        u = (tr & 4) ? 7 - u : u;
        if (tr & 1) { i32 t = u; u = v; v = t; }
      }
      u += (tile & 15) * 8;
      v += (tile >> 4) * 8;
      i32 count = 0;
      do {
        wx += dx; wy += dy; c--; count++;
      } while (c && (wx >> 16) == wcx && (wy >> 16) == wcy);
      i32 p = rd32(&rp, pal + 4 * rd8(&rh, sheet + u + v * 128));
      i32 a = (p >> 24) & 255;
      if (a) penspan(&out, p, d, count, IMUL(a + 1, alpha + 1) >> 8);
      d += 4 * count;
      continue;
    }
    do {
      wx += dx; wy += dy; d += 4; c--;
    } while (c && (wx >> 19) == (wcx >> 3) && (wy >> 19) == (wcy >> 3));
  } while (c);
  done(&out);
  return 0;
}

/* SA_STRETCH1 (row0, sy, sw2, pal, y, ystep, left, xstep, d, dw, dh, alpha1) */
static i32 stretch1(machine *m) {
  i32 v[12]; acc_args(m, 12, v);
  i32 row0 = v[0], sy = v[1], sw2 = v[2], pal = v[3], y = v[4], ystep = v[5], left = v[6], xstep = v[7], d = v[8],
      dw = v[9], dh = v[10], alpha1 = v[11];
  RW(rs); RW(rp);
  sink_t out; sink_init(&out, m, 1);
  do {
    i32 r = row0 + IMUL(sy + (y >> 16), sw2);
    i32 x = left, dd = d, n = dw;
    do {
      i32 p = rd32(&rp, pal + 4 * rd8(&rs, r + (x >> 16)));
      x += xstep;
      if (p >> 24) {
        i32 a = IMUL(((p >> 24) & 255) + 1, alpha1) >> 8;
        put(&out, dd, a >= 255 ? p & 0xFFFFFF : blend(p, get(&out, dd), a));
      }
      dd += 4;
    } while (--n > 0);
    d += 4 * 320;
    y += ystep;
  } while (--dh > 0);
  done(&out);
  return 0;
}

/* SA_STRETCHW (row0, sy, sw2, pal, y, ystep, left, xstep, xpx, d, dw, dh,
 * alpha1): scr_stretch wider than the source, worked out into runs */
static i32 stretchw(machine *m) {
  i32 v[13]; acc_args(m, 13, v);
  i32 row0 = v[0], sy = v[1], sw2 = v[2], pal = v[3], y = v[4], ystep = v[5], left = v[6], xstep = v[7], xpx = v[8],
      d = v[9], dw = v[10], dh = v[11], alpha1 = v[12];
  RW(rs); RW(rp);
  sink_t out; sink_init(&out, m, 1);
  u32 nruns = 0, room = 0;
  i32 *runX = 0, *runN = 0;
  i32 xc = dw, x = left, num = (x & 65535) == 0 ? xpx : 1;
  do {
    i32 nx = x + IMUL(xstep, num);
    if ((x >> 16) == (nx >> 16) && xc > num) { num++; nx += xstep; }
    if (nruns == room) {
      if (room >= (1u << 20)) break;                   /* a span that never ends, cut short */
      room = room ? room * 2 : 64;
      runX = acc_scratch(0, room); runN = acc_scratch(1, room);   /* grown, kept */
    }
    runX[nruns] = x >> 16; runN[nruns] = num; nruns++;
    x = nx;
    xc -= num;
    num = xc < xpx ? xc : xpx;
  } while (xc);
  /* the visible runs of the row last worked out: colour, opaque, offset, count */
  i32 *vis = acc_scratch(3, 4 * nruns + 4);
  i32 last = -1; u32 nvis = 0;
  do {
    i32 srcY = sy + (y >> 16);
    if (srcY != last) {
      last = srcY;
      i32 r = row0 + IMUL(srcY, sw2), off = 0;
      nvis = 0;
      for (u32 j = 0; j < nruns; j++) {
        i32 p = rd32(&rp, pal + 4 * rd8(&rs, r + runX[j]));
        if (p >> 24) {
          i32 a = IMUL(((p >> 24) & 255) + 1, alpha1) >> 8;
          vis[4 * nvis] = a >= 255 ? p & 0xFFFFFF : p; vis[4 * nvis + 1] = a >= 255;
          vis[4 * nvis + 2] = off; vis[4 * nvis + 3] = runN[j]; nvis++;
        }
        off += runN[j];
      }
    }
    for (u32 k = 0; k < nvis; k++) {
      i32 p = vis[4 * k], at = d + 4 * vis[4 * k + 2], n = vis[4 * k + 3];
      if (vis[4 * k + 1]) penspan(&out, p, at, n, 255);
      else {
        i32 a = (p >> 24) & 255;
        if (a) penspan(&out, p, at, n, IMUL(a + 1, alpha1) >> 8);
      }
    }
    d += 4 * 320;
    y += ystep;
  } while (--dh > 0);
  done(&out);
  return 0;
}

/* SA_TEXT (s, data, widths, cw, ch, cx, cy, clipx, clipy, clipw, cliph,
 * plain, ink, pen, alpha, fb): scr_text */
static i32 text(machine *m) {
  i32 v[16]; acc_args(m, 16, v);
  i32 s = v[0], data = v[1], widths = v[2], cw = v[3], ch = v[4], cx = v[5], cy = v[6], clx = v[7], cly = v[8],
      clw = v[9], clh = v[10], plain = v[11], ink = v[12], pen_ = v[13], alpha = v[14], fb = v[15];
  RW(rc); RW(rg); RW(rw);
  sink_t out; sink_init(&out, m, !plain);
  i32 hb = (ch + 7) / 8, size = IMUL(cw, hb);
  i32 pa = (pen_ >> 24) & 255, a = IMUL(pa + 1, alpha + 1) >> 8;
  for (i32 c; (c = rd8(&rc, s)) != 0; ) {
    s++;
    c &= 0x7F;
    c = c < 32 ? 0 : c - 32;
    i32 g = data + IMUL(c, size);
    for (i32 x = 0; x < cw; x++) {
      i32 xx = cx + x;
      if (xx < clx || (i64)xx >= (i64)clx + clw) { g += hb; continue; }
      for (i32 y = 0; y < ch; y += 8) {
        i32 bit = rd8(&rg, g); g++;
        if (!bit) continue;
        for (int po = 0; po < 8; po++) {
          if (!(bit & (1 << po))) continue;
          i32 yy = cy + y + po;
          if (yy >= 0 && yy >= cly && (i64)yy < (i64)cly + clh) {
            i32 at = fb + 4 * (xx + IMUL(yy, 320));
            if (plain) put(&out, at, ink);
            else if (pa) penspan(&out, pen_, at, 1, a);
          }
        }
      }
    }
    cx += rd8(&rw, widths + c);
  }
  done(&out);
  return 0;
}

const acc_fn acc_sbk[] = {hash, pixels, runs, edge, pen, row, tint, any, stretch1, stretchw, text};
const int acc_sbk_n = sizeof acc_sbk / sizeof acc_sbk[0];
