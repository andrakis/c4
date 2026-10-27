// filemgr.c -- File Manager (docs/grin-plan.md 4): a drive bar over a
// list of a directory's folders and files.
//
// The bar has A:, every other drive C4DOS has (DISK_COUNT, through its
// slot 29), and RAM:, its RAM disk. A drive is listed from its
// c4dos.dir, which DOS keeps on every drive it writes, a directory at a
// time (C4DOS's API v7); the RAM disk from DOS's own slots. A double
// click on a folder opens it, and on ".." goes back up. A file's size is read when it is selected, as the
// disk has no other way to say it. The list is read again when the
// window comes to the front, so a file Notepad has just saved is there.
//
// A double click opens a file: a text file in Notepad, a .c4r as a grin
// application or, if it is not one, as a DOS program full screen, and a
// .bat in a DOS Prompt window (File > Edit opens it in Notepad instead).
// Copy and Delete are
// DOS's: COPY through exec (slot 19) and unlink (slot 18), on the RAM
// disk and on drives that can be written.

#include <stdlib.h>
#include "shell.h"

enum { FM_MAX = 512, ROW = 16, BAR_H = 32, STATUS_H = 22, DRV_W = 44, LX = 6 };
enum { D_DELETE = 1, D_COPY = 2 };

struct fm {
    int drive;                        // the drive listed (0 = A:), or DRV_RAM
    char dir[WK_PATH];                // its directory listed: "" the root, "GAMES/DOOM"
    int n, top, sel;                  // its files, the first shown, the selected (-1 none)
    int size;                         // the selected file's size: -1 it would not open, -2 not read yet
    int was_focused;
    char *names;                      // FM_MAX names, WK_NAME bytes each
    char path[WK_PATH];               // the selected file's DOS name ("B:/NOTE.TXT")
    char ask[WK_PATH];                // Copy's destination
    char msg[WK_PATH + 96];
};

static char *file_items[9] = { "Open", "Edit", "Copy...", "Delete...", "-", "Refresh", "-", "Exit", 0 };
static char *help_items[2] = { "About File Manager...", 0 };
static struct wk_menu menus[2];
enum { M_OPEN = 0, M_EDIT = 1, M_COPY = 2, M_DELETE = 3, M_REFRESH = 5, M_EXIT = 7 };

static struct fm *F(int w) { return (struct fm *)wk_state(w); }
static char *name_of(struct fm *f, int k) { return f->names + k * WK_NAME; }

// the bar: the drives, then RAM: last
static int bar_n() { return gd_drives() + 1; }
static int bar_drive(int k) { return k < gd_drives() ? k : DRV_RAM; }
static int bar_x(int k) { return LX + k * (DRV_W + 4); }
static char *drive_label(int d, char *out) {
    if (d == DRV_RAM) ui_cpy(out, "RAM:");
    else { out[0] = 'A' + d; out[1] = ':'; out[2] = 0; }
    return out;
}

// is entry k a folder ("NAME/", or ".." going up)?
static int is_folder(struct fm *f, int k) {
    char *s;
    s = name_of(f, k);
    return ui_len(s) > 0 && s[ui_len(s) - 1] == '/';
}

// the DOS name of entry k, from the root, as a person writes it:
// "A:\GAMES\DOOM.C4R", "RAM:\X"
static char *fm_name(struct fm *f, int k, char *out) {
    char lab[8];
    int i;
    ui_ncpy(out, drive_label(f->drive, lab), WK_PATH);
    ui_ncat(out, "\\", WK_PATH);
    i = ui_len(out);
    ui_ncat(out, f->dir, WK_PATH);
    if (f->dir[0]) ui_ncat(out, "/", WK_PATH);
    ui_ncat(out, name_of(f, k), WK_PATH);
    while (out[i]) { if (out[i] == '/') out[i] = '\\'; ++i; }
    return out;
}

static void reload(int w) {
    struct fm *f;
    f = F(w);
    f->n = 0;
    // below the root, ".." first: the way back up
    if (f->names && f->dir[0]) { ui_cpy(f->names, "../"); f->n = 1; }
    if (f->names) f->n = f->n + gd_list(f->drive, f->dir, f->names + f->n * WK_NAME, FM_MAX - f->n);
    if (f->sel >= f->n) f->sel = 0 - 1;
    if (f->top > f->n) f->top = 0;
    f->size = 0 - 2;
    wk_invalidate(w, 0, 0, 0, 0);
}

static void choose(int w, int d) {
    struct fm *f;
    f = F(w);
    f->drive = d; f->sel = 0 - 1; f->top = 0;
    f->dir[0] = 0;
    reload(w);
}

