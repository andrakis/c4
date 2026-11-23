/* gui.c -- the display: libjs/gui-device.js's registers at 0x400, its
 * command and event rings (src/c4bb/sim/mbox.js), and in place of the
 * page's canvas (libjs/display.js) a canvas of c4pc's own, drawn here.
 *
 * A guest draws by writing command frames into a ring in its memory and
 * ringing the bell; events (mouse, keys, resize, focus) go back to it in
 * the other half of the same region. libjs queues the commands for the
 * page, which draws them later; c4pc draws each as it is taken from the
 * ring, into a back buffer, and a PRESENT (or a framebuffer frame) copies
 * what changed to the front, which the platform shows (pc.c: a VBE
 * framebuffer; host.c: a PNG when asked). A guest that never presents is
 * drawing in immediate mode, and each batch is shown as it is drawn, as
 * display.js shows it.
 *
 * The device's side is gui-device.js's, register for register: the same
 * answers to the same reads, the same frames in the rings, the same IRQ,
 * so that C4IX does on c4pc what it does on libjs. The drawing is
 * display.js's canvas calls done by hand: rectangles and lines on whole
 * pixels as a canvas strokes them at +0.5, circles and text with coverage,
 * images over what is there by their alpha, scaled frames by the nearest
 * pixel (imageSmoothingEnabled false). Text is in c4pc's faces (font.c),
 * not the browser's, so a string's width is the face's, near what the
 * window kit guesses. */
#include "c4pc.h"
#include "clib.h"
#include "font.h"

enum {
  CAPS = 0x400, W = 0x404, H = 0x408, RINGLEN = 0x40c, RING = 0x410, BELL = 0x414,
  EVMASK = 0x418, IRQ = 0x41c, MOUSE = 0x420, BUTTONS = 0x424, TICKS = 0x428, DROPPED = 0x42c,
  FB = 0x430, FBPITCH = 0x434, FBFLIP = 0x438, FBMODE = 0x43c, CLOCK = 0x440,
};
enum {
  C_CLEAR = 1, C_RECT = 2, C_RECTO = 3, C_LINE = 4, C_CIRCLE = 5, C_TEXT = 6, C_PIXEL = 7, C_PRESENT = 8,
  C_SIZE = 9, C_IMGDEF = 10, C_IMG = 11, C_CLIP = 12, C_NOCLIP = 13, C_TEXT2 = 14, C_CURSOR = 15, C_BLIT8 = 16,
};
enum { EV_MOVE = 1, EV_DOWN = 2, EV_UP = 3, EV_KEYDOWN = 4, EV_KEYUP = 5, EV_RESIZE = 6, EV_FOCUS = 7, EV_WHEEL = 8, EV_REL = 9 };
static const i32 MASK[10] = {0, 1, 2, 2, 4, 4, 8, 8, 16, 32};
#define PTR_NONE 7
#define PTR_LOCK 8

/* TEXT2's fonts (display.js FONTS) as c4pc's faces (mkfont.py's order:
 * DejaVu Sans Mono, Liberation Sans and Bold, Liberation Mono, DejaVu Sans
 * and Bold, Liberation Serif and Bold), and display.js's scale for each */
static const struct { int face, k100; } FONTMAP[9] = {
  {0, 100}, {1, 100}, {2, 100}, {3, 100}, {0, 100}, {4, 95}, {5, 95}, {6, 100}, {7, 100},
};

typedef struct { int w, h; u32 *px; u8 *alpha; } image;

struct gui {
  machine *m;
  int w, h;                            /* W and H: the display's size */
  i32 ring_len, ring_base; u32 ring_space;
  int mb; u32 tg_at, tg_cap, th_at, th_cap; i32 seq;   /* the rings, by word index */
  i32 evmask, irq_on, irq_count;
  i32 mouse_x, mouse_y, buttons, dropped;
  i32 fb_base, fb_pitch, fb_mode;
  i64 t0;
  int size_req;
  /* the canvas */
  u32 *back, *front; int cw, ch;
  int present_mode;
  int cx0, cy0, cx1, cy1;              /* the clip, inside the canvas */
  imap *images;
  int cursor;
  int bx0, by0, bx1, by1;              /* the back drawn on since the last present */
  int fx0, fy0, fx1, fy1;              /* the front changed since the platform last looked */
  /* the pointer, in the canvas */
  int px, py, pfx, pfy;                /* and its fraction, in 1/256 */
  u32 commands, presents, events, lost, flips;
  i32 *frames; u32 fcap;               /* frames taken from the ring, to draw */
};

