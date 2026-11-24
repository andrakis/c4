// wall.c -- the desktop's picture. docs/c4ix-desktop.md.
//
// The boot logo (/usr/share/splash/c4ix.c4p.gz, what splash shows; on a
// wide screen, 16:9 or so, c4ix-wide.c4p.gz; the C4PC hypervisor's control
// C4IX's own, hv.c4p.gz and hv-wide.c4p.gz: wall_pick chooses, and
// chooses again when the screen's shape changes -- the one place that
// does, for SVGA II's wide modes),
// over the whole desktop at its own shape: scaled until it covers the
// screen, what is past an edge cut evenly from both sides (a screen of
// another shape than the picture's 4:3 no longer stretches it), its tiles
// rounded up so that the last column and row reach the screen's edge. It goes to the display once (and
// again when the screen changes size), as tiles the display keeps
// (GUI_IMGDEF), and every redraw of the desktop's background draws the
// tiles it needs (d_image), clipped to the damage as everything else is.
// A tile has to fit the display's command ring, and is kept to half of
// it (so that it fits wherever the ring's head is): 8 x 8 tiles, or more, smaller ones, on a large screen or with
// 8-byte words (1280x1024's tile is 160 KB at 64 bits, more than the
// desktop's first ring's half). A tile the ring refused was dropped, and
// the display kept the last size's in its place -- a grid of tiles with
// dark gaps -- until the screen's size changed again (a VMware user
// at 1920x1080, 2026-11-24). Now one refused is sent again at the next
// redraw, and if it is refused again, in smaller tiles. Settings turns it off and on
// (theme_set / theme_wall, /etc/desktop.cfg).
//
// The image is 256 colours, gzipped: "C4P8", width, height, colours
// (16-bit little-endian), the RGB palette, a byte per pixel
// (src/c4ix/art/mkc4p.py; user/splash.c reads the same).

#include "desktop.h"
#include "wkgui.h"                      // gui_frame, for a tile the ring may refuse
#include "archive.h"

// tile (tx, ty)'s image id: WALL_ID + ty * WALL_MAXN + tx (Machines'
// thumbnails are 300 up)
enum { WALL_N = 8, WALL_MAXN = 32, WALL_ID = 1000 };
static int wall_n = WALL_N;              // tiles each way
static int wall_short;                   // some tile the ring refused: send them again

static int wall_ok;                      // the tiles are on the display
static int wall_tw, wall_th;             // a tile's size on the desktop
static int *wall_tile, wall_tile_n;      // a tile's pixels, as large as the screen's tiles are
                                         // (SVGA II's modes go past WK_MAX_W: 1920x1080 overran
                                         // a buffer of WK_MAX_W x WK_MAX_H's tiles)
static char *wall_pix;                   // the picture, kept to stretch again
static char *wall_d;                     // ... its inflated file, freed for another
static int wall_wide;                    // the picture loaded is the 16:9 one
static int wall_w, wall_h, wall_lut[256];

