// progman.c -- Program Manager, grin's shell window (docs/grin-plan.md 4).
//
// Its groups and their items are PROGMAN.INI's, a file on the home drive
// (C4DOS's HOME=B:, which the page keeps), made from the defaults below
// the first time grin starts without one:
//
//   [Main]
//   File Manager=FILEMAN,files,app
//   ...
//   [Games]
//   Doom=\GAMES\DOOM\DOOM,game,window
//
// An item is a title, a command line, an icon and a run mode: app (a grin
// application, or one of grin's own windows, named by the command lines
// FILEMAN, CONTROL, PROGMAN and DOSPRMPT), full (a DOS program, full
// screen) or window (a DOS program in a window of its own: the games,
// whose bare platform layers find grin and play in it, fwin.c). A DOS
// program started from File Manager or File > Run runs as the first item
// that names the same program says, as a PIF said, else full screen
// (grin_dos).
//
// One group is shown at a time, picked from the list down the left (MDI
// group windows come with G7). File > New and Properties edit an item,
// Delete removes it; each change is written to PROGMAN.INI at once.
// File > Run... takes a command line, and File > Exit grin, or closing
// Program Manager, ends the session, as it ended Windows: both ask first.

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "shell.h"

enum { GROUPS = 8, ITEMS = 48, G_NAME = 24, I_TITLE = 32, I_CMD = 96, I_ICON = 12, INI_MAX = 8192 };
enum { RUN_APP = 0, RUN_FULL = 1, RUN_WINDOW = 2, N_MODES = 3 };

struct group { char name[G_NAME]; };
struct item { int used, group, mode; char title[I_TITLE]; char cmd[I_CMD]; char icon[I_ICON]; };
static struct group groups[GROUPS];
static struct item items[ITEMS];
static int ngroups, loaded;

static char *defaults[16] = {
    "[Main]",
    "File Manager=FILEMAN,files,app",
    "DOS Prompt=DOSPRMPT,dos,app",
    "Control Panel=CONTROL,control,app",
    "Notepad=NOTEPAD,notepad,app",
    "[Accessories]",
    "Clock=CLOCK,clock,app",
    "Calculator=CALC,calc,app",
    "[Games]",
    "Wolfenstein 3D=\\GAMES\\WOLF3D\\WOLF3D,game,window",
    "Doom=\\GAMES\\DOOM\\DOOM,game,window",
    "Tyrian=\\GAMES\\TYRIAN\\TYRIAN,game,window",
    "Rise of the Triad=\\GAMES\\ROTT\\ROTT,game,window",
    0
};
// A PROGMAN.INI written before the games had directories (C4DOS's API
// v7) names them by their bare words, which no longer find their data:
// such an item is given the new command line when it is read.
static char *moved[9] = {
    "WOLF3D", "\\GAMES\\WOLF3D\\WOLF3D",
    "DOOM", "\\GAMES\\DOOM\\DOOM",
    "TYRIAN", "\\GAMES\\TYRIAN\\TYRIAN",
    "ROTT", "\\GAMES\\ROTT\\ROTT",
    0
};
static int ini_moved;
static char *mode_names[N_MODES] = { "app", "full", "window" };
static char *mode_labels[N_MODES] = { "grin application", "DOS program, full screen", "DOS program, in a window" };
enum { N_ICONS = 8 };
static char *icon_names[N_ICONS] = { "app", "files", "dos", "control", "notepad", "clock", "calc", "game" };
static int icon_pics[N_ICONS] = { PIC_APP, PIC_FILES, PIC_DOS, PIC_CONTROL, PIC_NOTEPAD, PIC_CLOCK, PIC_CALC, PIC_GAME };

static char inibuf[INI_MAX];
static char line[200];

