// wm.c -- the window manager. docs/c4ix-desktop.md.
//
// The only module that touches the display: it owns the rings, reads
// mouse and keys, draws the desktop, the windows' frames, the taskbar,
// the Start menu, menu bars and dialogs, and asks each window's tool to
// draw its client area (clipped to it) and to handle what happens there.

#include "desktop.h"
#define printf uprintf
#include "libjs/guest/gui.h"

static struct win *wins;
static int order[MAXW + 1], nz;          // z-order, bottom first
static int dirty, running;
static int start_open, start_hot;
static int drag, drag_dx, drag_dy;       // window index + 1 while dragging a title bar
static int sizing, size_w0, size_h0, size_x0, size_y0;
static int down_win, down_t, down_x, down_y;   // for double clicks
static int icon_t, icon_k;
static int menu_win, menu_idx, menu_hot; // an open drop-down: window + 1, menu, hot item
static int *ev;
static int clip_on;
static char dtext[128];                  // the input dialog's text
static int dcaret;
static char tbuf[128];

// ---- the drawing layer -------------------------------------------------------

void d_rect(int x, int y, int w, int h, int rgb) { if (w > 0 && h > 0) gui_rect(x, y, w, h, rgb); }
void d_recto(int x, int y, int w, int h, int rgb) { gui_recto(x, y, w, h, rgb); }
void d_line(int x0, int y0, int x1, int y1, int rgb) { gui_line(x0, y0, x1, y1, rgb); }
void d_disc(int x, int y, int r, int rgb) { gui_disc(x, y, r, rgb); }
void d_text(int x, int y, int rgb, int font, char *s) { gui_text2(x, y, rgb, 11, font, 0, s, 0 - 1); }
void d_textn(int x, int y, int rgb, int size, int font, int advance, char *s, int n) {
    gui_text2(x, y, rgb, size, font, advance, s, n);
}

// The Windows bevels: two pixels, light top-left, dark bottom-right (or
// the other way round for a sunken one).
void d_bevel(int x, int y, int w, int h, int style) {
    int a, b, c, d;
    if (style == B_ETCHED) {
        gui_recto(x, y, w - 1, h - 1, C_SHADOW);
        gui_recto(x + 1, y + 1, w - 1, h - 1, C_LIGHT);
        return;
    }
    if (style == B_RAISED) { a = C_LIGHT; b = C_DARK; c = C_FACE; d = C_SHADOW; }
    else if (style == B_PRESSED) { a = C_DARK; b = C_LIGHT; c = C_SHADOW; d = C_FACE; }
    else { a = C_SHADOW; b = C_LIGHT; c = C_DARK; d = C_FACE; }
    gui_rect(x, y, w, 1, a);             gui_rect(x, y, 1, h, a);
    gui_rect(x, y + h - 1, w, 1, b);     gui_rect(x + w - 1, y, 1, h, b);
    gui_rect(x + 1, y + 1, w - 2, 1, c); gui_rect(x + 1, y + 1, 1, h - 2, c);
    gui_rect(x + 1, y + h - 2, w - 2, 1, d); gui_rect(x + w - 2, y + 1, 1, h - 2, d);
}

// 16x16 pictures for each kind of window: title bars, the taskbar, icons.
void d_icon(int kind, int x, int y) {
    if (kind == K_TERM) {
        gui_rect(x, y + 1, 16, 13, C_DARK); gui_recto(x, y + 1, 16, 13, C_SHADOW);
        gui_text2(x + 2, y + 3, 0xc0c0c0, 8, F_MONO, 0, ">_", 2);
    } else if (kind == K_FILES) {
        gui_rect(x, y + 3, 7, 3, 0xc8a000); gui_rect(x, y + 5, 16, 10, 0xffd84a);
        gui_recto(x, y + 5, 16, 10, 0x806000);
    } else if (kind == K_TASKS) {
        gui_rect(x, y + 1, 16, 12, C_DARK); gui_recto(x, y + 1, 16, 12, C_SHADOW);
        gui_line(x + 2, y + 10, x + 6, y + 5, C_GREEN); gui_line(x + 6, y + 5, x + 9, y + 8, C_GREEN);
        gui_line(x + 9, y + 8, x + 14, y + 3, C_GREEN); gui_rect(x + 5, y + 13, 6, 2, C_SHADOW);
    } else if (kind == K_NOTE) {
        gui_rect(x + 2, y, 12, 16, C_WHITE); gui_recto(x + 2, y, 12, 16, C_SHADOW);
        gui_rect(x + 4, y + 4, 8, 1, 0x4060c0); gui_rect(x + 4, y + 7, 8, 1, 0x4060c0);
        gui_rect(x + 4, y + 10, 6, 1, 0x4060c0);
    } else if (kind == K_CALC) {
        gui_rect(x + 2, y, 12, 16, 0xa0a0a0); gui_recto(x + 2, y, 12, 16, C_DARK);
        gui_rect(x + 4, y + 2, 8, 4, 0xd0ffd0);
        gui_rect(x + 4, y + 8, 3, 2, C_DARK); gui_rect(x + 9, y + 8, 3, 2, C_DARK);
        gui_rect(x + 4, y + 12, 3, 2, C_DARK); gui_rect(x + 9, y + 12, 3, 2, 0xc02020);
    } else {
        gui_rect(x + 1, y + 1, 14, 14, C_WHITE); gui_recto(x + 1, y + 1, 14, 14, C_TITLE);
        gui_text2(x + 6, y + 2, C_TITLE, 12, F_BOLD, 0, "i", 1);
    }
}

