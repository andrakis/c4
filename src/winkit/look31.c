// look31.c -- window chrome: Windows 3.1. docs/grin-plan.md 3.5.
//
// A thick sizing border, black-edged and notched where a drag moves two
// edges at once; a solid title bar with the title centred in it; the
// control-menu box on its left (a click opens the window's menu, a double
// click closes the window); and raised minimise and maximise buttons on
// its right, arrows pointing down and up (both, once maximised, for
// Restore). There is no close button: 3.1 closed from the control menu.
// A dialog has the box alone, and a frame in the title's colour. The
// measures are the other looks', so a window's client area is the same
// size whichever look draws it.

#include "wkwin.h"
#include "wkgui.h"

enum { BOX = TITLE_H, NOTCH = TITLE_H + BORDER };

// arrowheads four rows deep: up has its point at y, down at y + 3
static void up(int cx, int y) { int i; i = 0; while (i < 4) { gui_rect(cx - i, y + i, 2 * i + 1, 1, C_DARK); ++i; } }
static void down(int cx, int y) { int i; i = 0; while (i < 4) { gui_rect(cx - i, y + 3 - i, 2 * i + 1, 1, C_DARK); ++i; } }

// a raised button on the title bar, black on its left
static void caption_button(int x, int y, int what) {
    int cx;
    gui_rect(x, y, 1, BOX, C_DARK);
    gui_rect(x + 1, y, BOX - 1, BOX, C_FACE);
    gui_rect(x + 1, y, BOX - 1, 1, C_LIGHT); gui_rect(x + 1, y, 1, BOX - 1, C_LIGHT);
    gui_rect(x + 1, y + BOX - 1, BOX - 1, 1, C_SHADOW); gui_rect(x + BOX - 1, y, 1, BOX, C_SHADOW);
    gui_rect(x + 2, y + BOX - 2, BOX - 3, 1, C_SHADOW); gui_rect(x + BOX - 2, y + 1, 1, BOX - 2, C_SHADOW);
    cx = x + BOX / 2;
    if (what == HT_MIN) down(cx, y + 7);
    else if (what == HT_MAX) up(cx, y + 7);
    else { up(cx, y + 3); down(cx, y + 10); }
}

// the control-menu box: a bar like a space bar, on the face
static void sysbox(int x, int y) {
    gui_rect(x, y, BOX - 1, BOX, C_FACE);
    gui_rect(x + BOX - 1, y, 1, BOX, C_DARK);
    gui_rect(x + 4, y + 8, 11, 3, C_SHADOW);
    gui_rect(x + 3, y + 7, 11, 3, C_WHITE);
    gui_recto(x + 3, y + 7, 10, 2, C_DARK);
}

static void frame31(struct win *w, int focused) {
    int bg, ink, x1, x2, tw, tx, edge, x, y;
    x = w->x; y = w->y;
    // the frame: black outside and in, the border's colour between
    edge = w->dialog ? (focused ? C_TITLE : C_ITITLE) : C_FACE;
    gui_rect(x, y, w->w, w->h, edge);
    gui_recto(x, y, w->w - 1, w->h - 1, C_DARK);
    gui_recto(x + BORDER - 1, y + BORDER - 1, w->w - 2 * BORDER + 1, w->h - 2 * BORDER + 1, C_DARK);
    if (w->resizable && !w->max) {
        // the notches: where a drag on the border moves two edges
        gui_rect(x + NOTCH, y, 1, BORDER, C_DARK);            gui_rect(x + w->w - 1 - NOTCH, y, 1, BORDER, C_DARK);
        gui_rect(x + NOTCH, y + w->h - BORDER, 1, BORDER, C_DARK); gui_rect(x + w->w - 1 - NOTCH, y + w->h - BORDER, 1, BORDER, C_DARK);
        gui_rect(x, y + NOTCH, BORDER, 1, C_DARK);            gui_rect(x, y + w->h - 1 - NOTCH, BORDER, 1, C_DARK);
        gui_rect(x + w->w - BORDER, y + NOTCH, BORDER, 1, C_DARK); gui_rect(x + w->w - BORDER, y + w->h - 1 - NOTCH, BORDER, 1, C_DARK);
    }
    // inside: the face, then the title bar and the black line under it
    gui_rect(x + BORDER, y + BORDER, w->w - 2 * BORDER, w->h - 2 * BORDER, C_FACE);
    bg = focused ? C_TITLE : C_LIGHT;
    gui_rect(x + BORDER, y + BORDER, w->w - 2 * BORDER, TITLE_H, bg);
    gui_rect(x + BORDER, y + BORDER + TITLE_H, w->w - 2 * BORDER, 1, C_DARK);
    sysbox(x + BORDER, y + BORDER);
    x1 = x + BORDER + BOX;
    x2 = x + w->w - BORDER;
    if (!w->dialog) {
        x2 = x2 - 2 * BOX;
        caption_button(x2, y + BORDER, HT_MIN);
        caption_button(x2 + BOX, y + BORDER, w->max ? HT_NONE : HT_MAX);
    }
    // the title, centred between the box and the buttons (its width a
    // guess: the host's fonts cannot be measured from here)
    ink = wk_ink(bg);
    tw = ui_len(w->title) * 7;
    tx = x1 + (x2 - x1 - tw) / 2;
    if (tx < x1 + 4) tx = x1 + 4;
    wk_clip_screen(x1, y + BORDER, x2 - x1 > 0 ? x2 - x1 : 0, TITLE_H);
    d_text(tx, y + BORDER + 3, ink, F_BOLD, w->title);
    wk_noclip();
}

