// control.c -- Control Panel (docs/grin-plan.md 4): Display (640x480 or
// 800x600, applied at once), Colors (the kit's schemes), Desktop (a
// pattern, and the screen saver) and Sound (the startup chime, with a
// Test). Every choice is kept in GRIN.INI.

#include "shell.h"

// ---- the window: an item for each ---------------------------------------------------

enum { N_ITEMS = 4, ITEM_Y = 12 };
static char *item_labels[N_ITEMS] = { "Colors", "Desktop", "Display", "Sound" };
static int item_pics[N_ITEMS] = { PIC_COLORS, PIC_DESKTOP, PIC_DISPLAY, PIC_SOUND };
static char *settings_items[7] = { "Colors...", "Desktop...", "Display...", "Sound...", "-", "Exit", 0 };
static struct wk_menu menus[1];

struct control { int sel; };

static void open_colors();
static void open_desktop();
static void open_display();
static void open_sound();

static int item_cx(int k) { return 12 + ITEM_W / 2 + k * (ITEM_W + 8); }

static void cp_draw(int w, int cw, int ch, int focused) {
    struct control *c;
    int k;
    c = (struct control *)wk_state(w);
    d_rect(0, 0, cw, ch, C_WHITE);
    k = 0;
    while (k < N_ITEMS) { grin_item(item_cx(k), ITEM_Y, item_pics[k], item_labels[k], focused && c->sel == k + 1); ++k; }
}

static void cp_choose(int k) {
    if (k == 0) open_colors();
    else if (k == 1) open_desktop();
    else if (k == 2) open_display();
    else if (k == 3) open_sound();
}

static void cp_mouse(int w, int ev, int x, int y, int b) {
    struct control *c;
    int k;
    c = (struct control *)wk_state(w);
    if (ev != E_DOWN && ev != E_DBL) return;
    c->sel = 0;
    k = 0;
    while (k < N_ITEMS) {
        if (grin_item_hit(x, y, item_cx(k), ITEM_Y)) { c->sel = k + 1; if (ev == E_DBL) cp_choose(k); return; }
        ++k;
    }
}

static void cp_key(int w, int down, int code, int ch, int mods) {
    struct control *c;
    c = (struct control *)wk_state(w);
    if (!down) return;
    if (code == KEY_RIGHT) c->sel = c->sel >= N_ITEMS ? N_ITEMS : c->sel + 1;
    else if (code == KEY_LEFT) c->sel = c->sel <= 1 ? 1 : c->sel - 1;
    else if (code == KEY_ENTER && c->sel) cp_choose(c->sel - 1);
}

static void cp_command(int w, int id) {
    if (id >= WK_CMD(0, 0) && id < WK_CMD(0, N_ITEMS)) cp_choose(id - WK_CMD(0, 0));
    else if (id == WK_CMD(0, N_ITEMS + 1)) wk_close(w);
}

static struct wk_class control_class = { .kind = ICON_SETTINGS, .draw = cp_draw, .mouse = cp_mouse, .key = cp_key,
                                         .command = cp_command };

int control_open() {
    int w;
    if ((w = wk_find(&control_class))) { wk_raise(w); return w; }
    control_class.size = sizeof(struct control);
    menus[0].title = "Settings"; menus[0].items = settings_items;
    if ((w = wk_open(&control_class, "Control Panel", 2 * BORDER + 24 + N_ITEMS * (ITEM_W + 8),
                     2 * BORDER + TITLE_H + 1 + MENU_H + 84, WK_FIXED, 0)) < 0) return w;
    wk_menu(w, menus, 1);
    return w;
}

// ---- the dialogs: a list of choices, OK and Cancel -----------------------------------
//
// Each is a column of radio buttons (two for the Desktop's), with OK and
// Cancel under them; OK applies the choice and keeps it.

enum { RADIO_H = 20, OK_W = 76, BTN_H = 24 };

struct choice {
    char *buf; int max;               // struct grin_dlg's
    int what;                         // which dialog
    int a, b, wait;                   // the choices: a's column, b's, the saver's wait
};
enum { D_DISPLAY = 1, D_COLORS = 2, D_DESKTOP = 3, D_SOUND = 4 };
static char *sizes[2] = { "640 x 480", "800 x 600" };
static char *chimes[2] = { "No chime", "A chime when grin starts" };

static int col_n(int what, int col) {
    if (what == D_DISPLAY || what == D_SOUND) return 2;
    if (what == D_COLORS) return wk_scheme_count();
    return col ? saver_count() : pattern_count();
}
static char *col_label(struct choice *c, int col, int k) {
    if (c->what == D_DISPLAY) return sizes[k];
    if (c->what == D_SOUND) return chimes[k];
    if (c->what == D_COLORS) return wk_scheme_name(k);
    return col ? saver_name(k) : pattern_name(k);
}
static int ncols(int what) { return what == D_DESKTOP ? 2 : 1; }
static int col_x(int col) { return 20 + col * 170; }
static int rows_y() { return 34; }
static int buttons_y(int what) {
    int n, m;
    n = col_n(what, 0);
    if (ncols(what) > 1 && (m = col_n(what, 1)) > n) n = m;
    return rows_y() + n * RADIO_H + (what == D_DESKTOP || what == D_SOUND ? 40 : 16);
}