// ---- windows ---------------------------------------------------------------

struct win *win_get(int i) { if (i < 0 || i >= MAXW) return 0; return wins + i; }
int win_index(struct win *w) { int i; i = 0; while (i < MAXW) { if (wins + i == w) return i; ++i; } return 0 - 1; }
void mark_dirty() { dirty = 1; }
int now_ms() { return gui_ticks(); }
char *dialog_text() { return dtext; }

static int client_x(struct win *w) { return w->x + BORDER; }
static int menu_h(struct win *w) { return w->nmenus ? MENU_H : 0; }
static int client_y(struct win *w) { return w->y + BORDER + TITLE_H + 1 + menu_h(w); }
static int client_w(struct win *w) { return w->w - 2 * BORDER; }
static int client_h(struct win *w) { return w->h - (client_y(w) - w->y) - BORDER; }

void win_raise(int i) {
    int k, j;
    k = 0;
    while (k < nz && order[k] != i) ++k;
    if (k == nz) { order[nz] = i; ++nz; dirty = 1; return; }
    j = k;
    while (j < nz - 1) { order[j] = order[j + 1]; ++j; }
    order[nz - 1] = i;
    wins[i].min = 0;
    dirty = 1;
}

static void unlink_z(int i) {
    int k, j;
    k = 0;
    while (k < nz && order[k] != i) ++k;
    if (k == nz) return;
    j = k;
    while (j < nz - 1) { order[j] = order[j + 1]; ++j; }
    --nz;
}

// The topmost window that is showing, or -1.
int win_focused() {
    int k;
    k = nz - 1;
    while (k >= 0) { if (!wins[order[k]].min) return order[k]; --k; }
    return 0 - 1;
}

// A dialog on top keeps the input to itself.
static int modal() {
    int f;
    f = win_focused();
    if (f >= 0 && (wins[f].kind == K_MSG || wins[f].kind == K_INPUT)) return f;
    return 0 - 1;
}

int win_open(int kind, char *title, int w, int h) {
    int i;
    struct win *p;
    i = 0;
    while (i < MAXW && wins[i].used) ++i;
    if (i == MAXW) return 0 - 1;
    p = wins + i;
    if (!p->st) p->st = (char *)ualloc(ST_BYTES);
    memset(p->st, 0, ST_BYTES);
    p->used = 1; p->kind = kind; p->min = 0; p->max = 0; p->resizable = 1;
    p->nmenus = 0; p->owner = 0 - 1; p->action = 0;
    p->x = 100 + i * 24; p->y = 20 + i * 22;
    if (p->x + w > SW) p->x = SW - w; if (p->x < 0) p->x = 0;
    if (p->y + h > SH - TASK_H) p->y = SH - TASK_H - h; if (p->y < 0) p->y = 0;
    p->w = w; p->h = h;
    ui_cpy(p->title, title);
    win_raise(i);
    return i;
}

void win_title(struct win *w, char *s) { ui_cpy(w->title, s); dirty = 1; }

void win_menus(struct win *w, int n, char **titles, char ***items) {
    w->nmenus = n; w->menu_titles = titles; w->menu_items = items;
}

void win_close(int i) {
    struct win *w;
    w = wins + i;
    if (!w->used) return;
    if (w->kind == K_TERM) term_close(w);
    w->used = 0;
    unlink_z(i);
    if (drag == i + 1) drag = 0;
    if (sizing == i + 1) sizing = 0;
    if (menu_win == i + 1) menu_win = 0;
    dirty = 1;
}

// ---- dialogs ---------------------------------------------------------------------

void msgbox(struct win *owner, char *title, char *text, int yesno, int action) {
    int i, tw;
    struct win *w;
    tw = ui_len(text) * 6 + 40;
    if (tw < 260) tw = 260;
    if (tw > 560) tw = 560;
    i = win_open(K_MSG, title, tw, 120);
    if (i < 0) return;
    w = wins + i;
    w->resizable = 0;
    w->x = (SW - tw) / 2; w->y = (SH - 120) / 2 - 40;
    w->owner = owner ? win_index(owner) : 0 - 1;
    w->action = action;
    ui_cpy(w->st, text);
    w->st[200] = yesno;
}

void inputbox(struct win *owner, char *title, char *prompt, char *initial, int action) {
    int i;
    struct win *w;
    i = win_open(K_INPUT, title, 380, 130);
    if (i < 0) return;
    w = wins + i;
    w->resizable = 0;
    w->x = (SW - 380) / 2; w->y = (SH - 130) / 2 - 40;
    w->owner = owner ? win_index(owner) : 0 - 1;
    w->action = action;
    ui_cpy(w->st, prompt);
    ui_cpy(dtext, initial);
    dcaret = ui_len(dtext);
}

static void answer(struct win *d, int yes) {
    int owner, action;
    struct win *o;
    owner = d->owner; action = d->action;
    win_close(win_index(d));
    if (action == A_RUN) { if (yes && dtext[0]) run_command(dtext); return; }
    if (owner < 0) return;
    o = wins + owner;
    if (!o->used) return;
    win_raise(owner);
    if (o->kind == K_FILES) files_answer(o, action, yes);
    else if (o->kind == K_NOTE) note_answer(o, action, yes);
    else if (o->kind == K_TASKS) tasks_answer(o, action, yes);
}