static int up(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int ieq(char *a, char *b) {
    while (*a && up(*a) == up(*b)) { ++a; ++b; }
    return !*a && !*b;
}
// does the command line's first word say w?
static int word_is(char *cmd, char *w) {
    while (*cmd == ' ') ++cmd;
    while (*w && up(*cmd) == up(*w)) { ++cmd; ++w; }
    return !*w && (!*cmd || *cmd == ' ');
}

static int icon_index(char *name) {
    int k;
    k = 0;
    while (k < N_ICONS) { if (ieq(icon_names[k], name)) return k; ++k; }
    return 0;
}

// ---- PROGMAN.INI -----------------------------------------------------------------------

// one line of it, in `line`
static void ini_take() {
    char *p, *f;
    int k, n;
    struct item *it;
    p = line;
    if (p[0] == '[') {
        if (ngroups >= GROUPS) return;
        k = 0; ++p;
        while (*p && *p != ']' && k < G_NAME - 1) { groups[ngroups].name[k] = *p; ++k; ++p; }
        groups[ngroups].name[k] = 0;
        if (k) ++ngroups;
        return;
    }
    if (!ngroups) return;
    k = 0;
    while (p[k] && p[k] != '=') ++k;
    if (!p[k] || !k) return;
    n = 0;
    while (n < ITEMS && items[n].used) ++n;
    if (n == ITEMS) return;
    it = &items[n];
    memset(it, 0, sizeof(struct item));
    p[k] = 0;
    ui_ncpy(it->title, p, I_TITLE);
    p = p + k + 1;
    // command line, icon, mode: split at commas
    f = p; while (*f && *f != ',') ++f;
    if (*f) { *f = 0; ++f; }
    ui_ncpy(it->cmd, p, I_CMD);
    p = f; while (*f && *f != ',') ++f;
    if (*f) { *f = 0; ++f; }
    ui_ncpy(it->icon, *p ? p : "app", I_ICON);
    it->mode = RUN_APP;
    k = 0;
    while (k < N_MODES) { if (ieq(f, mode_names[k])) it->mode = k; ++k; }
    if (!it->cmd[0]) return;
    k = 0;
    while (moved[k]) {
        if (it->mode == RUN_FULL && ieq(it->cmd, moved[k])) { ui_ncpy(it->cmd, moved[k + 1], I_CMD); ini_moved = 1; }
        k = k + 2;
    }
    it->group = ngroups - 1;
    it->used = 1;
}

static void pm_save() {
    int g, k, fd;
    inibuf[0] = 0;
    g = 0;
    while (g < ngroups) {
        ui_ncat(inibuf, "[", INI_MAX); ui_ncat(inibuf, groups[g].name, INI_MAX); ui_ncat(inibuf, "]\n", INI_MAX);
        k = 0;
        while (k < ITEMS) {
            if (items[k].used && items[k].group == g) {
                ui_ncat(inibuf, items[k].title, INI_MAX); ui_ncat(inibuf, "=", INI_MAX);
                ui_ncat(inibuf, items[k].cmd, INI_MAX); ui_ncat(inibuf, ",", INI_MAX);
                ui_ncat(inibuf, items[k].icon, INI_MAX); ui_ncat(inibuf, ",", INI_MAX);
                ui_ncat(inibuf, mode_names[items[k].mode], INI_MAX); ui_ncat(inibuf, "\n", INI_MAX);
            }
            ++k;
        }
        ++g;
    }
    if ((fd = open("PROGMAN.INI", O_WRONLY | O_CREAT | O_TRUNC)) < 0) return;
    write(fd, inibuf, ui_len(inibuf));
    close(fd);
}

// PROGMAN.INI, or the defaults (written out, so the next start finds them)
static void pm_load() {
    int fd, n, i, k;
    loaded = 1;
    ngroups = 0;
    memset(items, 0, sizeof(struct item) * ITEMS);
    n = 0;
    if ((fd = open("PROGMAN.INI", O_RDONLY)) >= 0) {
        n = read(fd, inibuf, INI_MAX - 1);
        close(fd);
        if (n < 0) n = 0;
    }
    inibuf[n] = 0;
    ini_moved = 0;
    i = 0;
    while (i < n) {
        k = 0;
        while (i < n && inibuf[i] != '\n' && inibuf[i] != '\r') { if (k < 199) { line[k] = inibuf[i]; ++k; } ++i; }
        while (i < n && (inibuf[i] == '\n' || inibuf[i] == '\r')) ++i;
        line[k] = 0;
        if (k) ini_take();
    }
    if (ngroups) { if (ini_moved) pm_save(); return; }
    k = 0;
    while (defaults[k]) { ui_ncpy(line, defaults[k], 200); ini_take(); ++k; }
    pm_save();
}

// ---- starting things ---------------------------------------------------------------------

static char msg[WK_PATH + 160];
static void say(char *a, char *b, char *c, char *d) {
    msg[0] = 0;
    ui_ncat(msg, a, WK_PATH + 160); ui_ncat(msg, b, WK_PATH + 160);
    ui_ncat(msg, c, WK_PATH + 160); ui_ncat(msg, d, WK_PATH + 160);
    wk_msgbox(0, 0, "Program Manager", msg, WK_OK);
}


// A command line: one of grin's own windows by name, or a program --
// an application, or a DOS program, which runs full screen (apps.c). 0
// if it started.
int grin_start(char *cmdline) {
    if (word_is(cmdline, "CONTROL")) return control_open() < 0 ? 0 - 1 : 0;
    if (word_is(cmdline, "FILEMAN") || word_is(cmdline, "WINFILE")) return filemgr_open() < 0 ? 0 - 1 : 0;
    if (word_is(cmdline, "PROGMAN")) return progman_open() < 0 ? 0 - 1 : 0;
    if (word_is(cmdline, "DOSPRMPT") || word_is(cmdline, "COMMAND")) return dosprompt_open(0) < 0 ? 0 - 1 : 0;
    return grin_run(cmdline);
}

static void launch(struct item *it) {
    if (it->mode == RUN_FULL) { grin_full(it->cmd); return; }
    if (it->mode == RUN_WINDOW) { grin_window(it->cmd); return; }
    grin_start(it->cmd);
}

// the program a command line names, as a file's name with no drive,
// folder or extension, in capitals: "A:DOOM.C4R -warp 2" -> "DOOM"
static void prog_of(char *cmd, char *out, int n) {
    char *s, *b;
    int k;
    while (*cmd == ' ') ++cmd;
    b = cmd; s = cmd;
    while (*s && *s != ' ') { if (*s == ':' || *s == '/' || *s == '\\') b = s + 1; ++s; }
    k = 0;
    while (b < s && *b != '.' && k < n - 1) { out[k] = up(*b); ++k; ++b; }
    out[k] = 0;
}

// A DOS program from File Manager or File > Run (apps.c's grin_run, when
// the program is not an application): in a window when the first
// Program Manager item that runs the same program runs it in one, else
// full screen
int grin_dos(char *cmdline) {
    char want[16], have[16];
    int k;
    if (!loaded) pm_load();
    prog_of(cmdline, want, 16);
    k = 0;
    while (k < ITEMS) {
        if (items[k].used && items[k].mode != RUN_APP) {
            prog_of(items[k].cmd, have, 16);
            if (want[0] && ieq(want, have)) return items[k].mode == RUN_WINDOW ? grin_window(cmdline) : grin_full(cmdline);
        }
        ++k;
    }
    return grin_full(cmdline);
}

// ---- the window ----------------------------------------------------------------------------

struct progman { int group, sel; };     // the group shown, and the item selected (1 up; 0 none)

static char *file_items[9] = { "New...", "Open", "Delete", "Properties...", "-", "Run...", "-", "Exit grin...", 0 };
static char *help_items[2] = { "About grin...", 0 };
static struct wk_menu menus[2];
enum { F_NEW = 0, F_OPEN = 1, F_DELETE = 2, F_PROPS = 3, F_RUN = 5, F_EXIT = 7 };
enum { A_EXIT = 1, A_ABOUT = 2, A_RUN = 3, A_DELETE = 4 };
enum { LIST_W = 124, ROW = 20, PAD = 6 };
static char runbuf[WK_PATH];
static char about[160];

// the group's frame: x, y, w, h; and how many items a row holds
static int gx() { return PAD + LIST_W + PAD; }
static int cols(int cw) {
    int n;
    n = (cw - gx() - PAD - 10) / (ITEM_W + 4);
    return n < 1 ? 1 : n;
}
// the k-th item of the group shown (0 up): its centre and top
static int item_cx(int k, int cw) { return gx() + 5 + ITEM_W / 2 + (k % cols(cw)) * (ITEM_W + 4); }
static int item_y(int k, int cw) { return PAD + 28 + (k / cols(cw)) * (ITEM_H + 6); }

// the items of group g in order: the index of the k-th (0 up), or -1
static int nth(int g, int k) {
    int i;
    i = 0;
    while (i < ITEMS) {
        if (items[i].used && items[i].group == g) { if (!k) return i; --k; }
        ++i;
    }
    return 0 - 1;
}
static int count(int g) { int k; k = 0; while (nth(g, k) >= 0) ++k; return k; }

static void pm_draw(int w, int cw, int ch, int focused) {
    struct progman *p;
    int g, k, i, x, gw, gh;
    p = (struct progman *)wk_state(w);
    d_rect(0, 0, cw, ch, C_FACE);
    if (cw < gx() + 100 || ch < 100) return;
    // the groups, a list down the left
    d_rect(PAD, PAD, LIST_W, ch - 2 * PAD, C_WHITE);
    d_bevel(PAD, PAD, LIST_W, ch - 2 * PAD, B_SUNKEN);
    g = 0;
    while (g < ngroups && PAD + 4 + (g + 1) * ROW < ch) {
        if (g == p->group) d_rect(PAD + 3, PAD + 3 + g * ROW, LIST_W - 6, ROW - 2, focused ? C_SEL : C_SHADOW);
        // a little group window before the name
        d_rect(PAD + 8, PAD + 7 + g * ROW, 12, 10, C_DARK);
        d_rect(PAD + 9, PAD + 8 + g * ROW, 10, 8, C_WHITE);
        d_rect(PAD + 9, PAD + 8 + g * ROW, 10, 2, C_TITLE);
        d_text(PAD + 26, PAD + 6 + g * ROW, g == p->group ? C_WHITE : C_DARK, F_SANS, groups[g].name);
        ++g;
    }
    // the group shown, as a group window inside
    x = gx(); gw = cw - x - PAD; gh = ch - 2 * PAD;
    d_rect(x, PAD, gw, gh, C_DARK);
    d_rect(x + 1, PAD + 1, gw - 2, gh - 2, C_FACE);
    d_rect(x + 3, PAD + 3, gw - 6, 16, C_TITLE);
    if (p->group < ngroups)
        d_text(x + gw / 2 - ui_len(groups[p->group].name) * 7 / 2, PAD + 5, wk_ink(C_TITLE), F_BOLD, groups[p->group].name);
    d_rect(x + 3, PAD + 20, gw - 6, gh - 23, C_WHITE);
    d_clip(x + 3, PAD + 20, gw - 6, gh - 23);
    k = 0;
    while ((i = nth(p->group, k)) >= 0) {
        if (item_y(k, cw) + ITEM_H - 10 < ch)
            grin_item_dim(item_cx(k, cw), item_y(k, cw), icon_pics[icon_index(items[i].icon)], items[i].title,
                          p->sel == i + 1 && focused, 0);
        ++k;
    }
    d_clip_end();
}

static void pm_mouse(int w, int ev, int x, int y, int b) {
    struct progman *p;
    int k, i, cw, ch;
    p = (struct progman *)wk_state(w);
    if (ev != E_DOWN && ev != E_DBL) return;
    wk_size(w, &cw, &ch);
    if (x < gx()) {
        k = (y - PAD - 3) / ROW;
        if (y >= PAD + 3 && k < ngroups && k != p->group) { p->group = k; p->sel = 0; }
        wk_invalidate(w, 0, 0, 0, 0);
        return;
    }
    p->sel = 0;
    k = 0;
    while ((i = nth(p->group, k)) >= 0 && !p->sel) {
        if (grin_item_hit(x, y, item_cx(k, cw), item_y(k, cw))) p->sel = i + 1;
        ++k;
    }
    wk_invalidate(w, 0, 0, 0, 0);
    if (p->sel && ev == E_DBL) launch(&items[p->sel - 1]);
}

// the selected item's place in its group (0 up), or -1
static int sel_k(struct progman *p) {
    int k, i;
    k = 0;
    while ((i = nth(p->group, k)) >= 0) { if (i + 1 == p->sel) return k; ++k; }
    return 0 - 1;
}

static void pm_key(int w, int down, int code, int ch, int mods) {
    struct progman *p;
    int k, n, cw, chh;
    p = (struct progman *)wk_state(w);
    if (!down) return;
    wk_size(w, &cw, &chh);
    n = count(p->group);
    k = sel_k(p);
    if (code == KEY_ENTER) { if (p->sel) launch(&items[p->sel - 1]); return; }
    if (code == KEY_PGDN || (code == KEY_TAB && !(mods & M_SHIFT))) { p->group = (p->group + 1) % (ngroups ? ngroups : 1); k = 0 - 1; }
    else if (code == KEY_PGUP || code == KEY_TAB) { p->group = p->group ? p->group - 1 : (ngroups ? ngroups - 1 : 0); k = 0 - 1; }
    else if (!n) return;
    else if (code == KEY_RIGHT) k = k < 0 ? 0 : (k + 1) % n;
    else if (code == KEY_LEFT) k = k <= 0 ? n - 1 : k - 1;
    else if (code == KEY_DOWN) k = k < 0 ? 0 : (k + cols(cw) < n ? k + cols(cw) : k);
    else if (code == KEY_UP) k = k < 0 ? 0 : (k >= cols(cw) ? k - cols(cw) : k);
    else if (code == KEY_HOME) k = 0;
    else if (code == KEY_END) k = n - 1;
    else return;
    p->sel = k >= 0 && nth(p->group, k) >= 0 ? nth(p->group, k) + 1 : 0;
    wk_invalidate(w, 0, 0, 0, 0);
}

static void props_open(int item, int group);

static void ask_exit(int w) {
    wk_msgbox(w, A_EXIT, "Exit grin", "This will end your grin session.", WK_OKCANCEL);
}

static void pm_command(int w, int id) {
    struct progman *p;
    char num[24];
    p = (struct progman *)wk_state(w);
    if (id == WK_CMD(0, F_NEW)) props_open(0 - 1, p->group);
    else if (id == WK_CMD(0, F_OPEN)) { if (p->sel) launch(&items[p->sel - 1]); }
    else if (id == WK_CMD(0, F_DELETE) || id == WK_CMD(0, F_PROPS)) {
        if (!p->sel) { wk_msgbox(w, 0, "Program Manager", "Select an item first.", WK_OK); return; }
        if (id == WK_CMD(0, F_PROPS)) { props_open(p->sel - 1, p->group); return; }
        msg[0] = 0;
        ui_ncat(msg, "Are you sure you want to delete the item '", WK_PATH + 160);
        ui_ncat(msg, items[p->sel - 1].title, WK_PATH + 160);
        ui_ncat(msg, "'?", WK_PATH + 160);
        wk_msgbox(w, A_DELETE, "Delete", msg, WK_YESNO);
    } else if (id == WK_CMD(0, F_RUN)) {
        runbuf[0] = 0;
        wk_inputbox(w, A_RUN, "Run", "Command Line:", runbuf, WK_PATH);
    } else if (id == WK_CMD(0, F_EXIT)) ask_exit(w);
    else if (id == WK_CMD(1, 0)) {
        about[0] = 0;
        ui_ncat(about, "grin, a graphical shell for C4DOS in the manner of Windows 3.1. Applications running: ", 160);
        ui_ncat(about, ui_num(num, apps_count()), 160);
        ui_ncat(about, ". File > Exit grin returns to A>.", 160);
        wk_msgbox(w, A_ABOUT, "About Program Manager", about, WK_OK);
    }
}

static void pm_answer(int w, int dialog, int result) {
    struct progman *p;
    p = (struct progman *)wk_state(w);
    if (dialog == A_EXIT && result) grin_quit();
    if (dialog == A_RUN && result && runbuf[0]) grin_start(runbuf);
    if (dialog == A_DELETE && result && p->sel) {
        items[p->sel - 1].used = 0;
        p->sel = 0;
        pm_save();
        wk_invalidate(w, 0, 0, 0, 0);
    }
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
    if (!loaded) pm_load();
    progman_class.size = sizeof(struct progman);
    menus[0].title = "File"; menus[0].items = file_items;
    menus[1].title = "Help"; menus[1].items = help_items;
    if ((w = wk_open(&progman_class, "Program Manager", 500, 300, 0, 0)) < 0) return w;
    wk_menu(w, menus, 2);
    wk_place(w, 16, 16, 500, 300);
    return w;
}

// ---- New and Properties: an item's title, command line, icon and run mode ------------------

struct props {
    char *buf; int max;               // struct grin_dlg's
    int item, group;                  // the item (-1: a new one), and its group
    int field, caret0, caret1;        // the text field with the keyboard (0, 1), and the carets
    int mode, icon;
    char title[I_TITLE];
    char cmd[I_CMD];
};
enum { P_W = 420, P_H = 196, P_FX = 112, BTN_W = 76, BTN_H = 24 };

static void pr_draw(int w, int cw, int ch, int focused) {
    struct props *r;
    int k;
    r = (struct props *)wk_state(w);
    d_rect(0, 0, cw, ch, C_FACE);
    d_text(12, 14, C_DARK, F_SANS, "Description:");
    ui_field(P_FX, 10, cw - P_FX - 12, r->title, r->caret0, r->field == 0);
    d_text(12, 44, C_DARK, F_SANS, "Command Line:");
    ui_field(P_FX, 40, cw - P_FX - 12, r->cmd, r->caret1, r->field == 1);
    ui_group(P_FX - 10, 72, cw - P_FX - 2, 76, "Run as");
    k = 0;
    while (k < N_MODES) { ui_radio(P_FX, 88 + k * 18, mode_labels[k], r->mode == k); ++k; }
    // the icon, and a button that steps through the others
    d_rect(30, 76, 36, 36, C_WHITE);
    grin_picture(icon_pics[r->icon], 32, 78);
    ui_button(12, 118, 72, 22, "Icon", 0, 0);
    ui_button(cw / 2 - BTN_W - 6, ch - BTN_H - 10, BTN_W, BTN_H, "OK", 0, 1);
    ui_button(cw / 2 + 6, ch - BTN_H - 10, BTN_W, BTN_H, "Cancel", 0, 0);
}

static void pr_ok(int w) {
    struct props *r;
    struct item *it;
    int n, pm;
    r = (struct props *)wk_state(w);
    if (!r->title[0] || !r->cmd[0]) { wk_msgbox(w, 0, "Program Item Properties", "An item needs a description and a command line.", WK_OK); return; }
    n = r->item;
    if (n < 0) {
        n = 0;
        while (n < ITEMS && items[n].used) ++n;
        if (n == ITEMS) { wk_msgbox(w, 0, "Program Item Properties", "Program Manager has no room for another item.", WK_OK); return; }
        memset(&items[n], 0, sizeof(struct item));
        items[n].group = r->group;
    }
    it = &items[n];
    ui_ncpy(it->title, r->title, I_TITLE);
    ui_ncpy(it->cmd, r->cmd, I_CMD);
    ui_ncpy(it->icon, icon_names[r->icon], I_ICON);
    it->mode = r->mode;
    it->used = 1;
    pm_save();
    wk_close(w);
    if ((pm = wk_find(&progman_class))) {
        ((struct progman *)wk_state(pm))->sel = n + 1;
        wk_invalidate(pm, 0, 0, 0, 0);
    }
}

static void pr_mouse(int w, int ev, int x, int y, int b) {
    struct props *r;
    int cw, ch, k;
    r = (struct props *)wk_state(w);
    if (ev != E_DOWN) return;
    wk_size(w, &cw, &ch);
    if (ui_hit(x, y, cw / 2 - BTN_W - 6, ch - BTN_H - 10, BTN_W, BTN_H)) { pr_ok(w); return; }
    if (ui_hit(x, y, cw / 2 + 6, ch - BTN_H - 10, BTN_W, BTN_H)) { wk_close(w); return; }
    if (ui_hit(x, y, P_FX, 10, cw - P_FX - 12, 22)) { r->field = 0; r->caret0 = ui_field_hit(x, P_FX, cw - P_FX - 12, r->title, r->caret0); return; }
    if (ui_hit(x, y, P_FX, 40, cw - P_FX - 12, 22)) { r->field = 1; r->caret1 = ui_field_hit(x, P_FX, cw - P_FX - 12, r->cmd, r->caret1); return; }
    if (ui_hit(x, y, 12, 118, 72, 22) || ui_hit(x, y, 30, 76, 36, 36)) { r->icon = (r->icon + 1) % N_ICONS; return; }
    k = 0;
    while (k < N_MODES) { if (ui_hit(x, y, P_FX - 2, 85 + k * 18, 200, 18)) r->mode = k; ++k; }
}

static void pr_key(int w, int down, int code, int ch, int mods) {
    struct props *r;
    r = (struct props *)wk_state(w);
    if (!down) return;
    if (code == KEY_ESC) { wk_close(w); return; }
    if (code == KEY_ENTER) { pr_ok(w); return; }
    if (code == KEY_TAB) { r->field = !r->field; return; }
    if (r->field == 0) ui_field_key(r->title, I_TITLE - 1, &r->caret0, code, ch, mods);
    else ui_field_key(r->cmd, I_CMD - 1, &r->caret1, code, ch, mods);
}

static struct wk_class props_class = { .kind = ICON_SETTINGS, .draw = pr_draw, .mouse = pr_mouse, .key = pr_key };

static void props_open(int item, int group) {
    int w;
    struct props *r;
    if (!ngroups) return;
    if ((w = grin_dialog(&props_class, sizeof(struct props), item < 0 ? "New Program Item" : "Program Item Properties",
                         P_W + 2 * BORDER, P_H + 2 * BORDER + TITLE_H + 1)) < 0) return;
    r = (struct props *)wk_state(w);
    r->item = item; r->group = group;
    if (item >= 0) {
        ui_ncpy(r->title, items[item].title, I_TITLE);
        ui_ncpy(r->cmd, items[item].cmd, I_CMD);
        r->mode = items[item].mode;
        r->icon = icon_index(items[item].icon);
    }
    r->caret0 = ui_len(r->title);
    r->caret1 = ui_len(r->cmd);
}
