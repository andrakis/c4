// C4IX userland: splash -- the loading screen.
//
//   splash -p PID [IMAGE]   until task PID is gone (vfsload -w, at boot)
//   splash [IMAGE]          until a key or a click
//
// The boot logo full screen, and along the bottom a band of colour that
// flows to the right, as the loading screens of the late nineties did
// with their palettes. vfsload -w (init --desktop) runs it while the
// root archive unpacks, so the display has something to say before the
// desktop has a filesystem to start on. It lets go of the display
// before it exits, so the desktop can take it.
//
// The screen's shape chooses the picture -- c4ix.c4p.gz (4:3) or
// c4ix-wide.c4p.gz (16:9), the C4PC hypervisor's control C4IX hv.c4p.gz
// or hv-wide.c4p.gz -- and the canvas, 800 wide and as tall as the screen
// is for that; the picture covers it at its own proportions, what is
// past an edge cut evenly from both (the wordmark at the top and the band
// at the bottom stay: the picture's shape is the screen's or near it).
// IMAGE (/usr/share/splash/c4ix.c4p.gz) is 256 colours, gzipped:
// "C4P8", width and height (16 bits each, little-endian), the number of
// colours (16 bits), that many RGB triples, then a byte per pixel, row
// by row. src/c4ix/art/mkc4p.py makes it from the PNG Kiln drew.
//
// On a host without the display it says "not fitted" and leaves.

#include "c4ix_user.h"
#include "archive.h"

#define printf uprintf
#define malloc ualloc

#include "libjs/guest/gui.h"

enum { SW = 800, BAR_H = 10, SEG = 16, NCOL = 24 };
static int SH, BAR_Y;

static int ramp[NCOL];

// the bar's colours: teal to frosting pink to cream and back, as the logo
static void ramp_init() {
    int i, k, t, r, g, b;
    i = 0;
    while (i < NCOL) {
        k = i < NCOL / 2 ? i : NCOL - i;            // 0 .. NCOL/2 .. 1
        t = k * 255 / (NCOL / 2);
        if (t < 128) {                              // teal 0x1f8f8f -> pink 0xf0a0c8
            r = 0x1f + (0xf0 - 0x1f) * t / 128;
            g = 0x8f + (0xa0 - 0x8f) * t / 128;
            b = 0x8f + (0xc8 - 0x8f) * t / 128;
        } else {                                    // pink -> cream 0xfff0c8
            t = t - 128;
            r = 0xf0 + (0xff - 0xf0) * t / 127;
            g = 0xa0 + (0xf0 - 0xa0) * t / 127;
            b = 0xc8;
        }
        ramp[i] = (r << 16) | (g << 8) | b;
        ++i;
    }
}

// The logo as 0xRRGGBB words, or 0
static int *load(char *path, int *pw, int *ph) {
    char *gz, *d, *pix;
    int glen, dlen, w, h, n, i, lut[256], *px;
    if (!(gz = arc_readfile(path, &glen))) return 0;
    if (arc_gunzip(gz, glen, &d, &dlen) < 0) return 0;
    if (dlen < 10 || d[0] != 'C' || d[1] != '4' || d[2] != 'P' || d[3] != '8') return 0;
    w = (d[4] & 255) | ((d[5] & 255) << 8);
    h = (d[6] & 255) | ((d[7] & 255) << 8);
    n = (d[8] & 255) | ((d[9] & 255) << 8);
    if (n < 1 || n > 256 || dlen < 10 + 3 * n + w * h) return 0;
    i = 0;
    while (i < n) {
        lut[i] = ((d[10 + 3 * i] & 255) << 16) | ((d[11 + 3 * i] & 255) << 8) | (d[12 + 3 * i] & 255);
        ++i;
    }
    pix = d + 10 + 3 * n;
    if (!(px = (int *)ualloc(w * h * sizeof(int)))) return 0;
    i = 0;
    while (i < w * h) { px[i] = lut[pix[i] & 255]; ++i; }
    *pw = w; *ph = h;
    return px;
}

// Is task PID still there? (A zombie has finished.)
static int alive(int pid) {
    int i, info[32];
    i = 0;
    while (utaskinfo(i, info)) {
        if (info[0] == pid) return info[2] != 3;    // TS_ZOMBIE
        ++i;
    }
    return 0;
}