static void dialog_draw(struct win *w, int ox, int oy, int cw, int ch) {
    int bx;
    d_rect(ox, oy, cw, ch, C_FACE);
    if (w->kind == K_MSG) {
        gui_disc(ox + 30, oy + 30, 14, w->st[200] ? C_TITLE : 0xd02020);
        gui_text2(ox + 26, oy + 20, C_WHITE, 18, F_BOLD, 0, w->st[200] ? "?" : "i", 1);
        d_text(ox + 56, oy + 24, C_DARK, F_SANS, w->st);
        if (w->st[200]) {
            bx = cw / 2 - 84;
            ui_button(ox + bx, oy + ch - 34, 76, 24, "Yes", 0, 1);
            ui_button(ox + bx + 92, oy + ch - 34, 76, 24, "No", 0, 0);
        } else ui_button(ox + cw / 2 - 38, oy + ch - 34, 76, 24, "OK", 0, 1);
    } else {
        d_text(ox + 12, oy + 12, C_DARK, F_SANS, w->st);
        ui_field(ox + 12, oy + 30, cw - 24, dtext, dcaret, 1);
        ui_button(ox + cw - 176, oy + ch - 34, 76, 24, "OK", 0, 1);
        ui_button(ox + cw - 88, oy + ch - 34, 76, 24, "Cancel", 0, 0);
    }
}

static void dialog_mouse(struct win *w, int x, int y, int cw, int ch) {
    int bx;
    if (w->kind == K_MSG) {
        if (w->st[200]) {
            bx = cw / 2 - 84;
            if (ui_hit(x, y, bx, ch - 34, 76, 24)) answer(w, 1);
            else if (ui_hit(x, y, bx + 92, ch - 34, 76, 24)) answer(w, 0);
        } else if (ui_hit(x, y, cw / 2 - 38, ch - 34, 76, 24)) answer(w, 1);
    } else {
        if (ui_hit(x, y, cw - 176, ch - 34, 76, 24)) answer(w, 1);
        else if (ui_hit(x, y, cw - 88, ch - 34, 76, 24)) answer(w, 0);
        else if (ui_hit(x, y, 12, 30, cw - 24, 22)) { dcaret = (x - 16) / 7; if (dcaret > ui_len(dtext)) dcaret = ui_len(dtext); if (dcaret < 0) dcaret = 0; }
    }
    dirty = 1;
}

static void dialog_key(struct win *w, int code, int ch, int mods) {
    if (code == KEY_ESC) { answer(w, 0); return; }
    if (code == KEY_ENTER) { answer(w, 1); return; }
    if (w->kind == K_MSG) {
        if (w->st[200] && (ch == 'y' || ch == 'Y')) answer(w, 1);
        else if (w->st[200] && (ch == 'n' || ch == 'N')) answer(w, 0);
        return;
    }
    ui_field_key(dtext, 120, &dcaret, code, ch, mods);
    dirty = 1;
}

void run_command(char *cmd) { term_open(cmd); }

// ---- per-kind dispatch -------------------------------------------------------------

static void app_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    int k;
    k = w->kind;
    if (k == K_TERM) term_draw(w, ox, oy, cw, ch, focused);
    else if (k == K_FILES) files_draw(w, ox, oy, cw, ch, focused);
    else if (k == K_TASKS) tasks_draw(w, ox, oy, cw, ch, focused);
    else if (k == K_NOTE) note_draw(w, ox, oy, cw, ch, focused);
    else if (k == K_CALC) calc_draw(w, ox, oy, cw, ch, focused);
    else if (k == K_MSG || k == K_INPUT) dialog_draw(w, ox, oy, cw, ch);
    else about_draw(w, ox, oy, cw, ch, focused);
}

static void app_mouse(struct win *w, int e, int x, int y) {
    int k, cw, ch;
    k = w->kind; cw = client_w(w); ch = client_h(w);
    if (k == K_FILES) files_mouse(w, e, x, y, cw, ch);
    else if (k == K_TASKS) tasks_mouse(w, e, x, y, cw, ch);
    else if (k == K_NOTE) note_mouse(w, e, x, y, cw, ch);
    else if (k == K_CALC) calc_mouse(w, e, x, y, cw, ch);
    else if ((k == K_MSG || k == K_INPUT) && e == E_DOWN) dialog_mouse(w, x, y, cw, ch);
}

static void app_key(struct win *w, int code, int ch, int mods) {
    int k;
    k = w->kind;
    if (k == K_TERM) term_key(w, code, ch, mods);
    else if (k == K_FILES) files_key(w, code, ch, mods);
    else if (k == K_TASKS) tasks_key(w, code, ch, mods);
    else if (k == K_NOTE) note_key(w, code, ch, mods);
    else if (k == K_CALC) calc_key(w, code, ch, mods);
    else if (k == K_MSG || k == K_INPUT) dialog_key(w, code, ch, mods);
    else if (code == KEY_ENTER || code == KEY_ESC) win_close(win_index(w));
}

static void app_command(struct win *w, int menu, int item) {
    if (w->kind == K_FILES) files_command(w, menu, item);
    else if (w->kind == K_NOTE) note_command(w, menu, item);
}

// ---- drawing the scene -----------------------------------------------------------

static int mix(int a, int b, int t, int n) {
    int r, g, bl;
    r = ((a >> 16) & 255) + ((((b >> 16) & 255) - ((a >> 16) & 255)) * t) / n;
    g = ((a >> 8) & 255) + ((((b >> 8) & 255) - ((a >> 8) & 255)) * t) / n;
    bl = (a & 255) + (((b & 255) - (a & 255)) * t) / n;
    return (r << 16) | (g << 8) | bl;
}