// the menu bar: black on white, the menu open shown inverted, a black line under it
static void menubar31(struct win *w, int focused, int open, int hot) {
    int k, x, y, lw;
    y = w->y + BORDER + TITLE_H + 1;
    gui_rect(wk_client_x(w), y, wk_client_w(w), MENU_H - 1, C_WHITE);
    gui_rect(wk_client_x(w), y + MENU_H - 1, wk_client_w(w), 1, C_DARK);
    x = wk_client_x(w) + 4;
    k = 0;
    while (k < w->nmenus) {
        lw = wk_menu_w(w, k);
        if (open == k) gui_rect(x, y + 1, lw, MENU_H - 3, C_SEL);
        d_text(x + 6, y + 4, open == k ? C_WHITE : C_DARK, F_SANS, w->menus[k].title);
        x = x + lw;
        ++k;
    }
}

static int hit31(struct win *w, int x, int y) {
    int e, x2, ty;
    ty = w->y + BORDER;
    if (ui_hit(x, y, w->x + BORDER, ty, BOX, TITLE_H)) return HT_SYSMENU;
    x2 = w->x + w->w - BORDER - 2 * BOX;
    if (!w->dialog && ui_hit(x, y, x2, ty, BOX, TITLE_H)) return HT_MIN;
    if (!w->dialog && ui_hit(x, y, x2 + BOX, ty, BOX, TITLE_H)) return HT_MAX;
    // the sizing border, its ends moving two edges
    if (w->resizable && !w->max) {
        e = 0;
        if (x < w->x + BORDER) e = SZ_L;
        else if (x >= w->x + w->w - BORDER) e = SZ_R;
        if (y < w->y + BORDER) e = e | SZ_T;
        else if (y >= w->y + w->h - BORDER) e = e | SZ_B;
        if (e) {
            if (e == SZ_L || e == SZ_R) {
                if (y < w->y + NOTCH) e = e | SZ_T;
                else if (y >= w->y + w->h - NOTCH) e = e | SZ_B;
            } else if (e == SZ_T || e == SZ_B) {
                if (x < w->x + NOTCH) e = e | SZ_L;
                else if (x >= w->x + w->w - NOTCH) e = e | SZ_R;
            }
            return HT_SIZE + e;
        }
    }
    if (y < w->y + BORDER + TITLE_H) return HT_CAPTION;
    if (w->nmenus && y < wk_client_y(w)) return HT_MENUBAR;
    if (ui_hit(x, y, wk_client_x(w), wk_client_y(w), wk_client_w(w), wk_client_h(w))) return HT_CLIENT;
    return HT_NONE;
}

struct wk_look look_win31 = { .frame = frame31, .menubar = menubar31, .hit = hit31, .title_centred = 1 };
