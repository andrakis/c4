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
// Each group is a window inside Program Manager's (MDI, below), with its
// place kept on its [name] line. File > New and Properties edit an item in
// the active group, Delete removes it; each change is written to
// PROGMAN.INI at once.
// File > Run... takes a command line, and File > Exit grin, or closing
// Program Manager, ends the session, as it ended Windows: both ask first.

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "shell.h"

enum { GROUPS = 8, ITEMS = 48, G_NAME = 24, I_TITLE = 32, I_CMD = 96, I_ICON = 12, INI_MAX = 8192 };
enum { RUN_APP = 0, RUN_FULL = 1, RUN_WINDOW = 2, N_MODES = 3 };

enum { GS_NORMAL = 0, GS_MIN = 1, GS_MAX = 2 };
// a group, and its window inside Program Manager's (below): placed once it
// has a place, its state, its place in the order (the highest in front),
// its icon's slot when minimised, and the item selected in it (1 up)
struct group { char name[G_NAME]; int placed, x, y, w, h, state, z, islot, sel; };
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

// numbers separated by commas, at most max of them: how many
static int nums(char *p, int *v, int max) {
    int n, neg, k;
    n = 0;
    while (*p == ' ') ++p;
    while (*p && n < max) {
        neg = 0;
        if (*p == '-') { neg = 1; ++p; }
        if (*p < '0' || *p > '9') break;
        k = 0;
        while (*p >= '0' && *p <= '9') { k = k * 10 + *p - '0'; ++p; }
        v[n] = neg ? 0 - k : k; ++n;
        if (*p != ',') break;
        ++p;
    }
    return n;
}