// D made S, and S added to D
static void s_cpy(char *d, char *s) { while (*s) { *d = *s; ++d; ++s; } *d = 0; }
static void s_cat(char *d, char *s) { while (*d) ++d; s_cpy(d, s); }
// PX (W x H) covering a canvas of SW x SH at its own proportions, cut
// evenly where it is past an edge: a new picture, or 0
static int *cover(int *px, int w, int h) {
    int *out, x, y, dw, dh, ox, oy, sx, sy;
    if (!(out = (int *)ualloc(SW * SH * sizeof(int)))) return 0;
    if (SW * h >= SH * w) { dw = SW; dh = h * SW / w; } else { dh = SH; dw = w * SH / h; }
    ox = (dw - SW) / 2; oy = (dh - SH) / 2;
    y = 0;
    while (y < SH) {
        sy = (y + oy) * h / dh; if (sy >= h) sy = h - 1;
        x = 0;
        while (x < SW) {
            sx = (x + ox) * w / dw; if (sx >= w) sx = w - 1;
            out[y * SW + x] = px[sy * w + sx];
            ++x;
        }
        ++y;
    }
    return out;
}
int main(int argc, char **argv) {
    int *region, *px, ev[4], pid, i, w, h, phase, k, x, sw, sh, wide, given;
    char *image;
    char path[64];
    pid = 0; given = 0;
    image = "/usr/share/splash/c4ix.c4p.gz";
    i = 1;
    while (i < argc) {
        if (!ustrcmp(argv[i], "-p") && i + 1 < argc) { ++i; pid = 0; k = 0; while (argv[i][k]) { pid = pid * 10 + argv[i][k] - '0'; ++k; } }
        else { image = argv[i]; given = 1; }
        ++i;
    }
    if (!gui_present()) { uprintf("splash: not fitted\n"); return 0; }
    // the screen's shape: the canvas's, and the picture's
    sw = *(int *)GUI_W; sh = *(int *)GUI_H;
    if (sw < 320 || sh < 200) { sw = 800; sh = 600; }
    SH = SW * sh / sw;
    if (SH < 400) SH = 400;
    if (SH > 640) SH = 640;
    BAR_Y = SH - 34;
    wide = sw * 100 >= sh * 155;
    if (!given) {
        s_cpy(path, "/usr/share/splash/"); s_cat(path, (__c4_info() & 0x400000) ? "hv" : "c4ix");
        s_cat(path, wide ? "-wide.c4p.gz" : ".c4p.gz");
        image = path;
    }
    region = ualloc(65536);
    if (!gui_attach(region, 65536, SW, SH)) { uprintf("splash: the display refused the ring\n"); return 1; }
    if (!pid) gui_events(GUI_M_KEYS | GUI_M_BUTTONS);
    gui_cursor(GUI_PTR_WAIT);
    ramp_init();

    // the wide picture missing: the 4:3; the hypervisor's missing: C4IX's
    if (!given && !(px = load(image, &w, &h))) {
        s_cpy(path, "/usr/share/splash/"); s_cat(path, (__c4_info() & 0x400000) ? "hv.c4p.gz" : "c4ix.c4p.gz");
        if (!(px = load(path, &w, &h))) image = "/usr/share/splash/c4ix.c4p.gz";
    } else if (given) px = load(image, &w, &h);
    if (!px) px = load(image, &w, &h);
    if (px && (w != SW || h != SH)) px = cover(px, w, h);
    if (px) {
        gui_fb(px, 0);
        gui_flip(SW, SH);
    } else {
        // no logo: say the name, at least
        gui_clear(0x0b2a3a);
        gui_text2(SW / 2 - 90, SH / 2 - 40, 0xf0a0c8, 64, GUI_SANS_BOLD, 0, "C4IX", 0 - 1);
        gui_text2(SW / 2 - 150, SH / 2 + 40, 0xffffff, 18, GUI_SANS, 0, "Built upon Robert Swierczek's C4", 0 - 1);
        gui_show();
    }

    phase = 0;
    while (1) {
        if (pid) { if (!alive(pid)) break; }
        else if (gui_poll(ev) && (ev[0] == GUI_EV_KEYDOWN || ev[0] == GUI_EV_DOWN)) break;
        // the band flows right: each segment takes the colour of the one
        // to its left on the frame before
        x = 0; k = 0;
        while (x < SW) {
            gui_rect(x, BAR_Y, SEG, BAR_H, ramp[(k - phase + NCOL * 1000) % NCOL]);
            x = x + SEG; ++k;
        }
        gui_show();
        ++phase;
        umsleep(60);
    }
    gui_cursor(GUI_PTR_ARROW);
    gui_detach();
    return 0;
}