static void gradient(int x, int y, int w, int h, int a, int b) {
    int i, n, bw;
    n = 16; bw = (w + n - 1) / n;
    i = 0;
    while (i < n) {
        if (i * bw < w) gui_rect(x + i * bw, y, (i + 1) * bw > w ? w - i * bw : bw, h, mix(a, b, i, n - 1));
        ++i;
    }
}

static void cap_button(int x, int y, int what) {
    gui_rect(x, y, 16, 14, C_FACE);
    d_bevel(x, y, 16, 14, B_RAISED);
    if (what == 0) {                               // close: an X
        gui_line(x + 4, y + 3, x + 10, y + 9, C_DARK); gui_line(x + 5, y + 3, x + 11, y + 9, C_DARK);
        gui_line(x + 10, y + 3, x + 4, y + 9, C_DARK); gui_line(x + 11, y + 3, x + 5, y + 9, C_DARK);
    } else if (what == 1) {                        // maximise
        gui_recto(x + 3, y + 2, 9, 9, C_DARK); gui_rect(x + 3, y + 3, 9, 1, C_DARK);
    } else if (what == 2) {                        // restore
        gui_recto(x + 5, y + 2, 7, 6, C_DARK); gui_rect(x + 5, y + 3, 7, 1, C_DARK);
        gui_rect(x + 3, y + 5, 7, 6, C_FACE);
        gui_recto(x + 3, y + 5, 7, 6, C_DARK); gui_rect(x + 3, y + 6, 7, 1, C_DARK);
    } else gui_rect(x + 4, y + 9, 6, 2, C_DARK);  // minimise
}

static int is_dialog(struct win *w) { return w->kind == K_MSG || w->kind == K_INPUT; }

static void draw_menubar(struct win *w, int focused) {
    int k, x, y, lw;
    y = w->y + BORDER + TITLE_H + 1;
    gui_rect(client_x(w), y, client_w(w), MENU_H, C_FACE);
    x = client_x(w) + 4;
    k = 0;
    while (k < w->nmenus) {
        lw = ui_len(w->menu_titles[k]) * 7 + 12;
        if (menu_win == win_index(w) + 1 && menu_idx == k) gui_rect(x, y + 1, lw, MENU_H - 2, C_SEL);
        d_text(x + 6, y + 4, menu_win == win_index(w) + 1 && menu_idx == k ? C_WHITE : C_DARK, F_SANS, w->menu_titles[k]);
        x = x + lw;
        ++k;
    }
}

// Where menu k of window w sits on the bar.
static int menu_x(struct win *w, int k) {
    int x, j;
    x = client_x(w) + 4;
    j = 0;
    while (j < k) { x = x + ui_len(w->menu_titles[j]) * 7 + 12; ++j; }
    return x;
}

static int count_items(char **items) { int n; n = 0; while (items[n]) ++n; return n; }

static void draw_dropdown() {
    struct win *w;
    char **items;
    int n, x, y, k, iy, mw, mh;
    w = wins + menu_win - 1;
    items = w->menu_items[menu_idx];
    n = count_items(items);
    x = menu_x(w, menu_idx);
    y = w->y + BORDER + TITLE_H + 1 + MENU_H;
    mw = 160; mh = 6;
    k = 0; while (k < n) { if (items[k][0] == '-') mh = mh + 8; else mh = mh + 20; ++k; }
    gui_rect(x, y, mw, mh, C_FACE);
    d_bevel(x, y, mw, mh, B_RAISED);
    iy = y + 3;
    k = 0;
    while (k < n) {
        if (items[k][0] == '-') { gui_rect(x + 3, iy + 3, mw - 6, 1, C_SHADOW); gui_rect(x + 3, iy + 4, mw - 6, 1, C_LIGHT); iy = iy + 8; }
        else {
            if (k == menu_hot) gui_rect(x + 3, iy, mw - 6, 20, C_SEL);
            d_text(x + 20, iy + 4, k == menu_hot ? C_WHITE : C_DARK, F_SANS, items[k]);
            iy = iy + 20;
        }
        ++k;
    }
}

// Which item of the open drop-down is at (x, y), or -1.
static int dropdown_hit(int px, int py) {
    struct win *w;
    char **items;
    int n, x, y, k, iy;
    w = wins + menu_win - 1;
    items = w->menu_items[menu_idx];
    n = count_items(items);
    x = menu_x(w, menu_idx);
    y = w->y + BORDER + TITLE_H + 1 + MENU_H;
    if (px < x || px >= x + 160) return 0 - 1;
    iy = y + 3;
    k = 0;
    while (k < n) {
        if (items[k][0] == '-') iy = iy + 8;
        else { if (py >= iy && py < iy + 20) return k; iy = iy + 20; }
        ++k;
    }
    return 0 - 1;
}

