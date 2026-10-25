// progman.c -- Program Manager, grin's shell window (docs/grin-plan.md 4).
//
// For now a placeholder: the Main group, holding Control Panel, and the
// Accessories group, holding Clock and Calculator (grin's first
// applications, G3), side by side; File > Run..., which starts an
// application by name; and File > Exit grin, which ends the session.
// Its groups and items, kept in PROGMAN.INI, come with G4. Closing
// Program Manager ends the session too, as it ended Windows: both ask
// first.

#include "shell.h"

struct progman { int sel; };

static char *file_items[4] = { "Run...", "-", "Exit grin...", 0 };
static char *help_items[2] = { "About grin...", 0 };
static struct wk_menu menus[2];
enum { A_EXIT = 1, A_ABOUT = 2, A_RUN = 3 };
enum { GROUP_X = 8, GROUP_Y = 8, ITEMS_Y = 32, N_ITEMS = 3 };

// the items, numbered 1 up: which group, the picture and label, and what
// a double click starts (0: Control Panel, built in)
static int item_group[N_ITEMS] = { 0, 1, 1 };
static int item_pic[N_ITEMS] = { PIC_CONTROL, PIC_CLOCK, PIC_CALC };
static char *item_label[N_ITEMS] = { "Control Panel", "Clock", "Calculator" };
static char *item_run[N_ITEMS] = { 0, "CLOCK", "CALC" };
static char *group_name[2] = { "Main", "Accessories" };
static char runbuf[WK_PATH];
static char about[160];

// group g's rectangle in a client area cw x ch: two side by side
static int group_w(int cw) { return (cw - 3 * GROUP_X) / 2; }
static int group_x(int g, int cw) { return GROUP_X + g * (group_w(cw) + GROUP_X); }
// item k's centre (k 1 up), across its group
static int item_cx(int k, int cw) {
    int i, n;
    n = 0; i = 0;
    while (i < k - 1) { if (item_group[i] == item_group[k - 1]) ++n; ++i; }
    return group_x(item_group[k - 1], cw) + 3 + ITEM_W / 2 + 4 + n * (ITEM_W + 4);
}

static void group_draw(int g, int cw, int ch) {
    int gx, gw, gh;
    gx = group_x(g, cw); gw = group_w(cw); gh = ch - 2 * GROUP_Y;
    d_rect(gx, GROUP_Y, gw, gh, C_DARK);
    d_rect(gx + 1, GROUP_Y + 1, gw - 2, gh - 2, C_FACE);
    d_rect(gx + 3, GROUP_Y + 3, gw - 6, 16, C_TITLE);
    d_text(gx + gw / 2 - ui_len(group_name[g]) * 7 / 2, GROUP_Y + 5, wk_ink(C_TITLE), F_BOLD, group_name[g]);
    d_rect(gx + 3, GROUP_Y + 20, gw - 6, gh - 23, C_WHITE);
}

static void pm_draw(int w, int cw, int ch, int focused) {
    struct progman *p;
    int k;
    p = (struct progman *)wk_state(w);
    d_rect(0, 0, cw, ch, C_WHITE);
    // the groups, open inside it (group windows of their own in G7)
    if (group_w(cw) < 100 || ch - 2 * GROUP_Y < 80) return;
    group_draw(0, cw, ch);
    group_draw(1, cw, ch);
    k = 1;
    while (k <= N_ITEMS) {
        grin_item(item_cx(k, cw), GROUP_Y + ITEMS_Y, item_pic[k - 1], item_label[k - 1], p->sel == k && focused);
        ++k;
    }
}

static void item_start(int k) {
    if (k < 1 || k > N_ITEMS) return;
    if (!item_run[k - 1]) control_open();
    else grin_run(item_run[k - 1]);
}

static void pm_mouse(int w, int ev, int x, int y, int b) {
    struct progman *p;
    int k, cw, ch;
    p = (struct progman *)wk_state(w);
    if (ev != E_DOWN && ev != E_DBL) return;
    wk_size(w, &cw, &ch);
    p->sel = 0;
    k = 1;
    while (k <= N_ITEMS && !p->sel) { if (grin_item_hit(x, y, item_cx(k, cw), GROUP_Y + ITEMS_Y)) p->sel = k; ++k; }
    wk_invalidate(w, 0, 0, 0, 0);
    if (p->sel && ev == E_DBL) item_start(p->sel);
}

static void pm_key(int w, int down, int code, int ch, int mods) {
    struct progman *p;
    p = (struct progman *)wk_state(w);
    if (!down) return;
    if (code == KEY_ENTER) item_start(p->sel);
    else if (code == KEY_RIGHT) p->sel = p->sel % N_ITEMS + 1;
    else if (code == KEY_LEFT) p->sel = p->sel > 1 ? p->sel - 1 : N_ITEMS;
    else if (code == KEY_HOME) p->sel = 1;
    else return;
    wk_invalidate(w, 0, 0, 0, 0);
}

static void ask_exit(int w) {
    wk_msgbox(w, A_EXIT, "Exit grin", "This will end your grin session.", WK_OKCANCEL);
}

static void pm_command(int w, int id) {
    char num[24];
    if (id == WK_CMD(0, 0)) {
        runbuf[0] = 0;
        wk_inputbox(w, A_RUN, "Run", "Command Line:", runbuf, WK_PATH);
    } else if (id == WK_CMD(0, 2)) ask_exit(w);
    else if (id == WK_CMD(1, 0)) {
        about[0] = 0;
        ui_ncat(about, "grin, a graphical shell for C4DOS in the manner of Windows 3.1. Applications running: ", 160);
        ui_ncat(about, ui_num(num, apps_count()), 160);
        ui_ncat(about, ". File > Exit grin returns to A>.", 160);
        wk_msgbox(w, A_ABOUT, "About Program Manager", about, WK_OK);
    }
}

static void pm_answer(int w, int dialog, int result) {
    if (dialog == A_EXIT && result) grin_quit();
    if (dialog == A_RUN && result && runbuf[0]) grin_run(runbuf);
}

// its close box, or its control menu's Close: the session ends, if the answer is OK
static int pm_close(int w) { ask_exit(w); return 0; }

static struct wk_class progman_class = {
    .kind = ICON_FILES, .draw = pm_draw, .mouse = pm_mouse, .key = pm_key, .command = pm_command,
    .answer = pm_answer, .close = pm_close
};

int progman_open() {
    int w;
    if ((w = wk_find(&progman_class))) { wk_raise(w); return w; }
    progman_class.size = sizeof(struct progman);
    menus[0].title = "File"; menus[0].items = file_items;
    menus[1].title = "Help"; menus[1].items = help_items;
    if ((w = wk_open(&progman_class, "Program Manager", 420, 260, 0, 0)) < 0) return w;
    wk_menu(w, menus, 2);
    wk_place(w, 16, 16, 420, 260);
    return w;
}
