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

enum { SW = 800, SH = 600, BAR_Y = 566, BAR_H = 10, SEG = 16, NCOL = 24 };

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

int main(int argc, char **argv) {
    int *region, *px, ev[4], pid, i, w, h, phase, k, x;
    char *image;
    pid = 0;
    image = "/usr/share/splash/c4ix.c4p.gz";
    i = 1;
    while (i < argc) {
        if (!ustrcmp(argv[i], "-p") && i + 1 < argc) { ++i; pid = 0; k = 0; while (argv[i][k]) { pid = pid * 10 + argv[i][k] - '0'; ++k; } }
        else image = argv[i];
        ++i;
    }
    if (!gui_present()) { uprintf("splash: not fitted\n"); return 0; }
    region = ualloc(65536);
    if (!gui_attach(region, 65536, SW, SH)) { uprintf("splash: the display refused the ring\n"); return 1; }
    if (!pid) gui_events(GUI_M_KEYS | GUI_M_BUTTONS);
    gui_cursor(GUI_PTR_WAIT);
    ramp_init();

    if ((px = load(image, &w, &h))) {
        gui_fb(px, 0);
        gui_flip(w, h);
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
