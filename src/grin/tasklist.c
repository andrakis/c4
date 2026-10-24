// tasklist.c -- the Task List (Ctrl+Esc, or a double click on the
// desktop), as Windows 3.1's: the windows open, Switch To, End Task and
// Cancel, and Cascade, Tile and Arrange Icons for all of them.

#include "shell.h"

enum { ROWS = 8, ROW = 16, LIST_X = 10, LIST_Y = 10, LIST_W = 300, MAX_TASKS = 64,
       BW = 92, BH = 24, B1_Y = LIST_Y + ROWS * ROW + 14, B2_Y = B1_Y + BH + 8 };

struct tasklist {
    char *buf; int max;               // struct grin_dlg's
    int sel, top;
    int n, ids[MAX_TASKS];            // the windows listed, refreshed as it draws
};

static char *labels[6] = { "Switch To", "End Task", "Cancel", "Cascade", "Tile", "Arrange Icons" };

// the windows a person would switch to: every one but the dialogs (this among them)
static void refresh(struct tasklist *t) {
    struct win *w;
    int keep, k;
    keep = t->sel >= 0 && t->sel < t->n ? t->ids[t->sel] : 0;
    t->n = 0;
    w = win_first();
    while (w && t->n < MAX_TASKS) {
        if (w->used && !w->dialog) { t->ids[t->n] = w->id; ++t->n; }
        w = w->next;
    }
    t->sel = t->n ? 0 : 0 - 1;
    k = 0;
    while (k < t->n) { if (t->ids[k] == keep) t->sel = k; ++k; }
    if (t->top > t->sel) t->top = t->sel;
    if (t->sel >= t->top + ROWS) t->top = t->sel - ROWS + 1;
    if (t->top < 0) t->top = 0;
}

static int button_x(int k) { return LIST_X + (k % 3) * (BW + 12); }
static int button_y(int k) { return k < 3 ? B1_Y : B2_Y; }

static void tl_draw(int w, int cw, int ch, int focused) {
    struct tasklist *t;
    int k, y;
    t = (struct tasklist *)wk_state(w);
    refresh(t);
    d_rect(0, 0, cw, ch, C_FACE);
    d_rect(LIST_X, LIST_Y, LIST_W, ROWS * ROW + 4, C_WHITE);
    d_bevel(LIST_X, LIST_Y, LIST_W, ROWS * ROW + 4, B_SUNKEN);
    d_clip(LIST_X + 2, LIST_Y + 2, LIST_W - 4, ROWS * ROW);
    k = t->top; y = LIST_Y + 2;
    while (k < t->n && k < t->top + ROWS) {
        if (k == t->sel) d_rect(LIST_X + 2, y, LIST_W - 4, ROW, C_SEL);
        d_text(LIST_X + 6, y + 2, k == t->sel ? C_WHITE : C_DARK, F_SANS, wk_get_title(t->ids[k]));
        y = y + ROW;
        ++k;
    }
    d_clip_end();
    k = 0;
    while (k < 6) { ui_button(button_x(k), button_y(k), BW, BH, labels[k], 0, k == 0); ++k; }
}

static void close_list(int w) { wk_close(w); }

// Cascade and Tile: the windows showing, out of maximised, leaving the row
// of icons along the bottom clear when there are icons
static int area_h() {
    struct win *w;
    w = win_first();
    while (w) { if (w->used && w->min && !w->dialog) return wk.ah - 64; w = w->next; }
    return wk.ah;
}

static void unmax(struct win *w) {
    if (w->max) { w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh; w->max = 0; }
}

void tasklist_cascade() {
    struct win *w;
    int k, step, ww, hh, f, ah;
    f = wk_focus();
    ah = area_h();
    step = TITLE_H + BORDER + 4;
    k = 0;
    w = win_first();
    while (w) {
        if (w->used && !w->dialog && !w->min) {
            unmax(w);
            ww = w->w; hh = w->h;
            if (w->resizable) { ww = wk.aw * 2 / 3; hh = ah * 2 / 3; }
            if (wk.ax + k * step + ww > wk.ax + wk.aw || wk.ay + k * step + hh > wk.ay + ah) k = 0;
            wk_place(w->id, wk.ax + k * step, wk.ay + k * step, ww, hh);
            wk_raise(w->id);
            ++k;
        }
        w = w->next;
    }
    if (f) wk_raise(f);
    wk_redraw_all();
}