static void draw_window(int i, int focused) {
    struct win *w;
    int tx, bx, by;
    w = wins + i;
    gui_rect(w->x, w->y, w->w, w->h, C_FACE);
    d_bevel(w->x, w->y, w->w, w->h, B_RAISED);
    if (focused) gradient(w->x + BORDER, w->y + BORDER, w->w - 2 * BORDER, TITLE_H, C_TITLE, C_TITLE2);
    else gradient(w->x + BORDER, w->y + BORDER, w->w - 2 * BORDER, TITLE_H, C_ITITLE, C_ITITLE2);
    tx = w->x + BORDER + 3;
    if (!is_dialog(w)) { d_icon(w->kind, tx, w->y + BORDER + 1); tx = tx + 20; }
    gui_clip(w->x + BORDER, w->y + BORDER, w->w - 2 * BORDER - 56, TITLE_H);
    d_text(tx, w->y + BORDER + 3, focused ? C_LIGHT : C_FACE, F_BOLD, w->title);
    gui_noclip();
    by = w->y + BORDER + 2;
    bx = w->x + w->w - BORDER - 2 - 16;
    cap_button(bx, by, 0);
    if (!is_dialog(w)) {
        cap_button(bx - 18, by, w->max ? 2 : 1);
        cap_button(bx - 34, by, 3);
    }
    if (w->nmenus) draw_menubar(w, focused);
    gui_clip(client_x(w), client_y(w), client_w(w), client_h(w));
    app_draw(w, client_x(w), client_y(w), client_w(w), client_h(w), focused);
    gui_noclip();
    if (w->resizable && !w->max) {                 // the size grip
        bx = w->x + w->w - BORDER - 12; by = w->y + w->h - BORDER - 12;
        gui_line(bx + 3, by + 11, bx + 11, by + 3, C_LIGHT); gui_line(bx + 4, by + 11, bx + 11, by + 4, C_SHADOW);
        gui_line(bx + 7, by + 11, bx + 11, by + 7, C_LIGHT); gui_line(bx + 8, by + 11, bx + 11, by + 8, C_SHADOW);
    }
}

static void icon(int x, int y, char *label, int kind) {
    int lw;
    if (kind == 0) {                               // My Computer
        gui_rect(x + 10, y, 28, 20, C_FACE); d_bevel(x + 10, y, 28, 20, B_RAISED);
        gui_rect(x + 13, y + 3, 22, 14, 0x008080);
        gui_rect(x + 8, y + 22, 32, 8, C_FACE); d_bevel(x + 8, y + 22, 32, 8, B_RAISED);
    } else if (kind == K_TERM) {
        gui_rect(x + 8, y, 32, 26, C_FACE); d_bevel(x + 8, y, 32, 26, B_RAISED);
        gui_rect(x + 11, y + 3, 26, 18, C_DARK);
        gui_text2(x + 13, y + 5, 0xc0c0c0, 10, F_MONO, 0, ">_", 2);
        gui_rect(x + 12, y + 28, 24, 3, C_FACE);
    } else {
        gui_rect(x + 14, y + 4, 20, 24, C_WHITE); gui_recto(x + 14, y + 4, 20, 24, C_SHADOW);
        if (kind == K_NOTE) { gui_rect(x + 17, y + 10, 14, 1, 0x4060c0); gui_rect(x + 17, y + 14, 14, 1, 0x4060c0); gui_rect(x + 17, y + 18, 10, 1, 0x4060c0); }
        else d_icon(kind, x + 16, y + 8);
    }
    lw = ui_len(label) * 6;
    d_text(x + 24 - lw / 2, y + 36, C_LIGHT, F_SANS, label);
}

static char *icon_labels[5];
static int icon_kinds[5];
enum { N_ICONS = 5 };

// The Start menu, NT 4 style: a banner down the left, then the tools.
static char *start_items[12];
static int start_kinds[12];
static int n_start;

static int start_height() {
    int k, h;
    h = 6; k = 0;
    while (k < n_start) { h = h + (start_items[k][0] == '-' ? 8 : 26); ++k; }
    return h;
}

static void draw_start() {
    int mx, my, mw, mh, k, iy;
    char c[2], *banner;
    mw = 200; mh = start_height();
    mx = 2; my = SH - TASK_H - mh;
    gui_rect(mx, my, mw, mh, C_FACE);
    d_bevel(mx, my, mw, mh, B_RAISED);
    gui_rect(mx + 3, my + 3, 22, mh - 6, C_SHADOW);
    c[1] = 0;
    banner = "C4IX";
    k = 0;
    while (k < 4) { c[0] = banner[k]; gui_text2(mx + 8, my + mh - 30 - (3 - k) * 18, C_WHITE, 16, F_BOLD, 0, c, 1); ++k; }
    iy = my + 3;
    k = 0;
    while (k < n_start) {
        if (start_items[k][0] == '-') {
            gui_rect(mx + 28, iy + 3, mw - 32, 1, C_SHADOW); gui_rect(mx + 28, iy + 4, mw - 32, 1, C_LIGHT);
            iy = iy + 8;
        } else {
            if (k == start_hot) gui_rect(mx + 26, iy, mw - 29, 26, C_SEL);
            if (start_kinds[k] > 0) d_icon(start_kinds[k], mx + 32, iy + 5);
            d_text(mx + 56, iy + 7, k == start_hot ? C_WHITE : C_DARK, F_SANS, start_items[k]);
            iy = iy + 26;
        }
        ++k;
    }
}

static int start_hit(int x, int y) {
    int mh, my, k, iy;
    mh = start_height();
    my = SH - TASK_H - mh;
    if (x < 28 || x >= 202 || y < my || y >= my + mh) return 0 - 1;
    iy = my + 3;
    k = 0;
    while (k < n_start) {
        if (start_items[k][0] == '-') iy = iy + 8;
        else { if (y >= iy && y < iy + 26) return k; iy = iy + 26; }
        ++k;
    }
    return 0 - 1;
}

