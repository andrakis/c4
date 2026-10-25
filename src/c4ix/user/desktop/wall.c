// wall.c -- the desktop's picture. docs/c4ix-desktop.md.
//
// The boot logo (/usr/share/splash/c4ix.c4p.gz, what splash shows),
// stretched over the whole desktop. It goes to the display once (and
// again when the screen changes size), as WALL_TX x WALL_TY tiles the
// display keeps (d_imgdef: each tile has
// to fit the command ring), and every redraw of the desktop's
// background draws the tiles it needs (d_image), clipped to the
// damage as everything else is. Settings turns it off and on
// (theme_set / theme_wall, /etc/desktop.cfg).
//
// The image is 256 colours, gzipped: "C4P8", width, height, colours
// (16-bit little-endian), the RGB palette, a byte per pixel
// (src/c4ix/art/mkc4p.py; user/splash.c reads the same).

#include "desktop.h"
#include "archive.h"

enum { WALL_TX = 8, WALL_TY = 8, WALL_ID = 100 };

static int wall_ok;                      // the tiles are on the display
static int wall_tw, wall_th;             // a tile's size on the desktop
static int wall_tile[(WK_MAX_W / WALL_TX) * (WK_MAX_H / WALL_TY)];
static char *wall_pix;                   // the picture, kept to stretch again
static int wall_w, wall_h, wall_lut[256];

// Give the display the tiles, stretched from the picture to the screen
// as it is now (nearest pixel: the logo is made at 800x600, so it is 1:1
// there). A new screen size needs nothing new from memory.
static void wall_tiles() {
    int tx, ty, x, y, sx, sy;
    wall_tw = wk.sw / WALL_TX;
    wall_th = wk.sh / WALL_TY;
    ty = 0;
    while (ty < WALL_TY) {
        tx = 0;
        while (tx < WALL_TX) {
            y = 0;
            while (y < wall_th) {
                sy = (ty * wall_th + y) * wall_h / wk.sh;
                x = 0;
                while (x < wall_tw) {
                    sx = (tx * wall_tw + x) * wall_w / wk.sw;
                    wall_tile[y * wall_tw + x] = wall_lut[wall_pix[sy * wall_w + sx] & 255];
                    ++x;
                }
                ++y;
            }
            d_imgdef(WALL_ID + ty * WALL_TX + tx, wall_tw, wall_th, wall_tile);
            ++tx;
        }
        ++ty;
    }
}

// Load the picture and give the display its tiles. 1 if it can be shown.
// The inflated picture is kept (the gzipped file is not).
int wall_load(char *path) {
    char *gz, *d;
    int glen, dlen, w, h, n, i;
    if (wall_ok) return 1;
    if (!(gz = arc_readfile(path, &glen))) return 0;
    i = arc_gunzip(gz, glen, &d, &dlen);
    free(gz);
    if (i < 0) return 0;
    if (dlen < 10 || d[0] != 'C' || d[1] != '4' || d[2] != 'P' || d[3] != '8') { free(d); return 0; }
    w = (d[4] & 255) | ((d[5] & 255) << 8);
    h = (d[6] & 255) | ((d[7] & 255) << 8);
    n = (d[8] & 255) | ((d[9] & 255) << 8);
    if (w < 1 || h < 1 || n < 1 || n > 256 || dlen < 10 + 3 * n + w * h) { free(d); return 0; }
    i = 0;
    while (i < n) {
        wall_lut[i] = ((d[10 + 3 * i] & 255) << 16) | ((d[11 + 3 * i] & 255) << 8) | (d[12 + 3 * i] & 255);
        ++i;
    }
    wall_pix = d + 10 + 3 * n;
    wall_w = w; wall_h = h;
    wall_tiles();
    wall_ok = 1;
    return 1;
}

// The screen changed size: the tiles again, at the new one.
void wall_resize() {
    if (wall_ok && (wall_tw != wk.sw / WALL_TX || wall_th != wk.sh / WALL_TY)) wall_tiles();
}

// The picture over X, Y, W, H, or 0 if there is none to draw.
int wall_draw(int x, int y, int w, int h) {
    int tx, ty;
    if (!wall_ok || !theme_wall()) return 0;
    ty = y / wall_th;
    while (ty < WALL_TY && ty * wall_th < y + h) {
        tx = x / wall_tw;
        while (tx < WALL_TX && tx * wall_tw < x + w) {
            d_image(WALL_ID + ty * WALL_TX + tx, tx * wall_tw, ty * wall_th);
            ++tx;
        }
        ++ty;
    }
    return 1;
}

int wall_shown() { return wall_ok && theme_wall(); }
