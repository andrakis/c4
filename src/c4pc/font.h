/* font.h -- the display's text (font.c): outlines from mkfont.py's
 * tables, filled at any size with coverage, kept once made */
#ifndef C4PC_FONT_H
#define C4PC_FONT_H

/* a face: units per em, the ascent and descent (hhea: the descent
 * negative), and for each of the characters 0-255 its advance and its
 * contours, each a closed polygon of points in font units (y up) */
typedef struct {
  u16 upem; i16 asc, desc;
  const u16 *adv, *first;      /* first: its first contour's index */
  const u8 *ncont;
  const u16 *cnt;              /* points in each contour */
  const u32 *cstart;           /* where each contour's points start */
  const i16 *pts;              /* x, y pairs */
} font_face;
extern const font_face font_faces[];
extern const int font_nfaces;

/* a glyph at a size, as coverage: w x h bytes (0-255), its top left
 * (x0, y0) from the pen on the baseline, y down */
typedef struct { int x0, y0, w, h; u8 *a; } glyph;

/* size: the em in 1/16 pixels. The glyph, made once and kept; 0 for one
 * with nothing to draw */
const glyph *font_glyph(int face, int size16, int ch);
/* how far the pen moves after ch, in 1/64 pixels */
int font_advance(int face, int size16, int ch);
/* from the em square's top (canvas's textBaseline 'top') to the baseline,
 * in pixels */
int font_top(int face, int size16);

#endif