static void ch_draw(int w, int cw, int ch, int focused) {
    struct choice *c;
    int col, k, y;
    char num[24];
    c = (struct choice *)wk_state(w);
    d_rect(0, 0, cw, ch, C_FACE);
    col = 0;
    while (col < ncols(c->what)) {
        k = 0;
        y = rows_y();
        if (c->what == D_DISPLAY) ui_group(10, 8, cw - 20, buttons_y(c->what) - 18, "Screen size");
        else if (c->what == D_COLORS) ui_group(10, 8, cw - 20, buttons_y(c->what) - 18, "Color schemes");
        else if (c->what == D_SOUND) ui_group(10, 8, cw - 20, buttons_y(c->what) - 18, "Startup");
        else ui_group(col_x(col) - 10, 8, 160, buttons_y(c->what) - 18, col ? "Screen Saver" : "Pattern");
        while (k < col_n(c->what, col)) {
            ui_radio(col_x(col), y, col_label(c, col, k), k == (col ? c->b : c->a));
            y = y + RADIO_H;
            ++k;
        }
        ++col;
    }
    if (c->what == D_DESKTOP) {
        // the saver's wait, and a Test of it
        y = buttons_y(c->what) - 34;
        d_text(col_x(1), y + 4, C_DARK, F_SANS, "Delay:");
        ui_button(col_x(1) + 44, y, 20, 20, "-", 0, 0);
        d_text(col_x(1) + 72, y + 4, C_DARK, F_SANS, ui_num(num, c->wait));
        ui_button(col_x(1) + 92, y, 20, 20, "+", 0, 0);
        d_text(col_x(1) + 118, y + 4, C_DARK, F_SANS, "min");
        ui_button(col_x(0), y, 64, 20, "Test", 0, 0);
    }
    if (c->what == D_SOUND) ui_button(col_x(0), buttons_y(c->what) - 34, 64, 20, "Test", 0, 0);
    ui_button(cw / 2 - OK_W - 6, buttons_y(c->what), OK_W, BTN_H, "OK", 0, 1);
    ui_button(cw / 2 + 6, buttons_y(c->what), OK_W, BTN_H, "Cancel", 0, 0);
}

static void ch_ok(int w) {
    struct choice *c;
    c = (struct choice *)wk_state(w);
    // closed first: a new screen size lays out every window, this one too
    wk_close(w);
    if (c->what == D_DISPLAY) grin_set_screen(c->a ? 800 : 640, c->a ? 600 : 480);
    else if (c->what == D_COLORS) grin_set_scheme(c->a);
    else if (c->what == D_SOUND) { ini.chime = c->a; ini_save(); }
    else {
        ini.saver = c->b; ini.wait = c->wait;
        grin_set_pattern(c->a);
    }
}

static void ch_mouse(int w, int ev, int x, int y, int b) {
    struct choice *c;
    int cw, chh, col, k, by;
    c = (struct choice *)wk_state(w);
    if (ev != E_DOWN && ev != E_DBL) return;
    wk_size(w, &cw, &chh);
    by = buttons_y(c->what);
    if (ui_hit(x, y, cw / 2 - OK_W - 6, by, OK_W, BTN_H)) { ch_ok(w); return; }
    if (ui_hit(x, y, cw / 2 + 6, by, OK_W, BTN_H)) { wk_close(w); return; }
    if (c->what == D_DESKTOP && ev == E_DOWN) {
        if (ui_hit(x, y, col_x(1) + 44, by - 34, 20, 20) && c->wait > 1) { --c->wait; return; }
        if (ui_hit(x, y, col_x(1) + 92, by - 34, 20, 20) && c->wait < 60) { ++c->wait; return; }
        if (ui_hit(x, y, col_x(0), by - 34, 64, 20)) { grin_saver_test(c->b); return; }
    }
    if (c->what == D_SOUND && ev == E_DOWN && ui_hit(x, y, col_x(0), by - 34, 64, 20)) { grin_chime(); return; }
    col = 0;
    while (col < ncols(c->what)) {
        k = 0;
        while (k < col_n(c->what, col)) {
            // the whole row, its label included
            if (ui_hit(x, y, col_x(col) - 2, rows_y() + k * RADIO_H - 3, 150, RADIO_H)) {
                if (col) c->b = k; else c->a = k;
                if (ev == E_DBL) ch_ok(w);
                return;
            }
            ++k;
        }
        ++col;
    }
}

static void ch_key(int w, int down, int code, int ch, int mods) {
    struct choice *c;
    c = (struct choice *)wk_state(w);
    if (!down) return;
    if (code == KEY_ESC) { wk_close(w); return; }
    if (code == KEY_ENTER) { ch_ok(w); return; }
    if (code == KEY_UP && c->a > 0) --c->a;
    if (code == KEY_DOWN && c->a < col_n(c->what, 0) - 1) ++c->a;
}

static struct wk_class choice_class = { .kind = ICON_SETTINGS, .draw = ch_draw, .mouse = ch_mouse, .key = ch_key };

static struct choice *choice_open(int what, char *title, int cw) {
    int w;
    struct choice *c;
    if ((w = grin_dialog(&choice_class, sizeof(struct choice), title, cw + 2 * BORDER,
                         buttons_y(what) + BTN_H + 12 + 2 * BORDER + TITLE_H + 1)) < 0) return 0;
    c = (struct choice *)wk_state(w);
    c->what = what;
    return c;
}

static void open_display() {
    struct choice *c;
    if ((c = choice_open(D_DISPLAY, "Display", 260))) c->a = wk.sw >= 800 ? 1 : 0;
}
static void open_colors() {
    struct choice *c;
    if ((c = choice_open(D_COLORS, "Color", 260))) c->a = ini.scheme;
}
static void open_sound() {
    struct choice *c;
    if ((c = choice_open(D_SOUND, "Sound", 260))) c->a = ini.chime;
}
static void open_desktop() {
    struct choice *c;
    if ((c = choice_open(D_DESKTOP, "Desktop", 350))) { c->a = ini.pattern; c->b = ini.saver; c->wait = ini.wait; }
}