void tasklist_tile() {
    struct win *w;
    int n, cols, rows, k, cw, chh, ah, x, y;
    ah = area_h();
    n = 0;
    w = win_first();
    while (w) { if (w->used && !w->dialog && !w->min) ++n; w = w->next; }
    if (!n) return;
    cols = 1;
    while (cols * cols < n) ++cols;
    rows = (n + cols - 1) / cols;
    // side by side first, as 3.1 tiled two windows
    if (n == 2) { cols = 2; rows = 1; }
    cw = wk.aw / cols; chh = ah / rows;
    k = 0;
    w = win_first();
    while (w) {
        if (w->used && !w->dialog && !w->min) {
            unmax(w);
            x = wk.ax + (k % cols) * cw; y = wk.ay + (k / cols) * chh;
            if (w->resizable) wk_place(w->id, x, y, cw, chh);
            else wk_place(w->id, x, y, w->w, w->h);
            ++k;
        }
        w = w->next;
    }
    wk_redraw_all();
}

static void act(int w, int k) {
    struct tasklist *t;
    int target;
    t = (struct tasklist *)wk_state(w);
    target = t->sel >= 0 && t->sel < t->n ? t->ids[t->sel] : 0;
    close_list(w);
    if (k == 0 && target) wk_raise(target);
    else if (k == 1 && target) wk_ask_close(target);
    else if (k == 3) tasklist_cascade();
    else if (k == 4) tasklist_tile();
    else if (k == 5) wk_arrange_icons();
    wk_redraw_all();
}

static void tl_mouse(int w, int ev, int x, int y, int b) {
    struct tasklist *t;
    int k;
    t = (struct tasklist *)wk_state(w);
    if (ev == E_WHEEL) {
        t->top = t->top + b;
        if (t->top > t->n - ROWS) t->top = t->n - ROWS;
        if (t->top < 0) t->top = 0;
        return;
    }
    if (ev != E_DOWN && ev != E_DBL) return;
    if (ui_hit(x, y, LIST_X, LIST_Y, LIST_W, ROWS * ROW + 4)) {
        k = t->top + (y - LIST_Y - 2) / ROW;
        if (k >= 0 && k < t->n) { t->sel = k; if (ev == E_DBL) act(w, 0); }
        return;
    }
    if (ev != E_DOWN) return;
    k = 0;
    while (k < 6) { if (ui_hit(x, y, button_x(k), button_y(k), BW, BH)) { act(w, k); return; } ++k; }
}

static void tl_key(int w, int down, int code, int ch, int mods) {
    struct tasklist *t;
    t = (struct tasklist *)wk_state(w);
    if (!down) return;
    if (code == KEY_ESC) { close_list(w); return; }
    if (code == KEY_ENTER) { act(w, 0); return; }
    if (code == KEY_UP && t->sel > 0) --t->sel;
    if (code == KEY_DOWN && t->sel < t->n - 1) ++t->sel;
    if (t->sel < t->top) t->top = t->sel;
    if (t->sel >= t->top + ROWS) t->top = t->sel - ROWS + 1;
}

static struct wk_class tasklist_class = { .kind = ICON_TASKS, .draw = tl_draw, .mouse = tl_mouse, .key = tl_key };

int tasklist_open() {
    int w, f;
    struct tasklist *t;
    if ((w = wk_find(&tasklist_class))) { wk_raise(w); return w; }
    f = wk_focus();
    if ((w = grin_dialog(&tasklist_class, sizeof(struct tasklist), "Task List",
                         2 * BORDER + LIST_X * 2 + LIST_W, B2_Y + BH + 12 + 2 * BORDER + TITLE_H + 1)) < 0) return w;
    t = (struct tasklist *)wk_state(w);
    t->sel = 0 - 1;
    refresh(t);
    // the window that had the keyboard is the one chosen
    t->sel = 0;
    while (t->sel < t->n && t->ids[t->sel] != f) ++t->sel;
    if (t->sel >= t->n) t->sel = t->n ? 0 : 0 - 1;
    return w;
}