/* ---- memory ----
 * The rings, the pixels and the palettes are the guest's words (libjs/
 * guest/gui.c): on the 64-bit machine 8 bytes each, a value in the low 32
 * bits, and text still four characters to a word. Indexes are words. */
static inline i32 rdw(gui *g, u32 idx) { return idx < (g->m->size >> WSH) ? (i32)g->m->w[idx] : 0; }
static inline void wrw(gui *g, u32 idx, i32 v) { if (idx < (g->m->size >> WSH)) g->m->w[idx] = (word)v; }
/* n words' low 32 bits, from the bytes of n words as read */
static __attribute__((unused)) void words32(u32 *out, const u8 *in, u32 n) {
  for (u32 k = 0; k < n; k++) out[k] = (u32)in[k * WB] | (u32)in[k * WB + 1] << 8 | (u32)in[k * WB + 2] << 16 | (u32)in[k * WB + 3] << 24;
}

/* ---- the canvas ---- */
static void dirty(gui *g, int x0, int y0, int x1, int y1) {
  { if (x0 < g->bx0) g->bx0 = x0; }
  { if (y0 < g->by0) g->by0 = y0; }
  { if (x1 > g->bx1) g->bx1 = x1; }
  { if (y1 > g->by1) g->by1 = y1; }
}
static void no_dirty(gui *g) { g->bx0 = g->by0 = 1 << 30; g->bx1 = g->by1 = -(1 << 30); }
static void noclip(gui *g) { g->cx0 = 0; g->cy0 = 0; g->cx1 = g->cw; g->cy1 = g->ch; }

static void canvas_new(gui *g, int w, int h) {
  if (g->back) { xfree(g->back); xfree(g->front); }
  g->cw = w; g->ch = h;
  g->back = xmalloc((size_t)w * h * 4);
  g->front = xmalloc((size_t)w * h * 4);
  noclip(g);
  no_dirty(g);
  g->fx0 = 0; g->fy0 = 0; g->fx1 = w; g->fy1 = h;
  g->present_mode = 0;
  if (g->px >= w) g->px = w - 1;
  if (g->py >= h) g->py = h - 1;
}

static void present(gui *g) {
  if (g->bx1 <= g->bx0 || g->by1 <= g->by0) return;
  int x0 = g->bx0 < 0 ? 0 : g->bx0, y0 = g->by0 < 0 ? 0 : g->by0;
  int x1 = g->bx1 > g->cw ? g->cw : g->bx1, y1 = g->by1 > g->ch ? g->ch : g->by1;
  for (int y = y0; y < y1; y++) memcpy(g->front + (size_t)y * g->cw + x0, g->back + (size_t)y * g->cw + x0, (size_t)(x1 - x0) * 4);
  { if (x0 < g->fx0) g->fx0 = x0; }
  { if (y0 < g->fy0) g->fy0 = y0; }
  { if (x1 > g->fx1) g->fx1 = x1; }
  { if (y1 > g->fy1) g->fy1 = y1; }
  no_dirty(g);
}