static int n_buttons() { int k, n; n = 0; k = 0; while (k < MAXW) { if (wins[k].used && !is_dialog(wins + k)) ++n; ++k; } return n; }
static int button_w() { int n, w; n = n_buttons(); if (!n) return 150; w = (SW - 64 - 84) / n - 4; return w > 150 ? 150 : w; }

static void draw_taskbar() {
    int k, i, bx, bw, secs, h, m, f;
    char *p;
    struct win *w;
    gui_rect(0, SH - TASK_H, SW, TASK_H, C_FACE);
    gui_rect(0, SH - TASK_H + 1, SW, 1, C_LIGHT);
    gui_rect(2, SH - TASK_H + 4, 56, 22, C_FACE);
    d_bevel(2, SH - TASK_H + 4, 56, 22, start_open ? B_PRESSED : B_RAISED);
    gui_rect(8, SH - TASK_H + 9, 5, 5, 0xff2020); gui_rect(14, SH - TASK_H + 9, 5, 5, 0x20c020);
    gui_rect(8, SH - TASK_H + 15, 5, 5, 0x2040ff); gui_rect(14, SH - TASK_H + 15, 5, 5, 0xffd020);
    d_text(23, SH - TASK_H + 9, C_DARK, F_BOLD, "Start");
    f = win_focused();
    bw = button_w();
    k = 0; i = 0;
    while (i < MAXW) {
        w = wins + i;
        if (w->used && !is_dialog(w)) {
            bx = 64 + k * (bw + 4);
            gui_rect(bx, SH - TASK_H + 4, bw, 22, i == f ? 0xe0e0e0 : C_FACE);
            d_bevel(bx, SH - TASK_H + 4, bw, 22, i == f ? B_PRESSED : B_RAISED);
            d_icon(w->kind, bx + 4, SH - TASK_H + 7);
            gui_clip(bx + 22, SH - TASK_H + 4, bw - 26, 22);
            d_text(bx + 24, SH - TASK_H + 9, C_DARK, i == f ? F_BOLD : F_SANS, w->title);
            gui_noclip();
            ++k;
        }
        ++i;
    }
    gui_rect(SW - 76, SH - TASK_H + 4, 72, 22, C_FACE);
    d_bevel(SW - 76, SH - TASK_H + 4, 72, 22, B_SUNKEN);
    secs = gui_clock();
    h = secs / 3600; m = (secs / 60) % 60;
    p = tbuf;
    if (h % 12 == 0) { *p = '1'; ++p; *p = '2'; ++p; }
    else { if (h % 12 >= 10) { *p = '1'; ++p; } *p = '0' + (h % 12) % 10; ++p; }
    *p = ':'; ++p; *p = '0' + m / 10; ++p; *p = '0' + m % 10; ++p;
    *p = ' '; ++p; *p = h < 12 ? 'A' : 'P'; ++p; *p = 'M'; ++p; *p = 0;
    d_text(SW - 64, SH - TASK_H + 9, C_DARK, F_SANS, tbuf);
}

static void redraw() {
    int k, f;
    gui_clear(C_DESK);
    k = 0;
    while (k < N_ICONS) { icon(18, 14 + k * 72, icon_labels[k], icon_kinds[k]); ++k; }
    f = win_focused();
    k = 0;
    while (k < nz) {
        if (!wins[order[k]].min) draw_window(order[k], order[k] == f);
        ++k;
    }
    if (menu_win) draw_dropdown();
    draw_taskbar();
    if (start_open) draw_start();
    gui_show();
    dirty = 0;
}

// ---- actions --------------------------------------------------------------------------

static void toggle_max(struct win *w) {
    if (!w->max) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->x = 0; w->y = 0; w->w = SW; w->h = SH - TASK_H; w->max = 1;
    } else {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh; w->max = 0;
    }
    dirty = 1;
}

static void launch(int kind) {
    if (kind == K_TERM) term_open(0);
    else if (kind == K_FILES) files_open("/");
    else if (kind == K_TASKS) tasks_open();
    else if (kind == K_NOTE) note_open(0);
    else if (kind == K_CALC) calc_open();
    else if (kind == K_ABOUT) about_open();
}

static void start_pick(int k) {
    start_open = 0; dirty = 1;
    if (k < 0) return;
    if (start_kinds[k] > 0) launch(start_kinds[k]);
    else if (start_kinds[k] == 0 - 1) inputbox(0, "Run", "Type the name of a program, and C4IX will run it:", "", A_RUN);
    else if (start_kinds[k] == 0 - 2) running = 0;
}