// into folder k: down into it, or up for ".."
static void enter(int w, int k) {
    struct fm *f;
    int n;
    char *s;
    f = F(w);
    s = name_of(f, k);
    if (s[0] == '.' && s[1] == '.') {
        n = ui_len(f->dir);
        while (n > 0 && f->dir[n - 1] != '/') --n;
        if (n > 0) --n;
        f->dir[n] = 0;
    } else {
        if (f->dir[0]) ui_ncat(f->dir, "/", WK_PATH);
        ui_ncat(f->dir, s, WK_PATH);
        n = ui_len(f->dir);
        if (n > 0 && f->dir[n - 1] == '/') f->dir[n - 1] = 0;
    }
    f->sel = 0 - 1; f->top = 0;
    reload(w);
}

static int list_y() { return BAR_H; }
static int list_h(int ch) { return ch - BAR_H - STATUS_H - 4; }
static int rows(int ch) { return (list_h(ch) - 4) / ROW; }

static void fm_draw(int w, int cw, int ch, int focused) {
    struct fm *f;
    int k, y, lw, d, home;
    char lab[8], num[32];
    f = F(w);
    // to the front: the list again, as another window may have changed it
    if (focused && !f->was_focused) { f->was_focused = 1; reload(w); }
    if (!focused) f->was_focused = 0;
    d_rect(0, 0, cw, ch, C_FACE);
    home = gd_home();
    k = 0;
    while (k < bar_n()) {
        d = bar_drive(k);
        ui_button(bar_x(k), 5, DRV_W, 22, drive_label(d, lab), d == f->drive, 0);
        ++k;
    }
    if (f->drive == home) d_text(bar_x(bar_n()) + 6, 10, C_DARK, F_SANS, "(files are saved here)");
    lw = cw - 2 * LX;
    d_rect(LX, list_y(), lw, list_h(ch), C_WHITE);
    d_bevel(LX, list_y(), lw, list_h(ch), B_SUNKEN);
    y = list_y() + 2;
    k = f->top;
    while (k < f->n && k < f->top + rows(ch)) {
        if (k == f->sel) d_rect(LX + 2, y, lw - 22, ROW, focused ? C_SEL : C_SHADOW);
        if (is_folder(f, k)) {
            // a folder, with its tab
            d_rect(LX + 5, y + 3, 5, 2, C_DARK);
            d_rect(LX + 5, y + 4, 12, 9, C_DARK);
            d_rect(LX + 6, y + 5, 10, 7, 0xffe080);
            ui_ncpy(f->msg, name_of(f, k), WK_PATH + 96);
            f->msg[ui_len(f->msg) - 1] = 0;
            d_text(LX + 22, y + 1, k == f->sel ? C_WHITE : C_DARK, F_SANS, f->msg);
        } else {
            // a page, folded at its corner
            d_rect(LX + 6, y + 2, 9, 12, C_DARK);
            d_rect(LX + 7, y + 3, 7, 10, C_WHITE);
            d_rect(LX + 11, y + 3, 3, 3, C_SHADOW);
            d_text(LX + 22, y + 1, k == f->sel ? C_WHITE : C_DARK, F_SANS, name_of(f, k));
        }
        y = y + ROW;
        ++k;
    }
    ui_scrollbar(LX + lw - 18, list_y() + 2, list_h(ch) - 4, f->n, rows(ch), f->top);
    // the status line: the file selected and its size, or how many
    if (f->sel >= 0 && f->size == 0 - 2) f->size = is_folder(f, f->sel) ? 0 - 1 : gd_size(fm_name(f, f->sel, f->path));
    f->msg[0] = 0;
    if (f->sel >= 0) {
        ui_ncpy(f->msg, f->path, WK_PATH + 96);
        if (f->size >= 0) {
            ui_ncat(f->msg, "   ", WK_PATH + 96);
            ui_ncat(f->msg, ui_num_commas(num, f->size), WK_PATH + 96);
            ui_ncat(f->msg, f->size == 1 ? " byte" : " bytes", WK_PATH + 96);
        }
    } else {
        // "A:\GAMES   5 files"
        ui_ncpy(f->msg, drive_label(f->drive, lab), WK_PATH + 96);
        ui_ncat(f->msg, "\\", WK_PATH + 96);
        k = ui_len(f->msg);
        ui_ncat(f->msg, f->dir, WK_PATH + 96);
        while (f->msg[k]) { if (f->msg[k] == '/') f->msg[k] = '\\'; ++k; }
        ui_ncat(f->msg, "   ", WK_PATH + 96);
        ui_ncat(f->msg, ui_num(num, f->n), WK_PATH + 96);
        ui_ncat(f->msg, f->n == 1 ? " file" : " files", WK_PATH + 96);
    }
    d_bevel(LX, ch - STATUS_H, cw - 2 * LX, STATUS_H - 2, B_SUNKEN);
    d_text(LX + 6, ch - STATUS_H + 3, C_DARK, F_SANS, f->msg);
}

