// clock.c -- Clock: the time of day, on a face or in figures, as a window
// kit class. docs/grin-plan.md 4. The smallest application, and the one
// grin's loader is first proved with.

#include "winkit.h"

enum { C_ANALOG = 0, C_DIGITAL = 1 };
struct clock { int digital; };

// sin(6k degrees) * 1000, k = 0..15: a minute's step around the face
static int sine[16] = { 0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995, 1000 };
static int sin60(int k) {
    k = k % 60;
    if (k < 0) k = k + 60;
    if (k <= 15) return sine[k];
    if (k <= 30) return sine[30 - k];
    if (k <= 45) return 0 - sine[k - 30];
    return 0 - sine[60 - k];
}

static char *settings_items[3] = { "Analog", "Digital", 0 };
static struct wk_menu menus[1];
static char tbuf[16];

static void hand(int cx, int cy, int pos, int len, int thick, int rgb) {
    int x, y;
    x = cx + len * sin60(pos) / 1000;
    y = cy - len * sin60(pos + 15) / 1000;
    d_line(cx, cy, x, y, rgb);
    if (thick) { d_line(cx + 1, cy, x + 1, y, rgb); d_line(cx, cy + 1, x, y + 1, rgb); }
}

static void two(char *p, int v) { p[0] = '0' + v / 10; p[1] = '0' + v % 10; }

static void clock_draw(int w, int cw, int ch, int focused) {
    struct clock *c;
    int t, h, m, s, cx, cy, r, k, x, y;
    c = (struct clock *)wk_state(w);
    t = wk_clock();
    h = t / 3600; m = (t / 60) % 60; s = t % 60;
    d_rect(0, 0, cw, ch, C_FACE);
    if (c->digital) {
        two(tbuf, h); tbuf[2] = ':'; two(tbuf + 3, m); tbuf[5] = ':'; two(tbuf + 6, s); tbuf[8] = 0;
        d_textn((cw - 8 * 15) / 2, (ch - 24) / 2, C_DARK, 24, F_MONO, 15, tbuf, 8);
        return;
    }
    cx = cw / 2; cy = ch / 2;
    r = (cw < ch ? cw : ch) / 2 - 8;
    if (r < 10) r = 10;
    d_disc(cx, cy, r + 2, C_SHADOW);
    d_disc(cx, cy, r, C_WHITE);
    // the hours as squares, the minutes as dots
    k = 0;
    while (k < 60) {
        x = cx + (r - 6) * sin60(k) / 1000;
        y = cy - (r - 6) * sin60(k + 15) / 1000;
        if (k % 5 == 0) d_rect(x - 2, y - 2, 4, 4, C_TITLE);
        else d_rect(x, y, 1, 1, C_SHADOW);
        ++k;
    }
    hand(cx, cy, (h % 12) * 5 + m / 12, r * 5 / 10, 1, C_DARK);
    hand(cx, cy, m, r * 8 / 10, 1, C_DARK);
    hand(cx, cy, s, r * 9 / 10, 0, 0xc02020);
    d_disc(cx, cy, 3, C_DARK);
}

static void clock_tick(int w, int id) { wk_invalidate(w, 0, 0, 0, 0); }

static void clock_command(int w, int id) {
    struct clock *c;
    c = (struct clock *)wk_state(w);
    c->digital = id % WK_CMD_STEP == C_DIGITAL;
    wk_invalidate(w, 0, 0, 0, 0);
}

static void clock_open_hook(int w, char *arg) {
    menus[0].title = "Settings"; menus[0].items = settings_items;
    wk_menu(w, menus, 1);
    wk_timer(w, 1, 1000);
}

struct wk_class clock_class = {
    .kind = ICON_CLOCK, .open = clock_open_hook, .draw = clock_draw, .timer = clock_tick,
    .command = clock_command
};

int clock_open() {
    clock_class.size = sizeof(struct clock);
    return wk_open(&clock_class, "Clock", 240, 260, 0, 0);
}