static inline void put(gui *g, int x, int y, u32 rgb) {
  if (x < g->cx0 || y < g->cy0 || x >= g->cx1 || y >= g->cy1) return;
  g->back[(size_t)y * g->cw + x] = rgb & 0xffffff;
}
static inline void blend(gui *g, int x, int y, u32 rgb, int a) {
  if (a <= 0 || x < g->cx0 || y < g->cy0 || x >= g->cx1 || y >= g->cy1) return;
  u32 *p = &g->back[(size_t)y * g->cw + x];
  if (a >= 255) { *p = rgb & 0xffffff; return; }
  u32 d = *p;
  u32 r = (((rgb >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a) + 127) / 255;
  u32 gg = (((rgb >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a) + 127) / 255;
  u32 b = ((rgb & 255) * a + (d & 255) * (255 - a) + 127) / 255;
  *p = r << 16 | gg << 8 | b;
}

static void fill(gui *g, int x, int y, int w, int h, u32 rgb) {
  if (w < 0) { x += w; w = -w; }
  if (h < 0) { y += h; h = -h; }
  int x0 = x < g->cx0 ? g->cx0 : x, y0 = y < g->cy0 ? g->cy0 : y;
  int x1 = x + w > g->cx1 ? g->cx1 : x + w, y1 = y + h > g->cy1 ? g->cy1 : y + h;
  if (x1 <= x0 || y1 <= y0) return;
  rgb &= 0xffffff;
  for (int yy = y0; yy < y1; yy++) {
    u32 *p = g->back + (size_t)yy * g->cw + x0;
    for (int xx = x0; xx < x1; xx++) *p++ = rgb;
  }
  dirty(g, x0, y0, x1, y1);
}

static void line(gui *g, int x0, int y0, int x1, int y1, u32 rgb) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
  dirty(g, (x0 < x1 ? x0 : x1), (y0 < y1 ? y0 : y1), (x0 > x1 ? x0 : x1) + 1, (y0 > y1 ? y0 : y1) + 1);
  for (int n = 0; n < 1 << 16; n++) {
    put(g, x0, y0, rgb);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

static double dsqrt(double v) {
#ifdef C4PC_BARE
  double r; __asm__("fsqrt" : "=t"(r) : "0"(v)); return r;
#else
  extern double sqrt(double); return sqrt(v);
#endif
}

/* arc() filled or stroked 1 wide, centred on (cx, cy): coverage by the
 * distance from the centre */
static void circle(gui *g, int cx, int cy, int r, u32 rgb, int filled) {
  if (r < 0) r = 0;
  int x0 = cx - r - 2, y0 = cy - r - 2, x1 = cx + r + 2, y1 = cy + r + 2;
  dirty(g, x0, y0, x1, y1);
  for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
    double ddx = x + 0.5 - cx, ddy = y + 0.5 - cy, d = dsqrt(ddx * ddx + ddy * ddy);
    double e = d - r, c = filled ? 0.5 - e : 1.0 - (e < 0 ? -e : e);
    if (c <= 0) continue;
    blend(g, x, y, rgb, c >= 1 ? 255 : (int)(c * 255));
  }
}

/* a string's bytes from a frame's words */
static int text_bytes(const i32 *w, int n, char *out) {
  for (int k = 0; k < n; k++) out[k] = (char)(((u32)w[k >> 2] >> ((k & 3) * 8)) & 0xff);
  return n;
}
static void glyph_at(gui *g, const glyph *gl, int x, int y, u32 rgb) {
  if (!gl) return;
  int ox = x + gl->x0, oy = y + gl->y0;
  dirty(g, ox, oy, ox + gl->w, oy + gl->h);
  for (int j = 0; j < gl->h; j++) for (int i = 0; i < gl->w; i++) {
    int a = gl->a[j * gl->w + i];
    if (a) blend(g, ox + i, oy + j, rgb, a);
  }
}
/* fillText at (x, y), textBaseline 'top'; or, with an advance, each
 * character that far after the last and spaces not drawn */
/* every string drawn, for a test (c4pc-host --texts; display.js recordText) */
void (*gui_text_hook)(int x, int y, const char *s, int n);
static void text(gui *g, int x, int y, u32 rgb, int size16, int font, int adv, const char *s, int n) {
  if (gui_text_hook) gui_text_hook(x, y, s, n);
  if (font < 0 || font > 8) font = font ? 1 : 0;
  int face = FONTMAP[font].face;
  if (size16 < 64) size16 = 64;                 /* Math.max(4, size) * k */
  size16 = size16 * FONTMAP[font].k100 / 100;
  int base = y + font_top(face, size16);
  i32 pen = x * 64;
  for (int k = 0; k < n; k++) {
    int c = (u8)s[k];
    if (adv) { if (c != ' ') glyph_at(g, font_glyph(face, size16, c), x + k * adv, base, rgb); continue; }
    glyph_at(g, font_glyph(face, size16, c), (pen + 32) >> 6, base, rgb);
    pen += font_advance(face, size16, c);
  }
}

/* ---- the rings (mbox.js HostMailbox) ---- */
#define HDR 3
static void mb_layout(gui *g) {
  wrw(g, g->tg_at, (i32)g->tg_cap); wrw(g, g->tg_at + 1, 0); wrw(g, g->tg_at + 2, 0);
  wrw(g, g->th_at, (i32)g->th_cap); wrw(g, g->th_at + 1, 0); wrw(g, g->th_at + 2, 0);
}
static int mb_send(gui *g, i32 type, const i32 *pl, int n) {
  u32 at0 = g->tg_at, cap = g->tg_cap;
  i32 len = HDR + n;
  i32 head = rdw(g, at0 + 1), tail = rdw(g, at0 + 2);
  i32 at = (i32)((u32)head % cap);
  i32 free_ = (i32)cap - (head - tail);
  if (at + len > (i32)cap) {
    if (free_ < ((i32)cap - at) + len) return 0;
    wrw(g, at0 + 3 + at, -1);
    head += (i32)cap - at;
    at = 0;
    free_ = (i32)cap - (head - tail);
  }
  if (free_ < len) return 0;
  u32 f = at0 + 3 + at;
  wrw(g, f + 1, type);
  wrw(g, f + 2, g->seq++);
  for (int i = 0; i < n; i++) wrw(g, f + HDR + i, pl[i]);
  wrw(g, f, len);
  wrw(g, at0 + 1, head + len);
  return 1;
}
/* every frame the guest has written, copied out as [len, type, ...] in
 * g->frames; the words in all, or -1 for a corrupt ring */
static i32 mb_drain(gui *g) {
  u32 at0 = g->th_at, cap = g->th_cap;
  i32 head = rdw(g, at0 + 1), tail = rdw(g, at0 + 2);
  u32 n = 0;
  while (tail != head) {
    i32 at = (i32)((u32)tail % cap);
    if (rdw(g, at0 + 3 + at) == -1) {
      tail += (i32)cap - at;
      if (tail == head) break;
      at = 0;
    }
    u32 f = at0 + 3 + at;
    i32 len = rdw(g, f);
    if (len < HDR || at + len > (i32)cap) return -1;
    u32 need = n + (u32)len - 1;
    if (need > g->fcap) { g->fcap = need * 2 + 1024; i32 *nf = xmalloc(g->fcap * 4); if (n) memcpy(nf, g->frames, n * 4); xfree(g->frames); g->frames = nf; }
    g->frames[n] = len - 1;                    /* [len, type, payload...]: the seq is dropped */
    g->frames[n + 1] = rdw(g, f + 1);
    for (i32 i = HDR; i < len; i++) g->frames[n + 2 + (i - HDR)] = rdw(g, f + (u32)i);
    n += (u32)len - 1;
    tail += len;
  }
  wrw(g, at0 + 2, tail);
  return (i32)n;
}

/* ---- the guest's memory, as the device reaches it (dev.c's dma) ---- */
/* a copy of a run of bytes; under a task's MMU checked page by page (its
 * fault, if it may not), or against the space that attached the ring when
 * the host drains on its own: 0 if that refuses it */
static int guest_read(gui *g, u32 addr, u32 len, u8 *out) {
  machine *m = g->m;
  if (!m->dma_ctx && g->ring_space) {
    for (u32 k = 0; k < len;) {
      u32 va = addr + k, r = len - k < 4096 - (va & 4095) ? len - k : 4096 - (va & 4095);
      i64 p = mmu_dma_space(m, g->ring_space, va, (i32)r, 0);
      if (p < 0 || p + r > m->size) return 0;
      memcpy(out + k, m->u8 + p, r);
      k += r;
    }
    return 1;
  }
  return dev_dma_read(m, addr, len, out) == len;
}

/* BLIT8 (x, y, w, h, src, pitch, pal, dw, dh): w x h bytes through the
 * palette, scaled to dw x dh by the nearest pixel */
static void blit8(gui *g, const i32 *p) {
  i32 x = p[0], y = p[1], w = p[2], h = p[3], src = p[4], pitch = p[5], pal = p[6], dw = p[7], dh = p[8];
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || pitch < w || (pal & (WB - 1))) return;
  u32 lut[256];
#if WB == 4
  if (!guest_read(g, (u32)pal, 1024, (u8 *)lut)) { g->lost++; return; }
#else
  { u8 raw[256 * WB]; if (!guest_read(g, (u32)pal, sizeof raw, raw)) { g->lost++; return; } words32(lut, raw, 256); }
#endif
  u8 *px = xmalloc((size_t)w * h);
  for (i32 j = 0; j < h; j++)
    if (!guest_read(g, (u32)src + (u32)j * (u32)pitch, (u32)w, px + (size_t)j * w)) { xfree(px); g->lost++; return; }
  if (dw <= 0) dw = w;
  if (dh <= 0) dh = h;
  int x0 = x < g->cx0 ? g->cx0 : x, y0 = y < g->cy0 ? g->cy0 : y;
  int x1 = x + dw > g->cx1 ? g->cx1 : x + dw, y1 = y + dh > g->cy1 ? g->cy1 : y + dh;
  for (int yy = y0; yy < y1; yy++) {
    int sy = (int)(((i64)(yy - y) * 2 + 1) * h / (2 * (i64)dh));
    u32 *d = g->back + (size_t)yy * g->cw;
    const u8 *row = px + (size_t)sy * w;
    for (int xx = x0; xx < x1; xx++) d[xx] = lut[row[(int)(((i64)(xx - x) * 2 + 1) * w / (2 * (i64)dw))]] & 0xffffff;
  }
  if (x1 > x0 && y1 > y0) dirty(g, x0, y0, x1, y1);
  xfree(px);
  g->commands++;
}

/* FBFLIP: w x h pixels from guest memory (rows, or columns), scaled to the
 * whole canvas, then presented */
static void flip(gui *g, int w, int h) {
  if (!w || !h || !g->fb_base) return;
  int mode = g->fb_mode, lines = mode ? w : h, run = mode ? h : w;
  i32 pitch = g->fb_pitch ? g->fb_pitch : run * WB;
  if ((g->fb_base & (WB - 1)) || (pitch & (WB - 1))) return;
  u32 *buf = xmalloc((size_t)w * h * 4);
#if WB == 4
  for (int l = 0; l < lines; l++)
    if (dev_dma_read(g->m, (u32)g->fb_base + (u32)l * (u32)pitch, (u32)run * 4, (u8 *)(buf + (size_t)l * run)) != (u32)run * 4) { xfree(buf); return; }
#else
  u8 *raw = xmalloc((size_t)run * WB);
  for (int l = 0; l < lines; l++) {
    if (dev_dma_read(g->m, (u32)g->fb_base + (u32)l * (u32)pitch, (u32)run * WB, raw) != (u32)run * WB) { xfree(raw); xfree(buf); return; }
    words32(buf + (size_t)l * run, raw, (u32)run);
  }
  xfree(raw);
#endif
  for (int yy = 0; yy < g->ch; yy++) {
    int sy = (int)(((i64)yy * 2 + 1) * h / (2 * (i64)g->ch));
    u32 *d = g->back + (size_t)yy * g->cw;
    for (int xx = 0; xx < g->cw; xx++) {
      int sx = (int)(((i64)xx * 2 + 1) * w / (2 * (i64)g->cw));
      d[xx] = (mode ? buf[(size_t)sx * h + sy] : buf[(size_t)sy * w + sx]) & 0xffffff;
    }
  }
  xfree(buf);
  dirty(g, 0, 0, g->cw, g->ch);
  g->flips++;
  g->present_mode = 1;
  present(g);
}

static void send(gui *g, i32 type, const i32 *pl, int n);

static void resize(gui *g, int w, int h) {
  { if (w < 1) w = 1; }
  { if (h < 1) h = 1; }
  { if (w > 4096) w = 4096; }
  { if (h > 4096) h = 4096; }
  if (w == g->cw && h == g->ch) return;
  canvas_new(g, w, h);
  /* the page tells the guest (Display.resize), and the device takes the size */
  i32 pl[2] = {w, h};
  send(g, EV_RESIZE, pl, 2);
}

static void image_def(gui *g, const i32 *p, int len) {
  i32 id = p[0], w = p[1], h = p[2];
  if (w <= 0 || h <= 0 || (i64)w * h > len - 5) return;
  image *im = imap_get(g->images, (u32)id);
  if (im) { xfree(im->px); xfree(im->alpha); xfree(im); }
  im = xmalloc(sizeof *im);
  im->w = w; im->h = h;
  im->px = xmalloc((size_t)w * h * 4);
  im->alpha = xmalloc((size_t)w * h);
  for (i32 k = 0; k < w * h; k++) {
    u32 v = (u32)p[3 + k];
    im->px[k] = v & 0xffffff;
    im->alpha[k] = (u8)(255 - (v >> 24));
  }
  imap_set(g->images, (u32)id, im);
}
static void image_draw(gui *g, i32 id, int x, int y) {
  image *im = imap_get(g->images, (u32)id);
  if (!im) return;
  dirty(g, x, y, x + im->w, y + im->h);
  for (int j = 0; j < im->h; j++) for (int i = 0; i < im->w; i++) {
    int a = im->alpha[j * im->w + i];
    if (a) blend(g, x + i, y + j, im->px[j * im->w + i], a);
  }
}

/* one frame's command (display.js draw), payload p of n words */
static void draw(gui *g, i32 type, const i32 *p, int len) {
  char s[4096];
  switch (type) {
    case C_CLEAR: fill(g, 0, 0, g->cw, g->ch, (u32)p[0]); break;     /* inside any clip, as a canvas's fillRect */
    case C_RECT: fill(g, p[0], p[1], p[2], p[3], (u32)p[4]); break;
    case C_RECTO: {
      /* strokeRect(x + .5, y + .5, w - 1, h - 1): the pixels on its edge */
      i32 x = p[0], y = p[1], w = p[2], h = p[3]; u32 c = (u32)p[4];
      if (w <= 0 || h <= 0) break;
      fill(g, x, y, w, 1, c); fill(g, x, y + h - 1, w, 1, c);
      fill(g, x, y, 1, h, c); fill(g, x + w - 1, y, 1, h, c);
      break;
    }
    case C_LINE: line(g, p[0], p[1], p[2], p[3], (u32)p[4]); break;
    case C_CIRCLE: circle(g, p[0], p[1], p[2], (u32)p[3], p[4] != 0); break;
    case C_TEXT: {
      int n = p[4]; if (n < 0) n = 0; if (n > (len - 7) * 4) n = (len - 7) * 4; if (n > 4095) n = 4095;
      text_bytes(p + 5, n, s);
      text(g, p[0], p[1], (u32)p[2], p[3] * 16, 0, 0, s, n);
      break;
    }
    case C_TEXT2: {
      int n = p[6]; if (n < 0) n = 0; if (n > (len - 9) * 4) n = (len - 9) * 4; if (n > 4095) n = 4095;
      text_bytes(p + 7, n, s);
      text(g, p[0], p[1], (u32)p[2], p[3] * 16, p[4], p[5], s, n);
      break;
    }
    case C_PIXEL: fill(g, p[0], p[1], 1, 1, (u32)p[2]); break;
    case C_PRESENT: g->present_mode = 1; present(g); g->presents++; break;
    case C_SIZE: resize(g, p[0] > 1 ? p[0] : 1, p[1] > 1 ? p[1] : 1); break;
    case C_CURSOR: g->cursor = p[0]; break;
    case C_IMGDEF: image_def(g, p, len); break;
    case C_IMG: image_draw(g, p[0], p[1], p[2]); break;
    case C_CLIP: {
      i32 x = p[0], y = p[1], w = p[2], h = p[3];
      g->cx0 = x < 0 ? 0 : x; g->cy0 = y < 0 ? 0 : y;
      g->cx1 = x + w > g->cw ? g->cw : x + w; g->cy1 = y + h > g->ch ? g->ch : y + h;
      if (w <= 0 || h <= 0) { g->cx1 = g->cx0; g->cy1 = g->cy0; }
      break;
    }
    case C_NOCLIP: noclip(g); break;
  }
}

/* gui-device.js pull: every command the guest has written, drawn */
static void pull(gui *g) {
  if (!g->mb) return;
  i32 n = mb_drain(g);
  if (n < 0) { mb_layout(g); return; }
  for (i32 i = 0; i < n;) {
    i32 len = g->frames[i], type = g->frames[i + 1];
    if (len < 2 || i + len > n) break;
    if (type == C_BLIT8) { if (len - 2 >= 9) blit8(g, g->frames + i + 2); }
    else { draw(g, type, g->frames + i + 2, len); g->commands++; }
    i += len;
  }
  if (!g->present_mode) present(g);
}

/* ---- events (gui-device.js event) ---- */
void (*gui_trace)(i32 type, const i32 *pl, int n, int sent);
static void send(gui *g, i32 type, const i32 *pl, int n) {
  if (gui_trace) gui_trace(type, pl, n, g->mb && (g->evmask & MASK[type]));
  if (!g->mb || !(g->evmask & MASK[type])) return;
  g->events++;
  if (!mb_send(g, type, pl, n)) { g->dropped++; return; }
  if (g->irq_on) g->irq_count++;
}

/* ---- the platform's side ---- */
gui *gui_new(machine *m, int w, int h) {
  gui *g = xmalloc(sizeof *g);
  g->m = m;
  g->w = w; g->h = h;
  g->evmask = 2 | 4 | 8;
  g->images = imap_new();
  g->t0 = plat_now_ms();
  canvas_new(g, w, h);
  g->px = w / 2; g->py = h / 2;
  return g;
}

i32 gui_read32(gui *g, u32 a) {
  switch (a) {
    case CAPS: return 1 | 2 | 4 | 8;
    case FB: return g->fb_base;
    case FBPITCH: return g->fb_pitch;
    case FBMODE: return g->fb_mode;
    case W: return g->w;
    case H: return g->h;
    case RINGLEN: return g->ring_len;
    case RING: return g->ring_base;
    case BELL: return g->mb && rdw(g, g->tg_at + 1) != rdw(g, g->tg_at + 2) ? 1 : 0;
    case EVMASK: return g->evmask;
    case IRQ: return g->irq_on;
    case MOUSE: return (i32)(((u32)g->mouse_x & 0xffff) << 16 | ((u32)g->mouse_y & 0xffff));
    case BUTTONS: return g->buttons;
    case TICKS: return (i32)(plat_now_ms() - g->t0);
    case DROPPED: return g->dropped;
    case CLOCK: return plat_clock_secs();
    default: return 0;
  }
}

void gui_write32(gui *g, u32 a, i32 v) {
  switch (a) {
    case W: g->w = v < 1 ? 1 : v > 4096 ? 4096 : v; g->size_req = 1; return;
    case H: g->h = v < 1 ? 1 : v > 4096 ? 4096 : v; g->size_req = 1; return;
    case RINGLEN: g->ring_len = v; return;
    case RING: {
      if (!v) { g->ring_base = 0; g->mb = 0; return; }
      i32 len = g->ring_len & ~(2 * WB - 1);
      if ((v & (WB - 1)) || len < 256) { g->ring_base = 0; g->mb = 0; return; }
      /* the task's own memory to write, or its page fault */
      i64 pa = dev_dma(g->m, (u32)v, len, 1, 1);
      if (pa < 0x1000 || pa + len > g->m->size) { g->ring_base = 0; g->mb = 0; return; }
      g->ring_base = v;
      g->ring_space = g->m->dma_ctx ? (u32)mmu_ptbr(g->m->dma_ctx) : 0;
      u32 half = (u32)len >> 1;
      g->tg_at = (u32)pa >> WSH; g->tg_cap = (half >> WSH) - 3;
      g->th_at = ((u32)pa + half) >> WSH; g->th_cap = (half >> WSH) - 3;
      g->seq = 0;
      g->mb = 1;
      mb_layout(g);
      return;
    }
    case BELL: pull(g); return;
    case EVMASK: g->evmask = v; return;
    case FB: g->fb_base = v; return;
    case FBPITCH: g->fb_pitch = v; return;
    case FBMODE: g->fb_mode = v & 1; return;
    case FBFLIP: pull(g); flip(g, (int)(((u32)v >> 16) & 0xffff), (int)(v & 0xffff)); return;
    case IRQ: g->irq_on = v ? 1 : 0; return;
    default: return;
  }
}

int gui_irq_pending(gui *g) { return g->irq_count > 0; }
void gui_irq_taken(gui *g) { g->irq_count = 0; }

/* after a slice (the worker's drain and its size request): what the guest
 * wrote and did not ring for, and a size it asked for */
void gui_tick(gui *g) {
  pull(g);
  if (g->size_req) { g->size_req = 0; resize(g, g->w, g->h); }
}

static void pointer_event(gui *g, i32 type, const i32 *pl, int n) {
  g->mouse_x = g->px; g->mouse_y = g->py;
  send(g, type, pl, n);
}

/* the mouse moved by dx, dy (canvas pixels in 1/256) with these buttons
 * down (bit 0 left, 1 middle, 2 right) and the wheel turned */
void gui_mouse(gui *g, int dx256, int dy256, int buttons, int wheel) {
  int locked = g->cursor == PTR_LOCK;
  int fx = g->pfx + dx256, fy = g->pfy + dy256;
  int dx = fx / 256, dy = fy / 256;
  g->pfx = fx - dx * 256; g->pfy = fy - dy * 256;
  if (dx || dy) {
    g->px += dx; g->py += dy;
    { if (g->px < 0) g->px = 0; }
    { if (g->px >= g->cw) g->px = g->cw - 1; }
    { if (g->py < 0) g->py = 0; }
    { if (g->py >= g->ch) g->py = g->ch - 1; }
    /* the move with the buttons as they were: a PS/2 packet carries the
     * motion and the buttons together, where the page's mousemove never
     * changes them; taken as the new buttons here, a press or a release
     * in a packet that also moved sent no DOWN or UP, and Duke Nukem 3D
     * kept firing after the button let go */
    if (locked && (g->evmask & MASK[EV_REL])) { i32 pl[3] = {dx, dy, g->buttons}; pointer_event(g, EV_REL, pl, 3); }
    else { i32 pl[3] = {g->px, g->py, g->buttons}; pointer_event(g, EV_MOVE, pl, 3); }
  }
  for (int b = 0; b < 3; b++) {
    int was = (g->buttons >> b) & 1, now = (buttons >> b) & 1;
    if (was == now) continue;
    i32 pl[3] = {g->px, g->py, b};
    if (now) g->buttons |= 1 << b; else g->buttons &= ~(1 << b);
    pointer_event(g, now ? EV_DOWN : EV_UP, pl, 3);
  }
  if (wheel) { i32 pl[3] = {g->px, g->py, wheel > 0 ? 1 : -1}; pointer_event(g, EV_WHEEL, pl, 3); }
}
/* ... or to x, y (a scripted test's) */
void gui_mouse_at(gui *g, int x, int y, int buttons) {
  gui_mouse(g, (x - g->px) * 256 - g->pfx, (y - g->py) * 256 - g->pfy, buttons, 0);
}
void gui_key(gui *g, int down, int code, int ch, int mods) {
  i32 pl[3] = {code, ch, mods};
  send(g, down ? EV_KEYDOWN : EV_KEYUP, pl, 3);
}
void gui_focus(gui *g, int on) { i32 pl[1] = {on ? 1 : 0}; send(g, EV_FOCUS, pl, 1); }

void gui_size(gui *g, int *w, int *h) { *w = g->cw; *h = g->ch; }
int gui_locked(gui *g) { return g->cursor == PTR_LOCK; }
int gui_attached(gui *g) { return g->mb && (g->evmask & 4); }
int gui_ring(gui *g) { return g->mb; }
u32 gui_flips(gui *g) { return g->flips; }

u32 *gui_front(gui *g, int *w, int *h) { *w = g->cw; *h = g->ch; return g->front; }
int gui_take_dirty(gui *g, int *x0, int *y0, int *x1, int *y1) {
  if (g->fx1 <= g->fx0 || g->fy1 <= g->fy0) return 0;
  *x0 = g->fx0; *y0 = g->fy0; *x1 = g->fx1; *y1 = g->fy1;
  g->fx0 = g->fy0 = 1 << 30; g->fx1 = g->fy1 = -(1 << 30);
  return 1;
}
int gui_pointer(gui *g, int *x, int *y) {
  *x = g->px; *y = g->py;
  return g->cursor == PTR_NONE || g->cursor == PTR_LOCK ? -1 : g->cursor;
}
void gui_stats(gui *g, u32 *commands, u32 *presents, u32 *events, u32 *flips) {
  *commands = g->commands; *presents = g->presents; *events = g->events; *flips = g->flips;
}
