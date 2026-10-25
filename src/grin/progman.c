// progman.c -- Program Manager, grin's shell window (docs/grin-plan.md 4).
//
// For now a placeholder: the Main group, holding Control Panel, and the
// File menu whose Exit grin ends the session. Its groups and items, kept
// in PROGMAN.INI, come with G4. Closing Program Manager ends the session
// too, as it ended Windows: both ask first.

#include "shell.h"

struct progman { int sel; };

static char *file_items[2] = { "Exit grin...", 0 };
static char *help_items[2] = { "About grin...", 0 };
static struct wk_menu menus[2];
enum { A_EXIT = 1, A_ABOUT = 2 };
enum { GROUP_X = 8, GROUP_Y = 8, ITEMS_Y = 32 };

static void pm_draw(int w, int cw, int ch, int focused) {
    struct progman *p;
    int gw, gh;
    p = (struct progman *)wk_state(w);
    d_rect(0, 0, cw, ch, C_WHITE);
    // the Main group, open inside it (a group window of its own in G7)
    gw = cw - 2 * GROUP_X; gh = ch - 2 * GROUP_Y;
    if (gw < 100 || gh < 80) return;
    d_rect(GROUP_X, GROUP_Y, gw, gh, C_DARK);
    d_rect(GROUP_X + 1, GROUP_Y + 1, gw - 2, gh - 2, C_FACE);
    d_rect(GROUP_X + 3, GROUP_Y + 3, gw - 6, 16, C_TITLE);
    d_text(GROUP_X + gw / 2 - 12, GROUP_Y + 5, wk_ink(C_TITLE), F_BOLD, "Main");
    d_rect(GROUP_X + 3, GROUP_Y + 20, gw - 6, gh - 23, C_WHITE);
    grin_item(GROUP_X + 3 + ITEM_W / 2 + 4, GROUP_Y + ITEMS_Y, PIC_CONTROL, "Control Panel", p->sel == 1 && focused);
}

static void pm_mouse(int w, int ev, int x, int y, int b) {
    struct progman *p;
    p = (struct progman *)wk_state(w);
    if (ev != E_DOWN && ev != E_DBL) return;
    if (grin_item_hit(x, y, GROUP_X + 3 + ITEM_W / 2 + 4, GROUP_Y + ITEMS_Y)) {
        p->sel = 1;
        if (ev == E_DBL) control_open();
    } else p->sel = 0;
}

static void pm_key(int w, int down, int code, int ch, int mods) {
    struct progman *p;
    p = (struct progman *)wk_state(w);
    if (!down) return;
    if (code == KEY_ENTER && p->sel == 1) control_open();
    else if (code == KEY_RIGHT || code == KEY_LEFT || code == KEY_HOME) p->sel = 1;
}

static void ask_exit(int w) {
    wk_msgbox(w, A_EXIT, "Exit grin", "This will end your grin session.", WK_OKCANCEL);
}

static void pm_command(int w, int id) {
    if (id == WK_CMD(0, 0)) ask_exit(w);
    else if (id == WK_CMD(1, 0))
        wk_msgbox(w, A_ABOUT, "About Program Manager",
                  "grin, a graphical shell for C4DOS in the manner of Windows 3.1. Choose File > Exit grin to return to A>.", WK_OK);
}

static void pm_answer(int w, int dialog, int result) {
    if (dialog == A_EXIT && result) grin_quit();
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
