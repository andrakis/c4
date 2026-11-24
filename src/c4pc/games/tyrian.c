/* tyrian.c -- OpenTyrian's accelerators (games/tyrian/c4/accel.mjs) in C:
 * the sprites' blits in each of their kinds, the background's rows of
 * tiles, the screen's hue and brightness filters and the timedemo's
 * checksum. c4/c4accel.h is the game's side (TA_*). */
#include "../accel.h"
#if WB == 4                            /* the 32-bit machine's games (c4pc.h) */

/* the kinds that read the screen: their writes are held (sink) */
static int reads(i32 kind) { return kind == 1 || kind == 4 || kind == 5 || kind == 11 || kind == 12; }
static i32 pixel(i32 kind, i32 d, i32 p, i32 a, i32 b) {
  switch (kind) {
  case 0: return d;                                                 /* blit_sprite */
  case 1: return (d & 0xf0) | (((p & 0x0f) + (d & 0x0f)) >> 1);     /* blit_sprite_blend */
  case 2: return (a | ((d & 0x0f) + b)) & 0xff;                     /* blit_sprite_hv_unsafe */
  case 3: case 4: {                                                 /* blit_sprite_hv, _hv_blend */
    i32 t = ((d & 0x0f) + b) & 0xff;
    if (t > 0xf) t = t >= 0x1f ? 0 : 0xf;
    return kind == 3 ? (a | t) & 0xff : (a | (((p & 0x0f) + t) >> 1)) & 0xff;
  }
  case 5: return b ? 0 : (p & 0xf0) | ((p & 0x0f) >> 1);           /* blit_sprite_dark */
  case 10: return d;                                                /* blit_sprite2 */
  case 11: return (((d & 0x0f) + (p & 0x0f)) >> 1) | (d & 0xf0);    /* blit_sprite2_blend */
  case 12: return ((p & 0x0f) >> 1) + (p & 0xf0);                   /* blit_sprite2_darken */
  case 13: return (a | (d & 0x0f)) & 0xff;                          /* blit_sprite2_filter */
  }
  return d;
}
/* the writes of one operation: straight to the screen, or held until
 * their pages are made sure of */
typedef struct { machine *m; int held; rw_t wr; } sink_t;
static void sink_init(sink_t *o, machine *m, int held) {
  o->m = m; o->held = held; rw_init(&o->wr, m);
  if (held) hold_begin(&acc_hold);
}
static inline void put(sink_t *o, i32 x, i32 v) { if (o->held) hold_put(&acc_holds[o->m->acc_k], x, v); else wr8(&o->wr, x, v); }
static void done(sink_t *o) {
  if (!o->held) return;
  hold_probe(o->m, &acc_holds[o->m->acc_k], 1);
  hold_write8(o->m, &acc_holds[o->m->acc_k]);
}

/* TA_SUM (h, pal, pixels, w, hgt, pitch): tyrian-sum.c's checksum */
static i32 sum(machine *m) {
  i32 x[6]; acc_args(m, 6, x);
  i32 h = x[0], pal = x[1], pix = x[2], w = x[3], hgt = x[4], pitch = x[5];
  RW(r32); RW(rb);
  for (i32 i = 0; i < 256; i++) h = IMUL(h, 31) + rd32(&r32, pal + 4 * i);
  for (i32 y = 0; y < hgt; y++) {
    i32 row = pix + IMUL(y, pitch);
    u8 *v = (row & 3) == 0 ? acc_span(m, row, (w + 3) & ~3, 0) : 0;
    for (i32 i = 0; i < w; i += 4) {
      i32 a = row + i, word;
      if (v) word = (i32)(v[i] | v[i + 1] << 8 | v[i + 2] << 16 | (u32)v[i + 3] << 24);
      else word = (i32)(rd8(&rb, a) | rd8(&rb, a + 1) << 8 | rd8(&rb, a + 2) << 16 | (u32)rd8(&rb, a + 3) << 24);
      h = IMUL(h, 31) + word;
    }
  }
  return h;
}

/* TA_SPRITE (data, size, width, pixels, ll, ul, pitch, kind, a, b) */
static i32 sprite(machine *m) {
  i32 x[10]; acc_args(m, 10, x);
  i32 data = x[0], size = x[1], width = x[2], pixels = x[3], ll = x[4], ul = x[5], pitch = x[6], kind = x[7],
      a = x[8], b = x[9];
  RW(rd); RW(rp);
  int rk = reads(kind);
  sink_t out; sink_init(&out, m, rk);
  i32 end = data + size;
  u32 xo = 0;
  for (; data < end; data++) {
    i32 d = rd8(&rd, data);
    if (d == 255) {
      data++;
      i32 n = rd8(&rd, data);
      pixels += n; xo += (u32)n;
    } else if (d == 254) {
      pixels = (i32)((u32)pixels + (u32)width - xo); xo = (u32)width;
    } else if (d == 253) {
      pixels++; xo++;
    } else {
      if (pixels >= ul) break;
      if (pixels >= ll) put(&out, pixels, pixel(kind, d, rk ? rd8(&rp, pixels) : 0, a, b));
      pixels++; xo++;
    }
    if (xo >= (u32)width) { pixels = (i32)((u32)pixels + (u32)pitch - xo); xo = 0; }
  }
  done(&out);
  return 0;
}