// one line of it, in `line`: "[Main] 6,6,340,190,0,3,0" is a group, with
// its window's place (G7's; a grin before G7 reads the name and no more)
static void ini_take() {
    char *p, *f;
    int k, n, v[7];
    struct item *it;
    struct group *g;
    p = line;
    if (p[0] == '[') {
        if (ngroups >= GROUPS) return;
        g = &groups[ngroups];
        memset(g, 0, sizeof(struct group));
        k = 0; ++p;
        while (*p && *p != ']' && k < G_NAME - 1) { g->name[k] = *p; ++k; ++p; }
        g->name[k] = 0;
        if (!k) return;
        if (*p == ']' && (n = nums(p + 1, v, 7)) >= 5) {
            g->placed = 1;
            g->x = v[0]; g->y = v[1]; g->w = v[2]; g->h = v[3];
            g->state = v[4] >= GS_NORMAL && v[4] <= GS_MAX ? v[4] : GS_NORMAL;
            g->z = n > 5 ? v[5] : 0;
            g->islot = n > 6 ? v[6] : 0;
        }
        ++ngroups;
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
    int g, k, fd, v[7];
    char num[16];
    inibuf[0] = 0;
    g = 0;
    while (g < ngroups) {
        ui_ncat(inibuf, "[", INI_MAX); ui_ncat(inibuf, groups[g].name, INI_MAX); ui_ncat(inibuf, "]", INI_MAX);
        if (groups[g].placed) {
            v[0] = groups[g].x; v[1] = groups[g].y; v[2] = groups[g].w; v[3] = groups[g].h;
            v[4] = groups[g].state; v[5] = groups[g].z; v[6] = groups[g].islot;
            k = 0;
            while (k < 7) { ui_ncat(inibuf, k ? "," : " ", INI_MAX); ui_ncat(inibuf, ui_num(num, v[k]), INI_MAX); ++k; }
        }
        ui_ncat(inibuf, "\n", INI_MAX);
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

// ---- the window: group windows inside it (MDI) -------------------------------------------
//
// Each group is a window of Program Manager's own, inside its client area,
// as 3.1's were: a title bar with the control-menu box and the minimise and
// maximise buttons, a sizing border, and the group's items. They overlap;
// the active one is in front and has the keyboard. A group minimised is an
// icon along the bottom of the client area, and a group maximised fills
// it (Program Manager's title then names it: "Program Manager - [Main]").
// The Window menu cascades or tiles them, arranges the icons, and lists
// the groups, a choice bringing that one forward (restored if it was an
// icon). Their places are kept on their PROGMAN.INI lines, after the
// name: "[Main] 6,6,340,190,0,3,0" -- x, y, width, height, the state (0,
// 1 minimised, 2 maximised), the place in the order (the highest in
// front) and the icon's slot.
//
// Keys: the arrows move the selection in the active group and Enter starts
// the item (or restores the active group from its icon); Ctrl+F6, Tab and
// PgDn go to the next group, Shift+Tab and PgUp to the one before;
// Shift+F5 cascades and Shift+F4 tiles.

struct progman { int group, sel; };     // the active group, and the item selected (1 up; 0 none)

static char *file_items[9] = { "New...", "Open", "Delete", "Properties...", "-", "Run...", "-", "Exit grin...", 0 };
static char *window_items[4 + GROUPS + 1];
enum { WNAME = G_NAME + 4 };
static char wnames[GROUPS * WNAME];       // the menu's "2 Accessories", WNAME bytes each
static char *help_items[2] = { "About grin...", 0 };
static struct wk_menu menus[3];
enum { F_NEW = 0, F_OPEN = 1, F_DELETE = 2, F_PROPS = 3, F_RUN = 5, F_EXIT = 7 };
enum { W_CASCADE = 0, W_TILE = 1, W_ARRANGE = 2, W_GROUP = 4 };
enum { A_EXIT = 1, A_ABOUT = 2, A_RUN = 3, A_DELETE = 4 };
enum { KEY_F6 = 117 };
// a group window's measures: its border, title bar and smallest size; the
// icons' row along the bottom, and an icon's step along it
enum { CB = 3, CT = 18, CMIN_W = 140, CMIN_H = 60, ICON_ROW = 64, ICON_STEP = ITEM_W + 4 };
static char runbuf[WK_PATH];
static char about[160];
static char pm_title[G_NAME + 32];
static int drag_g, drag_dx, drag_dy;    // a group's title bar being dragged (group + 1)
static int size_g, size_e, size_x0, size_y0, size_gx, size_gy, size_gw, size_gh;   // ... its border

static struct progman *PM(int w) { return (struct progman *)wk_state(w); }

// the group maximised, or -1; is any minimised?
static int maxed() { int g; g = 0; while (g < ngroups) { if (groups[g].state == GS_MAX) return g; ++g; } return 0 - 1; }
static int any_min() { int g; g = 0; while (g < ngroups) { if (groups[g].state == GS_MIN) return 1; ++g; } return 0; }

// a group's frame on the client area (a maximised one fills it)
static int gx(int g) { return groups[g].state == GS_MAX ? 0 : groups[g].x; }
static int gy(int g) { return groups[g].state == GS_MAX ? 0 : groups[g].y; }
static int gw(int g, int cw) { return groups[g].state == GS_MAX ? cw : groups[g].w; }
static int gh(int g, int ch) { return groups[g].state == GS_MAX ? ch : groups[g].h; }

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

// how many items a row of group g holds; the k-th item's centre and top
static int cols(int g, int cw) {
    int n;
    n = (gw(g, cw) - 2 * CB - 10) / (ITEM_W + 4);
    return n < 1 ? 1 : n;
}
static int item_cx(int g, int k, int cw) { return gx(g) + CB + 5 + ITEM_W / 2 + (k % cols(g, cw)) * (ITEM_W + 4); }
static int item_y(int g, int k, int cw) { return gy(g) + CB + CT + 6 + (k / cols(g, cw)) * (ITEM_H + 6); }

// a minimised group's icon: its centre and top
static int icon_cx(int g) { return 8 + ICON_STEP / 2 + groups[g].islot * ICON_STEP; }
static int icon_y(int ch) { return ch - ICON_ROW + 6; }

// g to the front of the order
static void to_front(int g) {
    int k, top;
    top = 0;
    k = 0;
    while (k < ngroups) { if (k != g && groups[k].z > top) top = groups[k].z; ++k; }
    if (groups[g].z <= top) groups[g].z = top + 1;
}

// the first icon slot no minimised group holds
static int free_slot() {
    int s, g, used;
    s = 0;
    while (1) {
        used = 0;
        g = 0;
        while (g < ngroups) { if (groups[g].state == GS_MIN && groups[g].islot == s) used = 1; ++g; }
        if (!used) return s;
        ++s;
    }
}

// Places for the groups that have none (a PROGMAN.INI from before G7):
// the first open, over the icons' row, the others icons, as 3.1 first
// showed Main with the rest minimised. Each is given the place it would
// cascade to, for when it is restored.
static void default_places(int cw, int ch) {
    int g, w, h;
    g = 0;
    while (g < ngroups) {
        if (!groups[g].placed) {
            w = 4 * (ITEM_W + 4) + 2 * CB + 14;
            if (w > cw - 12) w = cw - 12;
            h = ch - 12 - ICON_ROW;
            if (h < CMIN_H) h = CMIN_H;
            groups[g].x = 6 + g * (CT + 4); groups[g].y = 6 + g * (CT + 4); groups[g].w = w; groups[g].h = h;
            groups[g].placed = 1;
            if (g == 0) { groups[g].state = GS_NORMAL; to_front(g); }
            else { groups[g].state = GS_MIN; groups[g].islot = free_slot(); }
        }
        ++g;
    }
}

// a group pulled back, so that its title bar can still be reached
static void fit(int g, int cw, int ch) {
    struct group *p;
    p = &groups[g];
    if (p->w < CMIN_W) p->w = CMIN_W;
    if (p->h < CMIN_H) p->h = CMIN_H;
    if (p->x > cw - 40) p->x = cw - 40;
    if (p->x < 40 - p->w) p->x = 40 - p->w;
    if (p->y > ch - CT - 2 * CB) p->y = ch - CT - 2 * CB;
    if (p->y < 0) p->y = 0;
}

static void retitle(int w) {
    int m;
    m = maxed();
    pm_title[0] = 0;
    ui_ncat(pm_title, "Program Manager", G_NAME + 32);
    if (m >= 0) { ui_ncat(pm_title, " - [", G_NAME + 32); ui_ncat(pm_title, groups[m].name, G_NAME + 32); ui_ncat(pm_title, "]", G_NAME + 32); }
    if (!ui_eq(pm_title, wk_get_title(w))) wk_title(w, pm_title);
}

// group g active, and in front, with its selection
static void activate(int w, int g) {
    struct progman *p;
    int m;
    p = PM(w);
    if (g < 0 || g >= ngroups) return;
    if (p->group != g && p->group < ngroups) groups[p->group].sel = p->sel;
    // one is maximised and another is chosen: that one is maximised in its place
    m = maxed();
    if (m >= 0 && m != g && groups[g].state != GS_MIN) { groups[m].state = GS_NORMAL; groups[g].state = GS_MAX; }
    p->group = g;
    p->sel = groups[g].sel;
    to_front(g);
    retitle(w);
    wk_invalidate(w, 0, 0, 0, 0);
}

static void minimise(int w, int g) {
    if (groups[g].state == GS_MIN) return;
    groups[g].state = GS_MIN;
    groups[g].islot = free_slot();
    retitle(w);
    pm_save();
}
static void restore(int w, int g) {
    if (groups[g].state == GS_MIN) groups[g].state = maxed() >= 0 ? GS_MAX : GS_NORMAL;
    else if (groups[g].state == GS_MAX) groups[g].state = GS_NORMAL;
    activate(w, g);
    pm_save();
}
static void maximise(int w, int g) {
    int m;
    if ((m = maxed()) >= 0) groups[m].state = GS_NORMAL;
    groups[g].state = GS_MAX;
    activate(w, g);
    pm_save();
}

// ---- Cascade, Tile, Arrange Icons ------------------------------------------------------

// the groups open (not icons), from the back of the order to the front:
// the k-th of them, or -1
static int open_nth(int k) {
    int g, z, best, bz, last;
    last = 0 - 1;
    while (k >= 0) {
        best = 0 - 1; bz = 0;
        g = 0;
        while (g < ngroups) {
            z = groups[g].z;
            if (groups[g].state != GS_MIN && (last < 0 || z > groups[last].z || (z == groups[last].z && g > last))
                && (best < 0 || z < bz || (z == bz && g < best))) { best = g; bz = z; }
            ++g;
        }
        if (best < 0) return 0 - 1;
        last = best;
        --k;
    }
    return last;
}
static int open_count() { int n; n = 0; while (open_nth(n) >= 0) ++n; return n; }

static void cascade(int w) {
    int k, g, cw, ch, n, ww, hh;
    wk_size(w, &cw, &ch);
    if ((g = maxed()) >= 0) groups[g].state = GS_NORMAL;
    n = open_count();
    if (any_min()) ch = ch - ICON_ROW;
    ww = cw * 3 / 4; hh = ch * 3 / 4;
    if (ww < CMIN_W) ww = CMIN_W;
    if (hh < CMIN_H) hh = CMIN_H;
    k = 0;
    while (k < n) {
        g = open_nth(k);
        groups[g].x = 4 + k * (CT + 4); groups[g].y = 4 + k * (CT + 4);
        groups[g].w = ww; groups[g].h = hh;
        ++k;
    }
    retitle(w);
    pm_save();
}

// side by side, in as many rows as a square of them takes; a short last
// row's windows share its width
static void tile(int w) {
    int k, g, cw, ch, n, c, r, rows, in_row, rh, x;
    wk_size(w, &cw, &ch);
    if ((g = maxed()) >= 0) groups[g].state = GS_NORMAL;
    n = open_count();
    if (!n) return;
    if (any_min()) ch = ch - ICON_ROW;
    c = 1; while (c * c < n) ++c;
    rows = (n + c - 1) / c;
    rh = ch / rows;
    k = 0;
    r = 0;
    while (r < rows) {
        in_row = n - k < c ? n - k : c;
        x = 0;
        while (x < in_row) {
            g = open_nth(k);
            groups[g].x = x * cw / in_row; groups[g].y = r * rh;
            groups[g].w = (x + 1) * cw / in_row - x * cw / in_row; groups[g].h = rh;
            ++x; ++k;
        }
        ++r;
    }
    retitle(w);
    pm_save();
}

static void arrange_icons() {
    int g, s;
    s = 0;
    g = 0;
    while (g < ngroups) { if (groups[g].state == GS_MIN) { groups[g].islot = s; ++s; } ++g; }
    pm_save();
}

// ---- drawing ------------------------------------------------------------------------------

// an arrowhead four rows deep, its point at y: up (1) or down (0)
static void arrow(int cx, int y, int up) {
    int i;
    i = 0;
    while (i < 4) { d_rect(cx - i, up ? y + i : y + 3 - i, 2 * i + 1, 1, C_DARK); ++i; }
}
// a raised button on a group's title bar, as look31.c draws a window's
static void cap_button(int x, int y, int what) {
    d_rect(x, y, 1, CT, C_DARK);
    d_rect(x + 1, y, CT - 1, CT, C_FACE);
    d_rect(x + 1, y, CT - 1, 1, C_LIGHT); d_rect(x + 1, y, 1, CT - 1, C_LIGHT);
    d_rect(x + 1, y + CT - 1, CT - 1, 1, C_SHADOW); d_rect(x + CT - 1, y, 1, CT, C_SHADOW);
    if (what == HT_MIN) arrow(x + CT / 2, y + 7, 0);
    else if (what == HT_MAX) arrow(x + CT / 2, y + 7, 1);
    else { arrow(x + CT / 2, y + 3, 1); arrow(x + CT / 2, y + 10, 0); }
}

static void draw_group(struct progman *p, int g, int cw, int ch, int active) {
    int x, y, w, h, bg, k, i, x1, x2, tx;
    x = gx(g); y = gy(g); w = gw(g, cw); h = gh(g, ch);
    // the sizing border: black outside, the face inside
    d_rect(x, y, w, h, C_FACE);
    d_recto(x, y, w - 1, h - 1, C_DARK);
    // the title bar: the control-menu box, the name, the two buttons
    bg = active ? C_TITLE : C_LIGHT;
    d_rect(x + CB, y + CB, w - 2 * CB, CT, bg);
    d_rect(x + CB, y + CB + CT, w - 2 * CB, 1, C_DARK);
    d_rect(x + CB, y + CB, CT - 1, CT, C_FACE);
    d_rect(x + CB + CT - 1, y + CB, 1, CT, C_DARK);
    d_rect(x + CB + 4, y + CB + 8, CT - 8, 3, C_SHADOW);
    d_rect(x + CB + 3, y + CB + 7, CT - 8, 3, C_WHITE);
    d_recto(x + CB + 3, y + CB + 7, CT - 9, 2, C_DARK);
    x1 = x + CB + CT;
    x2 = x + w - CB - 2 * CT;
    cap_button(x2, y + CB, HT_MIN);
    cap_button(x2 + CT, y + CB, groups[g].state == GS_MAX ? HT_NONE : HT_MAX);
    tx = x1 + (x2 - x1 - ui_len(groups[g].name) * 7) / 2;
    if (tx < x1 + 4) tx = x1 + 4;
    d_clip(x1, y + CB, x2 - x1 > 0 ? x2 - x1 : 0, CT);
    d_text(tx, y + CB + 3, wk_ink(bg), F_BOLD, groups[g].name);
    d_clip_end();
    // the items, on white
    d_rect(x + CB, y + CB + CT + 1, w - 2 * CB, h - 2 * CB - CT - 1, C_WHITE);
    d_clip(x + CB, y + CB + CT + 1, w - 2 * CB, h - 2 * CB - CT - 1);
    k = 0;
    while ((i = nth(g, k)) >= 0) {
        if (item_y(g, k, cw) + 20 < y + h && item_y(g, k, cw) + ITEM_H - 10 < ch)
            grin_item_dim(item_cx(g, k, cw), item_y(g, k, cw), icon_pics[icon_index(items[i].icon)], items[i].title,
                          active && p->sel == i + 1, 0);
        ++k;
    }
    d_clip_end();
}

// a minimised group: a little group window, its name under it
static void draw_icon(int g, int ch, int sel) {
    int cx, y, lw, k;
    cx = icon_cx(g); y = icon_y(ch);
    d_rect(cx - 15, y + 4, 30, 24, C_DARK);
    d_rect(cx - 14, y + 5, 28, 22, C_WHITE);
    d_rect(cx - 14, y + 5, 28, 5, C_TITLE);
    k = 0;
    while (k < 4) { d_rect(cx - 11 + (k % 2) * 12, y + 13 + (k / 2) * 7, 8, 5, k == 3 ? C_SHADOW : C_SEL); ++k; }
    lw = ui_len(groups[g].name) * 6 + 4;
    if (sel) d_rect(cx - lw / 2, y + 35, lw, 14, C_SEL);
    d_text(cx - lw / 2 + 2, y + 36, sel ? C_WHITE : C_DARK, F_SANS, groups[g].name);
}

static void pm_draw(int w, int cw, int ch, int focused) {
    struct progman *p;
    int g, k, m;
    p = PM(w);
    d_rect(0, 0, cw, ch, C_FACE);
    if (cw < 120 || ch < 60) return;
    default_places(cw, ch);
    g = 0;
    while (g < ngroups) { if (groups[g].state == GS_NORMAL) fit(g, cw, ch); ++g; }
    if ((m = maxed()) >= 0) { draw_group(p, m, cw, ch, focused && p->group == m); return; }
    g = 0;
    while (g < ngroups) { if (groups[g].state == GS_MIN) draw_icon(g, ch, p->group == g); ++g; }
    k = 0;
    while ((g = open_nth(k)) >= 0) { draw_group(p, g, cw, ch, focused && p->group == g); ++k; }
}

// ---- the mouse -----------------------------------------------------------------------

// what of group g is at x, y, or HT_NONE: HT_SIZE and SZ_* bits on its
// border, as the kit's looks say
static int ghit(int g, int x, int y, int cw, int ch) {
    int x0, y0, w, h, e, x2;
    x0 = gx(g); y0 = gy(g); w = gw(g, cw); h = gh(g, ch);
    if (!ui_hit(x, y, x0, y0, w, h)) return HT_NONE;
    if (ui_hit(x, y, x0 + CB, y0 + CB, CT, CT)) return HT_SYSMENU;
    x2 = x0 + w - CB - 2 * CT;
    if (ui_hit(x, y, x2, y0 + CB, CT, CT)) return HT_MIN;
    if (ui_hit(x, y, x2 + CT, y0 + CB, CT, CT)) return HT_MAX;
    if (groups[g].state != GS_MAX) {
        e = 0;
        if (x < x0 + CB) e = SZ_L; else if (x >= x0 + w - CB) e = SZ_R;
        if (y < y0 + CB) e = e | SZ_T; else if (y >= y0 + h - CB) e = e | SZ_B;
        // a border's ends move two edges
        if (e == SZ_L || e == SZ_R) { if (y < y0 + CT) e = e | SZ_T; else if (y >= y0 + h - CT) e = e | SZ_B; }
        if (e == SZ_T || e == SZ_B) { if (x < x0 + CT) e = e | SZ_L; else if (x >= x0 + w - CT) e = e | SZ_R; }
        if (e) return HT_SIZE + e;
    }
    if (y < y0 + CB + CT + 1) return HT_CAPTION;
    return HT_CLIENT;
}

// the group at x, y, the front first, and what of it is there; -1 for none
static int group_at(int x, int y, int cw, int ch, int *ht) {
    int k, g;
    if ((g = maxed()) >= 0) { *ht = ghit(g, x, y, cw, ch); return *ht ? g : 0 - 1; }
    k = open_count() - 1;
    while (k >= 0) {
        g = open_nth(k);
        if ((*ht = ghit(g, x, y, cw, ch))) return g;
        --k;
    }
    return 0 - 1;
}

// the minimised group whose icon is at x, y, or -1
static int icon_at(int x, int y, int ch) {
    int g;
    if (maxed() >= 0) return 0 - 1;
    g = 0;
    while (g < ngroups) {
        if (groups[g].state == GS_MIN && ui_hit(x, y, icon_cx(g) - ITEM_W / 2, icon_y(ch), ITEM_W, 52)) return g;
        ++g;
    }
    return 0 - 1;
}

static void pm_mouse(int w, int ev, int x, int y, int b) {
    struct progman *p;
    struct group *q;
    int k, i, cw, ch, g, ht, dx, dy;
    p = PM(w);
    wk_size(w, &cw, &ch);
    if (ev == E_MOVE) {
        if (drag_g) {
            q = &groups[drag_g - 1];
            q->x = x - drag_dx; q->y = y - drag_dy;
            fit(drag_g - 1, cw, ch);
            wk_invalidate(w, 0, 0, 0, 0);
        } else if (size_g) {
            q = &groups[size_g - 1];
            dx = x - size_x0; dy = y - size_y0;
            if (size_e & SZ_R) q->w = size_gw + dx;
            if (size_e & SZ_B) q->h = size_gh + dy;
            if (size_e & SZ_L) q->w = size_gw - dx;
            if (size_e & SZ_T) q->h = size_gh - dy;
            if (q->w < CMIN_W) q->w = CMIN_W;
            if (q->h < CMIN_H) q->h = CMIN_H;
            if (size_e & SZ_L) q->x = size_gx + size_gw - q->w;
            if (size_e & SZ_T) q->y = size_gy + size_gh - q->h;
            wk_invalidate(w, 0, 0, 0, 0);
        }
        return;
    }
    if (ev == E_UP) {
        if (drag_g || size_g) pm_save();
        drag_g = 0; size_g = 0;
        return;
    }
    if (ev != E_DOWN && ev != E_DBL) return;
    drag_g = 0; size_g = 0;
    if ((g = group_at(x, y, cw, ch, &ht)) < 0) {
        // the icons: a click makes the group active, a double click restores it
        if ((g = icon_at(x, y, ch)) >= 0) { if (ev == E_DBL) restore(w, g); else activate(w, g); }
        return;
    }
    if (ev == E_DOWN || p->group != g) activate(w, g);
    if (ev == E_DBL) {
        if (ht == HT_SYSMENU) minimise(w, g);           // 3.1's Close, on a group: it becomes an icon
        else if (ht == HT_CAPTION) { if (groups[g].state == GS_MAX) restore(w, g); else maximise(w, g); }
        else if (ht == HT_CLIENT && p->sel && items[p->sel - 1].group == g) launch(&items[p->sel - 1]);
        wk_invalidate(w, 0, 0, 0, 0);
        return;
    }
    q = &groups[g];
    if (ht == HT_MIN) minimise(w, g);
    else if (ht == HT_MAX) { if (q->state == GS_MAX) restore(w, g); else maximise(w, g); }
    else if (ht == HT_CAPTION && q->state != GS_MAX) { drag_g = g + 1; drag_dx = x - q->x; drag_dy = y - q->y; }
    else if (ht >= HT_SIZE) {
        size_g = g + 1; size_e = ht - HT_SIZE;
        size_x0 = x; size_y0 = y; size_gx = q->x; size_gy = q->y; size_gw = q->w; size_gh = q->h;
    } else if (ht == HT_CLIENT) {
        p->sel = 0;
        k = 0;
        while ((i = nth(g, k)) >= 0 && !p->sel) {
            if (grin_item_hit(x, y, item_cx(g, k, cw), item_y(g, k, cw))) p->sel = i + 1;
            ++k;
        }
        groups[g].sel = p->sel;
    }
    wk_invalidate(w, 0, 0, 0, 0);
}

static int pm_pointer(int w, int x, int y) {
    int cw, ch, ht;
    wk_size(w, &cw, &ch);
    if (group_at(x, y, cw, ch, &ht) >= 0 && ht >= HT_SIZE) return PTR_RESIZE;
    return PTR_ARROW;
}

// ---- the keyboard ---------------------------------------------------------------------

// the selected item's place in its group (0 up), or -1
static int sel_k(struct progman *p) {
    int k, i;
    k = 0;
    while ((i = nth(p->group, k)) >= 0) { if (i + 1 == p->sel) return k; ++k; }
    return 0 - 1;
}

static void pm_key(int w, int down, int code, int ch, int mods) {
    struct progman *p;
    int k, n, cw, chh, g, c;
    p = PM(w);
    if (!down || !ngroups) return;
    wk_size(w, &cw, &chh);
    g = p->group;
    if (code == KEY_F5 && (mods & M_SHIFT)) { cascade(w); wk_invalidate(w, 0, 0, 0, 0); return; }
    if (code == KEY_F4 && (mods & M_SHIFT)) { tile(w); wk_invalidate(w, 0, 0, 0, 0); return; }
    if (code == KEY_ENTER) {
        if (groups[g].state == GS_MIN) restore(w, g);
        else if (p->sel) launch(&items[p->sel - 1]);
        return;
    }
    if (((code == KEY_F6 || code == KEY_TAB) && !(mods & M_SHIFT)) || code == KEY_PGDN) { activate(w, (g + 1) % ngroups); return; }
    if (code == KEY_F6 || code == KEY_TAB || code == KEY_PGUP) { activate(w, g ? g - 1 : ngroups - 1); return; }
    if (groups[g].state == GS_MIN || !(n = count(g))) return;
    k = sel_k(p);
    c = cols(g, cw);
    if (code == KEY_RIGHT) k = k < 0 ? 0 : (k + 1) % n;
    else if (code == KEY_LEFT) k = k <= 0 ? n - 1 : k - 1;
    else if (code == KEY_DOWN) k = k < 0 ? 0 : (k + c < n ? k + c : k);
    else if (code == KEY_UP) k = k < 0 ? 0 : (k >= c ? k - c : k);
    else if (code == KEY_HOME) k = 0;
    else if (code == KEY_END) k = n - 1;
    else return;
    p->sel = k >= 0 && nth(g, k) >= 0 ? nth(g, k) + 1 : 0;
    groups[g].sel = p->sel;
    wk_invalidate(w, 0, 0, 0, 0);
}

static void props_open(int item, int group);

static void ask_exit(int w) {
    wk_msgbox(w, A_EXIT, "Exit grin", "This will end your grin session.", WK_OKCANCEL);
}

// the Window menu: its three commands, then the groups numbered, as 3.1
// listed its windows there
static void window_menu() {
    int g, k;
    window_items[0] = "Cascade"; window_items[1] = "Tile"; window_items[2] = "Arrange Icons"; window_items[3] = "-";
    k = 4;
    g = 0;
    while (g < ngroups) {
        ui_num(wnames + g * WNAME, g + 1);
        ui_ncat(wnames + g * WNAME, " ", WNAME);
        ui_ncat(wnames + g * WNAME, groups[g].name, WNAME);
        window_items[k] = wnames + g * WNAME;
        ++k; ++g;
    }
    window_items[k] = 0;
    if (!ngroups) window_items[3] = 0;
}

static void pm_command(int w, int id) {
    struct progman *p;
    char num[24];
    int g;
    p = PM(w);
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
    else if (id == WK_CMD(1, W_CASCADE)) cascade(w);
    else if (id == WK_CMD(1, W_TILE)) tile(w);
    else if (id == WK_CMD(1, W_ARRANGE)) arrange_icons();
    else if (id >= WK_CMD(1, W_GROUP) && id < WK_CMD(1, W_GROUP + ngroups)) {
        g = id - WK_CMD(1, W_GROUP);
        if (groups[g].state == GS_MIN) restore(w, g); else activate(w, g);
    } else if (id == WK_CMD(2, 0)) {
        about[0] = 0;
        ui_ncat(about, "grin, a graphical shell for C4DOS in the manner of Windows 3.1. Applications running: ", 160);
        ui_ncat(about, ui_num(num, apps_count()), 160);
        ui_ncat(about, ". File > Exit grin returns to A>.", 160);
        wk_msgbox(w, A_ABOUT, "About Program Manager", about, WK_OK);
    }
    wk_invalidate(w, 0, 0, 0, 0);
}

static void pm_answer(int w, int dialog, int result) {
    struct progman *p;
    p = PM(w);
    if (dialog == A_EXIT && result) grin_quit();
    if (dialog == A_RUN && result && runbuf[0]) grin_start(runbuf);
    if (dialog == A_DELETE && result && p->sel) {
        items[p->sel - 1].used = 0;
        p->sel = 0;
        groups[p->group].sel = 0;
        pm_save();
        wk_invalidate(w, 0, 0, 0, 0);
    }
}

// its close box, or its control menu's Close: the session ends, if the answer is OK
static int pm_close(int w) { ask_exit(w); return 0; }

static struct wk_class progman_class = {
    .kind = ICON_FILES, .draw = pm_draw, .mouse = pm_mouse, .key = pm_key, .command = pm_command,
    .answer = pm_answer, .close = pm_close, .pointer = pm_pointer
};

int progman_open() {
    int w, g, top;
    if ((w = wk_find(&progman_class))) { wk_raise(w); return w; }
    if (!loaded) pm_load();
    progman_class.size = sizeof(struct progman);
    window_menu();
    menus[0].title = "File"; menus[0].items = file_items;
    menus[1].title = "Window"; menus[1].items = window_items;
    menus[2].title = "Help"; menus[2].items = help_items;
    if ((w = wk_open(&progman_class, "Program Manager", 500, 300, 0, 0)) < 0) return w;
    wk_menu(w, menus, 3);
    wk_place(w, 16, 16, 500, 300);
    // the active group: the one in front
    top = 0 - 1;
    g = 0;
    while (g < ngroups) { if (top < 0 || groups[g].z > groups[top].z) top = g; ++g; }
    PM(w)->group = top < 0 ? 0 : top;
    PM(w)->sel = top < 0 ? 0 : groups[top].sel;
    retitle(w);
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
