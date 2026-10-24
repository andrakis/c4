// look95.c -- window chrome: the Flat and Classic looks. docs/grin-plan.md 3.5.
//
// Classic is Windows 95's: a raised frame, a title bar shading from one
// colour to another, the window's icon and title on the left, and raised
// minimise, maximise and close buttons on the right. Flat keeps the
// layout and draws it with outlines and solid colours.

#include "wkwin.h"
#include "libjs/guest/gui.h"

void wk_gradient(int x, int y, int w, int h, int a, int b) {
    int i, n, bw;
    if (wk.style != S_CLASSIC) { gui_rect(x, y, w, h, a); return; }
    n = 16; bw = (w + n - 1) / n;
    i = 0;
    while (i < n) {
        if (i * bw < w) gui_rect(x + i * bw, y, (i + 1) * bw > w ? w - i * bw : bw, h, ui_mix(a, b, i, n - 1));
        ++i;
    }
}

// white on a dark bar, the scheme's dark on a light one
int wk_ink(int bg) {
    return (((bg >> 16) & 255) + ((bg >> 8) & 255) + (bg & 255)) < 384 ? C_WHITE : 0x404850;
}

enum { CAP_CLOSE = 0, CAP_MAX = 1, CAP_RESTORE = 2, CAP_MIN = 3 };
static int cap_ink, cap_bg;           // flat: the glyph's colour and the bar's under it

static void cap_button(int x, int y, int what) {
    int k;
    k = C_DARK;
    if (wk.style == S_FLAT) k = cap_ink;
    else { gui_rect(x, y, 16, 14, C_FACE); d_bevel(x, y, 16, 14, B_RAISED); }
    if (what == CAP_CLOSE) {                       // an X
        gui_line(x + 4, y + 3, x + 10, y + 9, k); gui_line(x + 5, y + 3, x + 11, y + 9, k);
        gui_line(x + 10, y + 3, x + 4, y + 9, k); gui_line(x + 11, y + 3, x + 5, y + 9, k);
    } else if (what == CAP_MAX) {
        gui_recto(x + 3, y + 2, 9, 9, k); gui_rect(x + 3, y + 3, 9, 1, k);
    } else if (what == CAP_RESTORE) {
        gui_recto(x + 5, y + 2, 7, 6, k); gui_rect(x + 5, y + 3, 7, 1, k);
        gui_rect(x + 3, y + 5, 7, 6, wk.style == S_FLAT ? cap_bg : C_FACE);
        gui_recto(x + 3, y + 5, 7, 6, k); gui_rect(x + 3, y + 6, 7, 1, k);
    } else gui_rect(x + 4, y + 9, 6, 2, k);        // minimise
}

static void frame95(struct win *w, int focused) {
    int tx, bx, by, ink;
    gui_rect(w->x, w->y, w->w, w->h, C_FACE);
    if (wk.style == S_FLAT) {
        cap_bg = focused ? C_TITLE : C_ITITLE;
        gui_rect(w->x, w->y, w->w, BORDER + TITLE_H, cap_bg);
        gui_recto(w->x, w->y, w->w - 1, w->h - 1, focused ? C_TITLE : C_SHADOW);
        ink = wk_ink(cap_bg);
        cap_ink = ink;
    } else {
        d_bevel(w->x, w->y, w->w, w->h, B_RAISED);
        if (focused) wk_gradient(w->x + BORDER, w->y + BORDER, w->w - 2 * BORDER, TITLE_H, C_TITLE, C_TITLE2);
        else wk_gradient(w->x + BORDER, w->y + BORDER, w->w - 2 * BORDER, TITLE_H, C_ITITLE, C_ITITLE2);
        ink = focused ? C_LIGHT : C_FACE;
    }
    tx = w->x + BORDER + 3;
    if (!w->dialog) { d_icon(w->kind, tx, w->y + BORDER + 1); tx = tx + 20; }
    wk_clip_screen(w->x + BORDER, w->y + BORDER, w->w - 2 * BORDER - 56, TITLE_H);
    d_text(tx, w->y + BORDER + 3, ink, F_BOLD, w->title);
    wk_noclip();
    by = w->y + BORDER + 2;
    bx = w->x + w->w - BORDER - 2 - 16;
    cap_button(bx, by, CAP_CLOSE);
    if (!w->dialog) {
        cap_button(bx - 18, by, w->max ? CAP_RESTORE : CAP_MAX);
        cap_button(bx - 34, by, CAP_MIN);
    }
}

static void menubar95(struct win *w, int focused, int open, int hot) {
    int k, x, y, lw;
    y = w->y + BORDER + TITLE_H + 1;
    gui_rect(wk_client_x(w), y, wk_client_w(w), MENU_H, C_FACE);
    x = wk_client_x(w) + 4;
    k = 0;
    while (k < w->nmenus) {
        lw = wk_menu_w(w, k);
        if (open == k) gui_rect(x, y + 1, lw, MENU_H - 2, C_SEL);
        d_text(x + 6, y + 4, open == k ? C_WHITE : C_DARK, F_SANS, w->menus[k].title);
        x = x + lw;
        ++k;
    }
}

// the size grip, over the client area's corner
static void grip95(struct win *w) {
    int bx, by;
    if (!w->resizable || w->max) return;
    bx = w->x + w->w - BORDER - 12; by = w->y + w->h - BORDER - 12;
    gui_line(bx + 3, by + 11, bx + 11, by + 3, C_LIGHT); gui_line(bx + 4, by + 11, bx + 11, by + 4, C_SHADOW);
    gui_line(bx + 7, by + 11, bx + 11, by + 7, C_LIGHT); gui_line(bx + 8, by + 11, bx + 11, by + 8, C_SHADOW);
}

static int hit95(struct win *w, int x, int y) {
    int bx, by;
    by = w->y + BORDER + 2;
    bx = w->x + w->w - BORDER - 2 - 16;
    if (ui_hit(x, y, bx, by, 16, 14)) return HT_CLOSE;
    if (!w->dialog && ui_hit(x, y, bx - 18, by, 16, 14)) return HT_MAX;
    if (!w->dialog && ui_hit(x, y, bx - 34, by, 16, 14)) return HT_MIN;
    if (y < w->y + BORDER + TITLE_H) return HT_CAPTION;
    if (w->resizable && !w->max && x >= w->x + w->w - 16 && y >= w->y + w->h - 16) return HT_SIZE + SZ_R + SZ_B;
    if (w->nmenus && y < wk_client_y(w)) return HT_MENUBAR;
    if (ui_hit(x, y, wk_client_x(w), wk_client_y(w), wk_client_w(w), wk_client_h(w))) return HT_CLIENT;
    return HT_NONE;
}

struct wk_look look_flat = { .frame = frame95, .menubar = menubar95, .hit = hit95, .grip = grip95 };
struct wk_look look_classic = { .frame = frame95, .menubar = menubar95, .hit = hit95, .grip = grip95 };
