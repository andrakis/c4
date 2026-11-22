/* font.c -- glyphs for the display's text (gui.c), from the outlines
 * mkfont.py writes. A glyph is filled at the size asked with 4 x 4 samples
 * a pixel under the nonzero rule, as coverage (anti-aliased, unhinted, as
 * a browser draws a canvas's text), and kept in a table: text is most of
 * what a desktop draws, again and again in the same few sizes. */
#include "c4pc.h"
#include "clib.h"
#include "font.h"

#define SS 4                        /* samples a pixel, each way */
#define MAXDIM 512                  /* a glyph bigger than this is not drawn */

typedef struct { double x0, y0, x1, y1; int dir; } edge;

/* ---- the table of glyphs made ---- */
typedef struct { int face, size16, ch; glyph *g; } slot;
#define SLOTS 8192
static slot *tab;
static int used;
static glyph empty;

static void flush_all(void) {
  for (int i = 0; i < SLOTS; i++) if (tab[i].g) { if (tab[i].g != &empty) { xfree(tab[i].g->a); xfree(tab[i].g); } tab[i].g = 0; }
  used = 0;
}

static double scale_of(const font_face *f, int size16) { return size16 / 16.0 / f->upem; }

int font_advance(int face, int size16, int ch) {
  if (face < 0 || face >= font_nfaces) return 0;
  const font_face *f = &font_faces[face];
  double v = f->adv[ch & 255] * scale_of(f, size16) * 64;
  return (int)(v + 0.5);
}

int font_top(int face, int size16) {
  if (face < 0 || face >= font_nfaces) return 0;
  const font_face *f = &font_faces[face];
  double em = size16 / 16.0;
  return (int)(em * f->asc / (double)(f->asc - f->desc) + 0.5);
}

static int cmp_d(double a, double b) { return a < b ? -1 : a > b; }

static glyph *make(const font_face *f, int size16, int ch) {
  int nc = f->ncont[ch];
  if (!nc) return &empty;
  double s = scale_of(f, size16);
  u32 c0 = f->first[ch];
  /* the bounds, in pixels, y down */
  double minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9;
  int ne = 0;
  for (int c = 0; c < nc; c++) {
    u32 n = f->cnt[c0 + c], st = f->cstart[c0 + c];
    ne += (int)n;
    for (u32 k = 0; k < n; k++) {
      double x = f->pts[2 * (st + k)] * s, y = -f->pts[2 * (st + k) + 1] * s;
      { if (x < minx) minx = x; }
      { if (x > maxx) maxx = x; }
      { if (y < miny) miny = y; }
      { if (y > maxy) maxy = y; }
    }
  }
  int x0 = (int)(minx >= 0 ? minx : minx - 1), y0 = (int)(miny >= 0 ? miny : miny - 1);
  int w = (int)maxx + 2 - x0, h = (int)maxy + 2 - y0;
  if (w <= 0 || h <= 0 || w > MAXDIM || h > MAXDIM) return &empty;
  /* the edges, in samples, from the box's corner */
  edge *e = xmalloc(sizeof *e * (ne ? ne : 1));
  int m = 0;
  for (int c = 0; c < nc; c++) {
    u32 n = f->cnt[c0 + c], st = f->cstart[c0 + c];
    for (u32 k = 0; k < n; k++) {
      u32 a = st + k, b = st + (k + 1 == n ? 0 : k + 1);
      double ax = (f->pts[2 * a] * s - x0) * SS, ay = (-f->pts[2 * a + 1] * s - y0) * SS;
      double bx = (f->pts[2 * b] * s - x0) * SS, by = (-f->pts[2 * b + 1] * s - y0) * SS;
      if (ay == by) continue;
      if (ay < by) e[m] = (edge){ax, ay, bx, by, 1};
      else e[m] = (edge){bx, by, ax, ay, -1};
      m++;
    }
  }
  glyph *g = xmalloc(sizeof *g);
  g->x0 = x0; g->y0 = y0; g->w = w; g->h = h;
  g->a = xmalloc((size_t)w * h);
  u16 *acc = xmalloc(sizeof *acc * (size_t)w);
  double *xs = xmalloc(sizeof *xs * (m ? m : 1));
  int *ds = xmalloc(sizeof *ds * (m ? m : 1));
  for (int row = 0; row < h; row++) {
    memset(acc, 0, sizeof *acc * (size_t)w);
    for (int sub = 0; sub < SS; sub++) {
      double sy = row * SS + sub + 0.5;
      int nx = 0;
      for (int i = 0; i < m; i++) {
        if (sy < e[i].y0 || sy >= e[i].y1) continue;
        double t = (sy - e[i].y0) / (e[i].y1 - e[i].y0);
        double x = e[i].x0 + t * (e[i].x1 - e[i].x0);
        /* in order by x, as they come */
        int j = nx++;
        while (j > 0 && cmp_d(xs[j - 1], x) > 0) { xs[j] = xs[j - 1]; ds[j] = ds[j - 1]; j--; }
        xs[j] = x; ds[j] = e[i].dir;
      }
      /* nonzero: inside while the winding is not 0 */
      int wind = 0;
      for (int i = 0; i < nx; i++) {
        wind += ds[i];
        if (!wind || i + 1 >= nx) continue;
        /* the samples whose centres are in [xa, xb): ceil(x - 0.5) each */
        double ta = xs[i] - 0.5, tb = xs[i + 1] - 0.5;
        int ca = ta <= 0 ? 0 : (int)ta + ((double)(int)ta < ta);
        int cb = tb <= 0 ? 0 : (int)tb + ((double)(int)tb < tb);
        if (cb > w * SS) cb = w * SS;
        for (int c = ca; c < cb; c++) acc[c / SS]++;
      }
    }
    for (int x = 0; x < w; x++) {
      int v = acc[x] * 255 / (SS * SS);
      g->a[row * w + x] = (u8)(v > 255 ? 255 : v);
    }
  }
  xfree(acc); xfree(xs); xfree(ds); xfree(e);
  return g;
}

const glyph *font_glyph(int face, int size16, int ch) {
  if (face < 0 || face >= font_nfaces || size16 <= 0) return 0;
  ch &= 255;
  if (!tab) tab = xmalloc(sizeof *tab * SLOTS);
  u32 hsh = ((u32)face * 2654435761u) ^ ((u32)size16 * 40503u) ^ ((u32)ch * 2246822519u);
  for (u32 i = 0; i < SLOTS; i++) {
    slot *sl = &tab[(hsh + i) & (SLOTS - 1)];
    if (!sl->g) break;
    if (sl->face == face && sl->size16 == size16 && sl->ch == ch) return sl->g == &empty ? 0 : sl->g;
  }
  if (used >= SLOTS * 3 / 4) flush_all();
  glyph *g = make(&font_faces[face], size16, ch);
  for (u32 i = 0; ; i++) {
    slot *sl = &tab[(hsh + i) & (SLOTS - 1)];
    if (sl->g) continue;
    sl->face = face; sl->size16 = size16; sl->ch = ch; sl->g = g;
    used++;
    break;
  }
  return g == &empty ? 0 : g;
}