static void mouse_down(int x, int y, int button) {
    int k, i, bx, by, bw, t, cx, cy, m;
    struct win *w;
    if (start_open) {
        k = start_hit(x, y);
        start_open = 0; dirty = 1;
        if (k >= 0) start_pick(k);
        if (!ui_hit(x, y, 2, SH - TASK_H + 4, 56, 22)) return;
        return;
    }
    if (menu_win) {
        w = wins + menu_win - 1;
        k = dropdown_hit(x, y);
        i = menu_idx;
        menu_win = 0; dirty = 1;
        if (k >= 0) { app_command(w, i, k); return; }
        // a click on another title of the same bar opens that one instead
        if (y >= w->y + BORDER + TITLE_H + 1 && y < w->y + BORDER + TITLE_H + 1 + MENU_H) {
            m = 0;
            while (m < w->nmenus) {
                if (m != i && x >= menu_x(w, m) && x < menu_x(w, m) + ui_len(w->menu_titles[m]) * 7 + 12) {
                    menu_win = win_index(w) + 1; menu_idx = m; menu_hot = 0 - 1; return;
                }
                ++m;
            }
        }
        return;
    }
    if (y >= SH - TASK_H) {
        if (modal() >= 0) return;
        if (ui_hit(x, y, 2, SH - TASK_H + 4, 56, 22)) { start_open = 1; start_hot = 0 - 1; dirty = 1; return; }
        bw = button_w();
        k = 0; i = 0;
        while (i < MAXW) {
            w = wins + i;
            if (w->used && !is_dialog(w)) {
                if (ui_hit(x, y, 64 + k * (bw + 4), SH - TASK_H + 4, bw, 22)) {
                    if (i == win_focused() && !w->min) w->min = 1;
                    else win_raise(i);
                    dirty = 1;
                    return;
                }
                ++k;
            }
            ++i;
        }
        return;
    }
    // windows, topmost first; a dialog keeps the input to itself
    k = nz - 1;
    while (k >= 0) {
        i = order[k];
        w = wins + i;
        if (!w->min && ui_hit(x, y, w->x, w->y, w->w, w->h)) {
            if (modal() >= 0 && modal() != i) return;
            win_raise(i);
            by = w->y + BORDER + 2;
            bx = w->x + w->w - BORDER - 2 - 16;
            if (ui_hit(x, y, bx, by, 16, 14)) {
                if (is_dialog(w)) answer(w, 0); else win_close(i);
                return;
            }
            if (!is_dialog(w) && ui_hit(x, y, bx - 18, by, 16, 14)) { toggle_max(w); return; }
            if (!is_dialog(w) && ui_hit(x, y, bx - 34, by, 16, 14)) { w->min = 1; dirty = 1; return; }
            if (y < w->y + BORDER + TITLE_H) {
                t = now_ms();
                if (!is_dialog(w) && down_win == i + 1 && t - down_t < 500 && y < w->y + BORDER + TITLE_H) { toggle_max(w); down_win = 0; return; }
                down_win = i + 1; down_t = t;
                if (!w->max) { drag = i + 1; drag_dx = x - w->x; drag_dy = y - w->y; }
                return;
            }
            if (w->resizable && !w->max && x >= w->x + w->w - 16 && y >= w->y + w->h - 16) {
                sizing = i + 1; size_w0 = w->w; size_h0 = w->h; size_x0 = x; size_y0 = y;
                return;
            }
            if (w->nmenus && y < client_y(w)) {
                m = 0;
                while (m < w->nmenus) {
                    if (x >= menu_x(w, m) && x < menu_x(w, m) + ui_len(w->menu_titles[m]) * 7 + 12) {
                        menu_win = i + 1; menu_idx = m; menu_hot = 0 - 1; dirty = 1; return;
                    }
                    ++m;
                }
                return;
            }
            cx = x - client_x(w); cy = y - client_y(w);
            if (cx >= 0 && cy >= 0 && cx < client_w(w) && cy < client_h(w)) {
                t = now_ms();
                app_mouse(w, E_DOWN, cx, cy);
                if (w->used && down_win == i + 1 && t - down_t < 500 && x - down_x < 5 && down_x - x < 5 && y - down_y < 5 && down_y - y < 5) {
                    app_mouse(w, E_DBL, cx, cy);
                    down_win = 0;
                } else { down_win = i + 1; down_t = t; down_x = x; down_y = y; }
                dirty = 1;
            }
            return;
        }
        --k;
    }
    if (modal() >= 0) return;
    // the desktop icons: a double click opens
    t = now_ms();
    k = 0 - 1;
    i = 0;
    while (i < N_ICONS) { if (ui_hit(x, y, 14, 12 + i * 72, 56, 64)) k = i; ++i; }
    if (k >= 0 && k == icon_k && t - icon_t < 500) {
        if (icon_kinds[k] == 0) files_open("/"); else launch(icon_kinds[k]);
        icon_k = 0 - 1;
    } else { icon_k = k; icon_t = t; }
}

static void mouse_move(int x, int y) {
    struct win *w;
    int k;
    if (drag) {
        w = wins + drag - 1;
        w->x = x - drag_dx; w->y = y - drag_dy;
        if (w->y < 0) w->y = 0;
        if (w->y > SH - TASK_H - TITLE_H) w->y = SH - TASK_H - TITLE_H;
        if (w->x < 60 - w->w) w->x = 60 - w->w;
        if (w->x > SW - 60) w->x = SW - 60;
        dirty = 1;
        return;
    }
    if (sizing) {
        w = wins + sizing - 1;
        w->w = size_w0 + x - size_x0; w->h = size_h0 + y - size_y0;
        if (w->w < 220) w->w = 220;
        if (w->h < 140) w->h = 140;
        dirty = 1;
        return;
    }
    if (start_open) { k = start_hit(x, y); if (k != start_hot) { start_hot = k; dirty = 1; } return; }
    if (menu_win) { k = dropdown_hit(x, y); if (k != menu_hot) { menu_hot = k; dirty = 1; } return; }
    k = win_focused();
    if (k >= 0) {
        w = wins + k;
        if (ui_hit(x, y, client_x(w), client_y(w), client_w(w), client_h(w)))
            app_mouse(w, E_MOVE, x - client_x(w), y - client_y(w));
    }
}

static void mouse_up(int x, int y) {
    struct win *w;
    int k;
    drag = 0; sizing = 0;
    k = win_focused();
    if (k >= 0) { w = wins + k; app_mouse(w, E_UP, x - client_x(w), y - client_y(w)); }
}