/* TA_SPRITE2 (data, pixels, ll, ul, vpitch, kind, a) */
static i32 sprite2(machine *m) {
  i32 x[7]; acc_args(m, 7, x);
  i32 data = x[0], pixels = x[1], ll = x[2], ul = x[3], vpitch = x[4], kind = x[5], a = x[6];
  RW(rd); RW(rp);
  int rk = reads(kind);
  sink_t out; sink_init(&out, m, rk);
  for (i32 c; (c = rd8(&rd, data)) != 0x0f; data++) {
    pixels += c & 0x0f;
    i32 count = c >> 4;
    if (count == 0) { pixels = pixels + vpitch - 12; continue; }
    int stop = 0;
    while (count--) {
      data++;
      if (pixels >= ul) { stop = 1; break; }
      if (pixels >= ll) put(&out, pixels, pixel(kind, rd8(&rd, data), rk ? rd8(&rp, pixels) : 0, a, 0));
      pixels++;
    }
    if (stop) break;
  }
  done(&out);
  return 0;
}

/* TA_SPRITE2C (data, x, y, pixels, pitch, h, kind, a): clipped on every side */
static i32 sprite2c(machine *m) {
  i32 v[8]; acc_args(m, 8, v);
  i32 data = v[0], x = v[1], y = v[2], pix = v[3], pitch = v[4], h = v[5], kind = v[6], a = v[7];
  RW(rd); RW(wr);
  for (i32 c; (c = rd8(&rd, data)) != 0x0f; data++) {
    if (y >= h) break;
    i32 fill = c >> 4;
    x += c & 0x0f;
    if (fill == 0) { y++; x -= 12; }
    else if (y >= 0) {
      i32 row = pix + IMUL(y, pitch);
      do {
        data++;
        if (x >= 0 && x < pitch) wr8(&wr, row + x, pixel(kind, rd8(&rd, data), 0, a, 0));
        x++;
      } while (--fill);
    } else {
      data += fill;
      x += fill;
    }
  }
  return 0;
}

/* TA_BGROW (dst, pitch, x, y, map, kind): a row of 12 background tiles */
static i32 bgrow(machine *m) {
  i32 v[6]; acc_args(m, 6, v);
  i32 dst = v[0], pitch = v[1], x = v[2], y = v[3], map = v[4], kind = v[5];
  RW(r32); RW(rs); RW(rp);
  sink_t out; sink_init(&out, m, kind == 2);
  i32 ty = 0, th = 28;
  if (y < 0) { ty = -y; y = 0; } else if (y > 184 - th) th = 184 - y;
  i32 di = IMUL(y, pitch) + x;
  i32 tiles[12];
  for (int ti = 0; ti < 12; ti++) tiles[ti] = rd32(&r32, map + 4 * ti);
  for (; ty < th; ty++) {
    for (int ti = 0; ti < 12; ti++) {
      i32 data = tiles[ti];
      if (data == 0 && kind != 0) { di += 24; continue; }
      data = data + ty * 24;
      for (int tx = 0; tx < 24; tx++, data++, di++) {
        i32 s = rd8(&rs, data), at = dst + di;
        if (kind == 0) put(&out, at, s);
        else if (s != 0) put(&out, at, kind == 1 ? s : (s & 0xf0) | (((s & 0x0f) + (rd8(&rp, at) & 0x0f)) >> 1));
      }
    }
    di = di + pitch - 12 * 24;
  }
  done(&out);
  return 0;
}

/* TA_FILTER (pixels, pitch, kind, a): the play area through
 * filterScreenHue (kind 0) or filterScreenValue (1) */
static i32 filter(machine *m) {
  i32 v[4]; acc_args(m, 4, v);
  i32 pix = v[0], pitch = v[1], kind = v[2], a = v[3];
  if (kind == 1 && a == 0) return 0;
  RW(rp);
  sink_t out; sink_init(&out, m, 1);
  i32 hue = (i32)((u32)a << 4) & 0xff, i = 24;
  for (i32 y = 184; y > 0; y--) {
    for (i32 x = 0; x < 264; x++, i++) {
      i32 at = pix + i, p = rd8(&rp, at);
      if (kind == 0) put(&out, at, hue | (p & 0x0f));
      else {
        i32 w = (p & 0x0f) + a;
        put(&out, at, (p & 0xf0) | (w >= 0 && w <= 0xf ? w : w >= 0x10 && w <= 0x1e ? 0xf : 0));
      }
    }
    i += pitch - 264;
  }
  done(&out);
  return 0;
}

const acc_fn acc_tyrian[] = {sum, sprite, sprite2, sprite2c, bgrow, filter};
const int acc_tyrian_n = sizeof acc_tyrian / sizeof acc_tyrian[0];
#endif