static int ends(char *s, char *e) {
    int n, m, i;
    n = ui_len(s); m = ui_len(e);
    if (m > n) return 0;
    i = 0;
    while (i < m) { if ((s[n - m + i] | 32) != (e[i] | 32)) return 0; ++i; }
    return 1;
}
static int is_text(char *s) {
    return ends(s, ".txt") || ends(s, ".c") || ends(s, ".h") || ends(s, ".bat") || ends(s, ".ini") ||
           ends(s, ".cfg") || ends(s, ".md") || ends(s, ".doc") || ends(s, ".sys") || ends(s, ".dir") ||
           ends(s, ".frm") || ends(s, ".lst") || ends(s, ".json") || ends(s, ".s");
}

static char cmd[WK_PATH + 16];
static void edit_sel(int w) {
    struct fm *f;
    f = F(w);
    if (f->sel < 0 || is_folder(f, f->sel)) return;
    fm_name(f, f->sel, f->path);
    ui_cpy(cmd, "NOTEPAD ");
    ui_ncat(cmd, f->path, WK_PATH + 16);
    grin_run(cmd);
}
static void open_sel(int w) {
    struct fm *f;
    f = F(w);
    if (f->sel < 0) return;
    if (is_folder(f, f->sel)) { enter(w, f->sel); return; }
    fm_name(f, f->sel, f->path);
    if (ends(f->path, ".bat")) dosprompt_open(f->path);
    else if (is_text(f->path)) {
        ui_cpy(cmd, "NOTEPAD ");
        ui_ncat(cmd, f->path, WK_PATH + 16);
        grin_run(cmd);
    } else if (ends(f->path, ".c4r")) grin_start(f->path);
    else {
        ui_ncpy(f->msg, "No application opens ", WK_PATH + 96);
        ui_ncat(f->msg, f->path, WK_PATH + 96);
        ui_ncat(f->msg, ". Notepad opens text files (.TXT, .C, .INI), grin starts .C4R programs, and a DOS Prompt runs .BAT files.", WK_PATH + 96);
        wk_msgbox(w, 0, "File Manager", f->msg, WK_OK);
    }
}

static void fm_command(int w, int id) {
    struct fm *f;
    int home;
    f = F(w);
    if (id / WK_CMD_STEP == 1) {
        wk_msgbox(w, 0, "About File Manager", "File Manager: the files on C4DOS's drives and its RAM disk.", WK_OK);
        return;
    }
    id = id % WK_CMD_STEP;
    if (id == M_REFRESH) { reload(w); return; }
    if (id == M_EXIT) { wk_ask_close(w); return; }
    if (f->sel >= 0 && is_folder(f, f->sel) && id == M_OPEN) { enter(w, f->sel); return; }
    if (f->sel < 0 || is_folder(f, f->sel)) { if (id <= M_DELETE) wk_msgbox(w, 0, "File Manager", "Select a file first.", WK_OK); return; }
    fm_name(f, f->sel, f->path);
    if (id == M_OPEN) open_sel(w);
    else if (id == M_EDIT) edit_sel(w);
    else if (id == M_DELETE) {
        ui_ncpy(f->msg, "Delete ", WK_PATH + 96);
        ui_ncat(f->msg, f->path, WK_PATH + 96);
        ui_ncat(f->msg, "?", WK_PATH + 96);
        wk_msgbox(w, D_DELETE, "Confirm File Delete", f->msg, WK_YESNO);
    } else if (id == M_COPY) {
        // offered: the home drive, or from there the RAM disk
        home = gd_home();
        if (home >= 0 && home != f->drive) { f->ask[0] = 'A' + home; f->ask[1] = ':'; f->ask[2] = '\\'; f->ask[3] = 0; }
        else ui_cpy(f->ask, "RAM:");
        ui_ncpy(f->msg, "Copy ", WK_PATH + 96);
        ui_ncat(f->msg, f->path, WK_PATH + 96);
        ui_ncat(f->msg, " to:", WK_PATH + 96);
        wk_inputbox(w, D_COPY, "Copy", f->msg, f->ask, WK_PATH);
    }
}