static void wheel(int x, int y, int dy) {
    int k, i;
    struct win *w;
    k = nz - 1;
    while (k >= 0) {
        i = order[k]; w = wins + i;
        if (!w->min && ui_hit(x, y, client_x(w), client_y(w), client_w(w), client_h(w))) {
            app_mouse(w, E_WHEEL, x - client_x(w), dy);
            dirty = 1;
            return;
        }
        --k;
    }
}

static void key_down(int code, int ch, int mods) {
    int f;
    // typing between two clicks makes them two clicks, not a double click
    down_win = 0; icon_k = 0 - 1;
    // Ctrl+Shift+Esc: Task Manager, from anywhere
    if (code == KEY_ESC && (mods & M_CTRL) && (mods & M_SHIFT)) { tasks_open(); return; }
    if (start_open && code == KEY_ESC) { start_open = 0; dirty = 1; return; }
    if (menu_win && code == KEY_ESC) { menu_win = 0; dirty = 1; return; }
    f = win_focused();
    if (f < 0) return;
    app_key(wins + f, code, ch, mods);
    dirty = 1;
}

static void shutdown_screen() {
    gui_clear(C_DARK);
    gui_text2(SW / 2 - 200, SH / 2 - 20, 0xff8c1a, 22, F_BOLD, 0, "It's now safe to turn off your computer.", 0 - 1);
    gui_show();
}

int main(int argc, char **argv) {
    int *region, k, busy, secs, clock_min, last_tick, last_draw, input;

    if (!gui_present()) { printf("desktop: not fitted\n"); return 0; }
    region = ualloc(65536);
    if (!gui_attach(region, 65536, SW, SH)) { printf("desktop: the display refused the ring\n"); return 1; }
    gui_events(GUI_M_MOVE | GUI_M_BUTTONS | GUI_M_KEYS | GUI_M_WHEEL);
    wins = (struct win *)ualloc(MAXW * sizeof(struct win));
    memset(wins, 0, MAXW * sizeof(struct win));
    ev = ualloc(4 * sizeof(int));

    icon_labels[0] = "My Computer"; icon_kinds[0] = 0;
    icon_labels[1] = "Command Prompt"; icon_kinds[1] = K_TERM;
    icon_labels[2] = "Notepad"; icon_kinds[2] = K_NOTE;
    icon_labels[3] = "Calculator"; icon_kinds[3] = K_CALC;
    icon_labels[4] = "Task Manager"; icon_kinds[4] = K_TASKS;
    k = 0;
    start_items[k] = "Command Prompt"; start_kinds[k] = K_TERM; ++k;
    start_items[k] = "Explorer"; start_kinds[k] = K_FILES; ++k;
    start_items[k] = "Notepad"; start_kinds[k] = K_NOTE; ++k;
    start_items[k] = "Calculator"; start_kinds[k] = K_CALC; ++k;
    start_items[k] = "Task Manager"; start_kinds[k] = K_TASKS; ++k;
    start_items[k] = "About C4IX"; start_kinds[k] = K_ABOUT; ++k;
    start_items[k] = "-"; start_kinds[k] = 0; ++k;
    start_items[k] = "Run..."; start_kinds[k] = 0 - 1; ++k;
    start_items[k] = "-"; start_kinds[k] = 0; ++k;
    start_items[k] = "Shut Down..."; start_kinds[k] = 0 - 2; ++k;
    n_start = k;
    nz = 0; icon_k = 0 - 1;
    printf("desktop: running on the display. Start > Shut Down to come back here.\n");

    term_open(0);
    running = 1; dirty = 1;
    clock_min = 0 - 1; last_tick = 0; last_draw = 0;
    while (running) {
        busy = 0; input = 0;
        while (gui_poll(ev)) {
            busy = 1; input = 1;
            if (ev[0] == GUI_EV_DOWN) mouse_down(ev[1], ev[2], ev[3]);
            else if (ev[0] == GUI_EV_UP) mouse_up(ev[1], ev[2]);
            else if (ev[0] == GUI_EV_MOVE) mouse_move(ev[1], ev[2]);
            else if (ev[0] == GUI_EV_KEYDOWN) key_down(ev[1], ev[2], ev[3]);
            else if (ev[0] == GUI_EV_WHEEL) wheel(ev[1], ev[2], ev[3]);
        }
        k = 0;
        while (k < MAXW) {
            if (wins[k].used && wins[k].kind == K_TERM) term_tick(wins + k);
            ++k;
        }
        // a tick a second for the windows that show live numbers
        if (now_ms() - last_tick >= 1000) {
            last_tick = now_ms();
            k = 0;
            while (k < MAXW) { if (wins[k].used && wins[k].kind == K_TASKS) tasks_tick(wins + k); ++k; }
        }
        secs = gui_clock() / 60;
        if (secs != clock_min) { clock_min = secs; dirty = 1; }
        // A redraw is the whole scene. Input gets one at once; a program
        // pouring out text (raycast, top) gets at most one every 25 ms --
        // it can print far faster than a person can read. Machine time
        // (__time), the clock the desktop sleeps on: host time runs at a
        // different rate whenever the machine is not paced to it.
        if (dirty && (input || __time() - last_draw >= 25)) { redraw(); last_draw = __time(); busy = 1; }
        if (!busy) umsleep(dirty ? 5 : 15);
    }

    k = 0;
    while (k < MAXW) { if (wins[k].used) win_close(k); ++k; }
    shutdown_screen();
    gui_detach();
    printf("desktop: shut down\n");
    return 0;
}