// Give the display the tiles, stretched from the picture to the screen
// as it is now (nearest pixel: the logo is made at 800x600, so it is 1:1
// there). A new screen size needs nothing new from memory.
// One tile to the display: 0 if the ring would not take it
static int wall_def(int id, int w, int h, int *px) {
    int *p;
    if (!(p = gui_frame(GUI_IMGDEF, 3 + w * h))) return 0;
    p[0] = id; p[1] = w; p[2] = h;
    memcpy((char *)(p + 3), (char *)px, w * h * sizeof(int));
    gui_commit();
    return 1;
}
// how many tiles each way for this screen: 8, or more until one is at most
// half of the ring
static int wall_grid(int n) {
    int lim;
    lim = desk_ring_words() / 2;
    while (n < WALL_MAXN && ((wk.sw + n - 1) / n) * ((wk.sh + n - 1) / n) + 3 > lim) n = n + 4;
    return n < WALL_MAXN ? n : WALL_MAXN;
}
static void wall_tiles() {
    int tx, ty, x, y, sx, sy, dw, dh, offx, offy;
    wall_n = wall_grid(wall_short ? wall_n + 4 : WALL_N);
    wall_short = 0;
    wall_tw = (wk.sw + wall_n - 1) / wall_n;
    wall_th = (wk.sh + wall_n - 1) / wall_n;
    // the picture's size on the screen: covering it, at its own shape
    if (wk.sw * wall_h >= wk.sh * wall_w) { dw = wk.sw; dh = wall_h * wk.sw / wall_w; }
    else { dh = wk.sh; dw = wall_w * wk.sh / wall_h; }
    offx = (dw - wk.sw) / 2; offy = (dh - wk.sh) / 2;
    if (wall_tw * wall_th > wall_tile_n) {
        if (wall_tile) free(wall_tile);
        if (!(wall_tile = (int *)malloc(wall_tw * wall_th * sizeof(int)))) { wall_tile_n = 0; wall_ok = 0; return; }
        wall_tile_n = wall_tw * wall_th;
    }
    ty = 0;
    while (ty < wall_n) {
        tx = 0;
        while (tx < wall_n) {
            y = 0;
            while (y < wall_th) {
                sy = (ty * wall_th + y + offy) * wall_h / dh;
                if (sy >= wall_h) sy = wall_h - 1;
                x = 0;
                while (x < wall_tw) {
                    sx = (tx * wall_tw + x + offx) * wall_w / dw;
                    if (sx >= wall_w) sx = wall_w - 1;
                    wall_tile[y * wall_tw + x] = wall_lut[wall_pix[sy * wall_w + sx] & 255];
                    ++x;
                }
                ++y;
            }
            if (!wall_def(WALL_ID + ty * WALL_MAXN + tx, wall_tw, wall_th, wall_tile)) wall_short = 1;
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
    if (wall_d) free(wall_d);
    wall_d = d;
    wall_pix = d + 10 + 3 * n;
    wall_w = w; wall_h = h;
    wall_tiles();
    wall_ok = 1;
    return 1;
}

// is the screen wide: 16:9 or near it (16:10 counts), not 4:3 or 5:4
static int wall_screen_wide() { return wk.sw * 100 >= wk.sh * 155; }
// The picture for this desktop and this screen's shape, loaded: the
// hypervisor's control C4IX's own or C4IX's, 16:9 on a wide screen (the
// 4:3 one where there is no 16:9). 1 if one is
int wall_pick() {
    char path[64];
    int hv;
    hv = (__c4_info() & C4I_HV_BIT) != 0;
    ui_cpy(path, "/usr/share/splash/"); ui_cat(path, hv ? "hv" : "c4ix");
    if (wall_screen_wide()) {
        ui_cat(path, "-wide.c4p.gz");
        if (wall_load(path)) { wall_wide = 1; return 1; }
        ui_cpy(path, "/usr/share/splash/"); ui_cat(path, hv ? "hv" : "c4ix");
    }
    ui_cat(path, ".c4p.gz");
    if (wall_load(path)) { wall_wide = 0; return 1; }
    // the control C4IX's own missing (an older disk): C4IX's
    return hv && wall_load("/usr/share/splash/c4ix.c4p.gz");
}

// The screen changed size: the tiles again, at the new one -- or the
// other picture, where the screen's shape went from 4:3 to wide or back.
void wall_resize() {
    if (wall_ok && wall_wide != wall_screen_wide()) { wall_pick(); return; }
    if (wall_ok && (wall_n != wall_grid(WALL_N) || wall_tw != (wk.sw + wall_n - 1) / wall_n || wall_th != (wk.sh + wall_n - 1) / wall_n)) wall_tiles();
}

// The picture over X, Y, W, H, or 0 if there is none to draw.
int wall_draw(int x, int y, int w, int h) {
    int tx, ty;
    if (!wall_ok || !theme_wall()) return 0;
    // tiles the ring refused last time: again, before they are drawn
    if (wall_short && wall_n < WALL_MAXN) wall_tiles();
    ty = y / wall_th;
    while (ty < wall_n && ty * wall_th < y + h) {
        tx = x / wall_tw;
        while (tx < wall_n && tx * wall_tw < x + w) {
            d_image(WALL_ID + ty * WALL_MAXN + tx, tx * wall_tw, ty * wall_th);
            ++tx;
        }
        ++ty;
    }
    return 1;
}

int wall_shown() { return wall_ok && theme_wall(); }