static void fm_answer(int w, int dialog, int yes) {
    struct fm *f;
    f = F(w);
    if (!yes) return;
    if (dialog == D_DELETE) {
        if (gd_unlink(f->path) < 0) {
            ui_ncpy(f->msg, "Cannot delete ", WK_PATH + 96);
            ui_ncat(f->msg, f->path, WK_PATH + 96);
            ui_ncat(f->msg, ": its drive cannot be written.", WK_PATH + 96);
            wk_msgbox(w, 0, "File Manager", f->msg, WK_OK);
        }
        f->sel = 0 - 1;
        reload(w);
    } else if (dialog == D_COPY && f->ask[0]) {
        ui_cpy(cmd, "COPY ");
        ui_ncat(cmd, f->path, WK_PATH + 16);
        ui_ncat(cmd, " ", WK_PATH + 16);
        ui_ncat(cmd, f->ask, WK_PATH + 16);
        if (gd_exec(cmd) != 0) {
            ui_ncpy(f->msg, "Cannot copy ", WK_PATH + 96);
            ui_ncat(f->msg, f->path, WK_PATH + 96);
            ui_ncat(f->msg, " to ", WK_PATH + 96);
            ui_ncat(f->msg, f->ask, WK_PATH + 96);
            ui_ncat(f->msg, ".", WK_PATH + 96);
            wk_msgbox(w, 0, "File Manager", f->msg, WK_OK);
        }
        reload(w);
    }
}

static void select_row(int w, int k) {
    struct fm *f;
    f = F(w);
    if (k < 0 || k >= f->n) return;
    f->sel = k;
    f->size = 0 - 2;
}

static void fm_mouse(int w, int ev, int x, int y, int b) {
    struct fm *f;
    int cw, ch, k, t;
    f = F(w);
    wk_size(w, &cw, &ch);
    if (ev == E_WHEEL) {
        f->top = f->top + b * 3;
        if (f->top > f->n - rows(ch)) f->top = f->n - rows(ch);
        if (f->top < 0) f->top = 0;
        return;
    }
    if (ev == E_MOVE) { t = ui_scroll_drag(cw - LX - 18, list_y() + 2, y); if (t >= 0) f->top = t; return; }
    if (ev != E_DOWN && ev != E_DBL) return;
    if (y < BAR_H) {
        k = 0;
        while (k < bar_n()) { if (ui_hit(x, y, bar_x(k), 5, DRV_W, 22)) { choose(w, bar_drive(k)); return; } ++k; }
        return;
    }
    t = ui_scroll_click(x, y, cw - LX - 18, list_y() + 2, list_h(ch) - 4, f->n, rows(ch), f->top);
    if (t >= 0) { f->top = t; return; }
    if (ui_hit(x, y, LX, list_y() + 2, cw - 2 * LX - 20, rows(ch) * ROW)) {
        k = f->top + (y - list_y() - 2) / ROW;
        if (k >= f->n) { f->sel = 0 - 1; return; }
        select_row(w, k);
        if (ev == E_DBL) open_sel(w);
    }
}

static void fm_key(int w, int down, int code, int ch, int mods) {
    struct fm *f;
    int cw, chh, k;
    f = F(w);
    if (!down) return;
    wk_size(w, &cw, &chh);
    k = f->sel;
    if (code == KEY_ENTER) { open_sel(w); return; }
    if (code == KEY_DEL) { fm_command(w, WK_CMD(0, M_DELETE)); return; }
    if (code == KEY_F5) { reload(w); return; }
    if (!f->n) return;
    if (code == KEY_DOWN) k = k + 1;
    else if (code == KEY_UP) k = k - 1;
    else if (code == KEY_PGDN) k = k + rows(chh);
    else if (code == KEY_PGUP) k = k - rows(chh);
    else if (code == KEY_HOME) k = 0;
    else if (code == KEY_END) k = f->n - 1;
    else return;
    if (k < 0) k = 0;
    if (k >= f->n) k = f->n - 1;
    select_row(w, k);
    if (f->sel < f->top) f->top = f->sel;
    if (f->sel >= f->top + rows(chh)) f->top = f->sel - rows(chh) + 1;
}

static void fm_destroy(int w) {
    struct fm *f;
    f = F(w);
    if (f->names) { free(f->names); f->names = 0; }
}

static struct wk_class fm_class = {
    .kind = ICON_FILES, .draw = fm_draw, .mouse = fm_mouse, .key = fm_key, .command = fm_command,
    .answer = fm_answer, .destroy = fm_destroy
};

int filemgr_open() {
    int w, home;
    struct fm *f;
    if ((w = wk_find(&fm_class))) { wk_raise(w); return w; }
    fm_class.size = sizeof(struct fm);
    menus[0].title = "File"; menus[0].items = file_items;
    menus[1].title = "Help"; menus[1].items = help_items;
    if ((w = wk_open(&fm_class, "File Manager", 440, 320, 0, 0)) < 0) return w;
    wk_menu(w, menus, 2);
    f = F(w);
    f->names = (char *)malloc(FM_MAX * WK_NAME);
    home = gd_home();
    f->drive = home >= 0 ? home : 0;
    f->dir[0] = 0;
    f->sel = 0 - 1;
    reload(w);
    return w;
}
