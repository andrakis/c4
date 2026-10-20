// files.c -- Explorer. docs/c4ix-desktop.md.
//
// A folder tree beside a file list, over C4IX's VFS. Double-click opens:
// a folder shows its contents, a program (a .c4r, or anything C4R
// inside, which is how /bin's are named) runs in a new Command
// Prompt, a source file opens in the Editor, anything else in Notepad.
// New Folder, Delete (confirmed)
// and Rename go through the kernel's mkdir, unlink and rename.
//
// The tree scrolls on its own: a scroll bar (arrows, page clicks, a thumb
// to drag) and the wheel. It follows the selection only when that moves,
// not at every redraw, or the bar could never leave it. A folder with
// folders in it has a +/- box: click it, double-click the row, or use
// Left/Right or -/+ to collapse and expand. A collapsed folder stays
// collapsed through a reload (by the hash of its path), but the folders
// above the one being shown always open.

#include "desktop.h"

// NAMEW is the kernel's VN_NAME_MAX, TPW a tree path. Sized for the root
// archive's tree (some 70 folders, 80 files in the largest), and to fit
// ST_BYTES with room to spare.
enum { MAXE = 400, NAMEW = 64, MAXT = 256, TPW = 128, MAXDEPTH = 10, TREE_W = 180, ROW_H = 17, TOOL_H = 30, STATUS_H = 20 };
// a tree node's flags: it has folders under it, and it is collapsed
enum { T_KIDS = 1, T_COLL = 2, MAXCOLL = 64 };

struct files {
    char path[128];
    int n, sel, top;                   // the list: entries, selection, first row shown
    char name[MAXE * NAMEW];
    int isdir[MAXE], size[MAXE], prog[MAXE];
    int nt, tsel, ttop;                // the tree: folders, their depth, the one shown
    char tpath[MAXT * TPW];
    int tdepth[MAXT];
    char tflag[MAXT];                  // T_KIDS, T_COLL
    int tfollow;                       // scroll the tree to tsel at the next draw
    int tdrag, tdragy, tdragtop;       // dragging the tree's thumb: from where
    int pane;                          // 0 the list has the keyboard, 1 the tree
};

static struct files *F(struct win *w) { return (struct files *)w->st; }
static char tbuf[256];

static char *menu_titles[3];
static char *file_items[8];
static char *view_items[3];
static char *go_items[2];
static char **menu_items[3];

static char *ename(struct files *f, int i) { return f->name + i * NAMEW; }

static void join(char *out, char *dir, char *name) {
    ui_cpy(out, dir);
    if (!(out[0] == '/' && out[1] == 0)) ui_cat(out, "/");
    ui_cat(out, name);
}

static int before(char *a, char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (*a & 255) < (*b & 255);
}

// Read a directory: folders first, then files, each alphabetical.
static void load(struct files *f) {
    int i, j, r, s, d, st[4];
    char nm[NAMEW];
    f->n = 0;
    i = 0;
    while (f->n < MAXE) {
        r = ureaddir(f->path, i, nm);
        if (r < 0) break;
        ui_cpy(ename(f, f->n), nm);
        f->isdir[f->n] = r;
        join(tbuf, f->path, nm);
        st[2] = 0;
        f->size[f->n] = (!r && ustat(tbuf, st) == 0) ? st[1] : 0;
        f->prog[f->n] = !r && (st[2] || ui_ends(nm, ".c4r"));
        ++f->n;
        ++i;
    }
    // insertion sort
    i = 1;
    while (i < f->n) {
        j = i;
        while (j > 0) {
            if (f->isdir[j] > f->isdir[j - 1] ||
                (f->isdir[j] == f->isdir[j - 1] && before(ename(f, j), ename(f, j - 1)))) {
                ui_cpy(nm, ename(f, j)); ui_cpy(ename(f, j), ename(f, j - 1)); ui_cpy(ename(f, j - 1), nm);
                d = f->isdir[j]; f->isdir[j] = f->isdir[j - 1]; f->isdir[j - 1] = d;
                s = f->size[j]; f->size[j] = f->size[j - 1]; f->size[j - 1] = s;
                s = f->prog[j]; f->prog[j] = f->prog[j - 1]; f->prog[j - 1] = s;
                --j;
            } else break;
        }
        ++i;
    }
    if (f->sel >= f->n) f->sel = f->n - 1;
    if (f->sel < 0) f->sel = 0;
    if (f->top > f->sel) f->top = f->sel;
}

// Every folder, depth first, for the tree. DIR is copied in first thing,
// so a caller may pass a buffer the recursion will reuse.
static void tree_walk(struct files *f, char *dir, int depth) {
    int i, r, me;
    char nm[NAMEW];
    if (f->nt >= MAXT || depth > MAXDEPTH) return;
    me = f->nt;
    ui_cpy(f->tpath + me * TPW, dir);
    f->tdepth[me] = depth;
    f->tflag[me] = 0;
    ++f->nt;
    i = 0;
    while (1) {
        r = ureaddir(f->tpath + me * TPW, i, nm);
        if (r < 0) break;
        if (r == 1 && f->nt < MAXT) {
            join(tbuf, f->tpath + me * TPW, nm);
            tree_walk(f, tbuf, depth + 1);
        }
        ++i;
    }
    // a box only where a folder below was taken: not past MAXDEPTH or MAXT
    if (f->nt > me + 1) f->tflag[me] = T_KIDS;
}

static int phash(char *s) {
    int h;
    h = 5381;
    while (*s) { h = (h * 33 + (*s & 255)) & 0x7fffffff; ++s; }
    return h;
}

// The tree again, from the disk. What was collapsed stays so, but every
// folder above the one shown opens, so the selection can be seen.
static void tree_load(struct files *f) {
    int old[MAXCOLL], no, i, j, h, d;
    no = 0;
    i = 0;
    while (i < f->nt) {
        if ((f->tflag[i] & T_COLL) && no < MAXCOLL) { old[no] = phash(f->tpath + i * TPW); ++no; }
        ++i;
    }
    f->nt = 0;
    tree_walk(f, "/", 0);
    i = 0;
    while (i < f->nt && no) {
        h = phash(f->tpath + i * TPW);
        j = 0;
        while (j < no && old[j] != h) ++j;
        if (j < no) f->tflag[i] = f->tflag[i] | T_COLL;
        ++i;
    }
    f->tsel = 0;
    while (f->tsel < f->nt && !ui_eq(f->tpath + f->tsel * TPW, f->path)) ++f->tsel;
    if (f->tsel >= f->nt) f->tsel = 0;
    d = f->tdepth[f->tsel];
    i = f->tsel - 1;
    while (i >= 0 && d > 0) {
        if (f->tdepth[i] < d) { f->tflag[i] = f->tflag[i] & T_KIDS; d = f->tdepth[i]; }
        --i;
    }
    f->tfollow = 1;
}

// The node shown after node I (itself shown): a collapsed one hides
// everything deeper that follows it.
static int t_next(struct files *f, int i) {
    int d;
    if (!(f->tflag[i] & T_COLL)) return i + 1;
    d = f->tdepth[i];
    ++i;
    while (i < f->nt && f->tdepth[i] > d) ++i;
    return i;
}

// how many nodes are shown, and the row node I is on (-1 if hidden)
static int t_shown(struct files *f) {
    int i, n;
    i = 0; n = 0;
    while (i < f->nt) { ++n; i = t_next(f, i); }
    return n;
}
static int t_row(struct files *f, int node) {
    int i, n;
    i = 0; n = 0;
    while (i < f->nt) { if (i == node) return n; ++n; i = t_next(f, i); }
    return 0 - 1;
}
// the node on shown row R, or -1
static int t_at(struct files *f, int r) {
    int i;
    i = 0;
    while (i < f->nt && r > 0) { i = t_next(f, i); --r; }
    return (r == 0 && i < f->nt) ? i : 0 - 1;
}

// the tree's rows, and its scroll bar's place, in client coordinates
static int t_rows(int ch) { return (ch - TOOL_H - STATUS_H - 2 - 6) / ROW_H; }
static int t_clamp(struct files *f, int rows) {
    int max;
    max = t_shown(f) - rows;
    if (max < 0) max = 0;
    if (f->ttop > max) f->ttop = max;
    if (f->ttop < 0) f->ttop = 0;
    return max;
}
enum { TSB_X = 2 + TREE_W - 18 };

static void go(struct win *w, char *path) {
    struct files *f;
    f = F(w);
    if (path != f->path) ui_cpy(f->path, path);
    f->sel = 0; f->top = 0;
    load(f);
    tree_load(f);
    ui_cpy(tbuf, "Exploring - ");
    ui_cat(tbuf, f->path);
    win_title(w, tbuf);
}

static void up(struct win *w) {
    struct files *f;
    int k;
    f = F(w);
    k = ui_len(f->path);
    while (k > 0 && f->path[k] != '/') --k;
    if (k == 0) f->path[1] = 0; else f->path[k] = 0;
    go(w, f->path);
}

// show tree node I's folder, keeping the keyboard in the tree
static void t_select(struct win *w, int node) {
    struct files *f;
    f = F(w);
    if (node < 0 || node >= f->nt) return;
    ui_cpy(tbuf, f->tpath + node * TPW);
    go(w, tbuf);
    f->pane = 1;
}

// Collapse or expand node I. A selection it hides moves to it, as a
// collapsed folder is where the selection was going to be seen.
static void t_toggle(struct win *w, int i) {
    struct files *f;
    int d, j;
    f = F(w);
    if (i < 0 || i >= f->nt || !(f->tflag[i] & T_KIDS)) return;
    f->tflag[i] = f->tflag[i] ^ T_COLL;
    if (!(f->tflag[i] & T_COLL)) return;
    d = f->tdepth[i];
    j = i + 1;
    while (j < f->nt && f->tdepth[j] > d) { if (j == f->tsel) { t_select(w, i); return; } ++j; }
}

static char *type_of(char *name, int dir, int prog) {
    if (dir) return "File Folder";
    if (prog) return "C4IX Program";
    if (ui_ends(name, ".c")) return "C Source";
    if (ui_ends(name, ".h")) return "C Header";
    if (ui_ends(name, ".txt")) return "Text Document";
    if (ui_ends(name, ".sh")) return "Shell Script";
    if (ui_ends(name, ".lisp")) return "Lisp Source";
    if (ui_ends(name, ".f")) return "Forth Source";
    return "File";
}

// what opens in the Editor rather than Notepad
static int is_source(char *p) {
    return ui_ends(p, ".c") || ui_ends(p, ".h") || ui_ends(p, ".b4k") || ui_ends(p, ".B4K") ||
           ui_ends(p, ".lisp") || ui_ends(p, ".f") || ui_ends(p, ".js") || ui_ends(p, ".mjs");
}

static void open_sel(struct win *w) {
    struct files *f;
    f = F(w);
    if (f->sel < 0 || f->sel >= f->n) return;
    join(tbuf, f->path, ename(f, f->sel));
    if (f->isdir[f->sel]) go(w, tbuf);
    else if (f->prog[f->sel]) run_command(tbuf);
    else if (is_source(tbuf)) edit_open(tbuf);
    else note_open(tbuf);
}

int files_open(char *path) {
    int i;
    struct win *w;
    i = win_open(K_FILES, "Exploring", 600, 420);
    if (i < 0) return i;
    w = win_get(i);
    menu_titles[0] = "File"; menu_titles[1] = "View"; menu_titles[2] = "Go";
    file_items[0] = "Open"; file_items[1] = "New Folder"; file_items[2] = "Delete";
    file_items[3] = "Rename"; file_items[4] = "-"; file_items[5] = "Close"; file_items[6] = 0;
    view_items[0] = "Refresh"; view_items[1] = 0;
    go_items[0] = "Up One Level"; go_items[1] = 0;
    menu_items[0] = file_items; menu_items[1] = view_items; menu_items[2] = go_items;
    win_menus(w, 3, menu_titles, menu_items);
    go(w, path);
    return i;
}

// ---- drawing --------------------------------------------------------------------

static void small_icon(int dir, int prog, int x, int y) {
    if (dir) {
        d_rect(x, y + 2, 6, 2, 0xc8a000); d_rect(x, y + 4, 14, 9, 0xffd84a); d_recto(x, y + 4, 14, 9, 0x806000);
    } else if (prog) {
        d_rect(x, y + 1, 14, 12, C_WHITE); d_recto(x, y + 1, 14, 12, C_DARK); d_rect(x + 1, y + 2, 12, 3, C_TITLE);
    } else {
        d_rect(x + 2, y, 10, 14, C_WHITE); d_recto(x + 2, y, 10, 14, C_SHADOW);
        d_rect(x + 4, y + 4, 6, 1, C_SHADOW); d_rect(x + 4, y + 7, 6, 1, C_SHADOW);
    }
}

static int widths[3];
static char *labels[3];

// the +/- box on a folder with folders in it, centred where its lines meet
static void t_box(int x, int y, int coll) {
    d_rect(x, y, 9, 9, C_WHITE);
    d_recto(x, y, 9, 9, C_SHADOW);
    d_rect(x + 2, y + 4, 5, 1, C_DARK);
    if (coll) d_rect(x + 4, y + 2, 1, 5, C_DARK);
}

void files_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    struct files *f;
    int i, y, lx, lw, lh, rows, tx, th, trows, sel_bg, nv, r;
    f = F(w);
    d_rect(ox, oy, cw, ch, C_FACE);
    // the toolbar: Up, and the address
    ui_button(ox + 4, oy + 4, 26, 22, "", 0, 0);
    d_rect(ox + 10, oy + 9, 6, 2, 0xc8a000); d_rect(ox + 10, oy + 11, 14, 10, 0xffd84a);
    d_line(ox + 17, oy + 13, ox + 17, oy + 19, C_DARK); d_line(ox + 14, oy + 16, ox + 17, oy + 13, C_DARK); d_line(ox + 20, oy + 16, ox + 17, oy + 13, C_DARK);
    d_text(ox + 38, oy + 9, C_DARK, F_SANS, "Address");
    ui_field(ox + 86, oy + 4, cw - 90, f->path, 0, 0);
    // the tree
    tx = ox + 2; th = ch - TOOL_H - STATUS_H - 2;
    d_rect(tx, oy + TOOL_H, TREE_W, th, C_WHITE);
    d_bevel(tx, oy + TOOL_H, TREE_W, th, B_SUNKEN);
    trows = t_rows(ch);
    nv = t_shown(f);
    if (f->tfollow) {
        r = t_row(f, f->tsel);
        if (r >= 0 && r < f->ttop) f->ttop = r;
        if (r >= f->ttop + trows) f->ttop = r - trows + 1;
        f->tfollow = 0;
    }
    t_clamp(f, trows);
    i = t_at(f, f->ttop);
    r = 0;
    y = oy + TOOL_H + 3;
    while (i >= 0 && i < f->nt && r < trows) {
        lx = tx + 18 + f->tdepth[i] * 14;
        if (f->tdepth[i] > 0) {
            d_rect(lx - 9, y + 8, 7, 1, C_SHADOW);
            d_rect(lx - 9, y - 2, 1, 10, C_SHADOW);
        }
        if (f->tflag[i] & T_KIDS) t_box(lx - 13, y + 4, f->tflag[i] & T_COLL);
        small_icon(1, 0, lx, y + 1);
        tbuf[0] = 0;
        if (i == 0) ui_cpy(tbuf, "C4IX (/)");
        else {
            lw = ui_len(f->tpath + i * TPW);
            while (lw > 0 && f->tpath[i * TPW + lw - 1] != '/') --lw;
            ui_cpy(tbuf, f->tpath + i * TPW + lw);
        }
        sel_bg = i == f->tsel;
        if (sel_bg) d_rect(lx + 17, y, ui_len(tbuf) * 6 + 6, ROW_H - 1, f->pane == 1 && focused ? C_SEL : C_FACE);
        d_text(lx + 20, y + 2, sel_bg && f->pane == 1 && focused ? C_WHITE : C_DARK, F_SANS, tbuf);
        y = y + ROW_H;
        ++r;
        i = t_next(f, i);
    }
    // after the names, so a long one runs under the bar, not over it
    if (nv > trows) ui_scrollbar(ox + TSB_X, oy + TOOL_H + 2, th - 4, nv, trows, f->ttop);
    // the list
    lx = tx + TREE_W + 4; lw = cw - TREE_W - 8;
    d_rect(lx, oy + TOOL_H, lw, th, C_WHITE);
    d_bevel(lx, oy + TOOL_H, lw, th, B_SUNKEN);
    widths[0] = lw - 4 - 16 - 170; widths[1] = 70; widths[2] = 100;
    labels[0] = "Name"; labels[1] = "Size"; labels[2] = "Type";
    ui_header(lx + 2, oy + TOOL_H + 2, widths, labels, 3);
    lh = th - 22;
    rows = (lh - 4) / ROW_H;
    if (f->sel < f->top) f->top = f->sel;
    if (f->sel >= f->top + rows) f->top = f->sel - rows + 1;
    i = f->top;
    y = oy + TOOL_H + 22;
    while (i < f->n && i < f->top + rows) {
        small_icon(f->isdir[i], f->prog[i], lx + 6, y + 1);
        sel_bg = i == f->sel;
        if (sel_bg) d_rect(lx + 23, y, ui_len(ename(f, i)) * 6 + 6, ROW_H - 1, f->pane == 0 && focused ? C_SEL : C_FACE);
        d_text(lx + 26, y + 2, sel_bg && f->pane == 0 && focused ? C_WHITE : C_DARK, F_SANS, ename(f, i));
        if (!f->isdir[i]) {
            ui_num_commas(tbuf, (f->size[i] + 1023) / 1024);
            ui_cat(tbuf, " KB");
            d_text(lx + 2 + widths[0] + widths[1] - ui_len(tbuf) * 6 - 8, y + 2, C_DARK, F_SANS, tbuf);
        }
        d_text(lx + 2 + widths[0] + widths[1] + 6, y + 2, C_DARK, F_SANS, type_of(ename(f, i), f->isdir[i], f->prog[i]));
        y = y + ROW_H;
        ++i;
    }
    ui_scrollbar(lx + lw - 18, oy + TOOL_H + 20, lh - 2, f->n, rows, f->top);
    // the status bar
    d_rect(ox, oy + ch - STATUS_H, cw, STATUS_H, C_FACE);
    d_bevel(ox + 2, oy + ch - STATUS_H + 2, cw - 4, STATUS_H - 3, B_SUNKEN);
    ui_num(tbuf, f->n);
    ui_cat(tbuf, f->n == 1 ? " object" : " object(s)");
    d_text(ox + 8, oy + ch - STATUS_H + 5, C_DARK, F_SANS, tbuf);
}

// ---- input ------------------------------------------------------------------------------

void files_mouse(struct win *w, int ev, int x, int y, int cw, int ch) {
    struct files *f;
    int th, lx, lw, rows, row, t, lh, trows, nv, track, thumb, ty, node, bx;
    f = F(w);
    th = ch - TOOL_H - STATUS_H - 2;
    lx = 2 + TREE_W + 4; lw = cw - TREE_W - 8;
    lh = th - 22;
    rows = (lh - 4) / ROW_H;
    trows = t_rows(ch);
    nv = t_shown(f);
    track = th - 4 - 32;
    thumb = nv > trows ? (track * trows) / nv : track;
    if (thumb < 10) thumb = 10;
    if (ev == E_UP) { f->tdrag = 0; return; }
    if (ev == E_MOVE) {
        if (f->tdrag && nv > trows && track > thumb) {
            f->ttop = f->tdragtop + ((y - f->tdragy) * (nv - trows)) / (track - thumb);
            t_clamp(f, trows);
        }
        return;
    }
    if (ev == E_WHEEL && x < 2 + TREE_W) {
        f->ttop = f->ttop + y * 3;
        t_clamp(f, trows);
        return;
    }
    if (ev == E_WHEEL) {
        // y is the wheel's direction here
        f->top = f->top + y * 3;
        if (f->top > f->n - rows) f->top = f->n - rows;
        if (f->top < 0) f->top = 0;
        if (f->sel < f->top) f->sel = f->top;
        if (f->sel >= f->top + rows) f->sel = f->top + rows - 1;
        return;
    }
    if (ev != E_DOWN && ev != E_DBL) return;
    if (ui_hit(x, y, 4, 4, 26, 22)) { if (ev == E_DOWN) up(w); return; }
    if (nv > trows && ui_hit(x, y, TSB_X, TOOL_H + 2, 16, th - 4)) {
        if (ev != E_DOWN) return;
        t = ui_scroll_click(x, y, TSB_X, TOOL_H + 2, th - 4, nv, trows, f->ttop);
        ty = TOOL_H + 2 + 16 + ((track - thumb) * f->ttop) / (nv - trows);
        if (t == f->ttop && y >= ty && y < ty + thumb) { f->tdrag = 1; f->tdragy = y; f->tdragtop = f->ttop; }
        if (t >= 0) f->ttop = t;
        return;
    }
    if (ui_hit(x, y, 2, TOOL_H, TREE_W, th)) {
        f->pane = 1;
        node = t_at(f, f->ttop + (y - TOOL_H - 3) / ROW_H);
        if (node < 0) return;
        bx = 2 + 18 + f->tdepth[node] * 14 - 13;
        if (x >= bx - 2 && x < bx + 11 && (f->tflag[node] & T_KIDS)) { if (ev == E_DOWN) t_toggle(w, node); return; }
        if (ev == E_DOWN) t_select(w, node);
        else t_toggle(w, node);
        return;
    }
    t = ui_scroll_click(x, y, lx + lw - 18, TOOL_H + 20, lh - 2, f->n, rows, f->top);
    if (t >= 0) { f->top = t; if (f->sel < f->top) f->sel = f->top; if (f->sel >= f->top + rows) f->sel = f->top + rows - 1; return; }
    if (ui_hit(x, y, lx, TOOL_H + 22, lw, lh)) {
        f->pane = 0;
        row = f->top + (y - TOOL_H - 22) / ROW_H;
        if (row >= 0 && row < f->n) {
            f->sel = row;
            if (ev == E_DBL) open_sel(w);
        }
    }
}

void files_key(struct win *w, int code, int ch, int mods) {
    struct files *f;
    int r, i;
    f = F(w);
    if (code == KEY_TAB) { f->pane = !f->pane; return; }
    // these mean the same whichever pane has the keyboard
    if (code == KEY_F5) { files_command(w, 1, 0); return; }
    if (code == KEY_BACK) { up(w); return; }
    if (f->pane == 1) {
        r = t_row(f, f->tsel);
        if (code == KEY_UP && r > 0) t_select(w, t_at(f, r - 1));
        else if (code == KEY_DOWN && r >= 0 && r < t_shown(f) - 1) t_select(w, t_at(f, r + 1));
        else if (code == KEY_HOME) t_select(w, 0);
        else if (code == KEY_END) t_select(w, t_at(f, t_shown(f) - 1));
        else if (code == KEY_LEFT || ch == '-') {
            // collapse; on one already collapsed (or with none), its parent
            if ((f->tflag[f->tsel] & T_KIDS) && !(f->tflag[f->tsel] & T_COLL)) t_toggle(w, f->tsel);
            else if (code == KEY_LEFT && f->tsel > 0) {
                i = f->tsel - 1;
                while (i > 0 && f->tdepth[i] >= f->tdepth[f->tsel]) --i;
                t_select(w, i);
            }
        }
        else if (code == KEY_RIGHT || ch == '+') {
            // expand; on one already open, its first folder
            if (f->tflag[f->tsel] & T_COLL) t_toggle(w, f->tsel);
            else if (code == KEY_RIGHT && (f->tflag[f->tsel] & T_KIDS) && f->tsel + 1 < f->nt) t_select(w, f->tsel + 1);
        }
        return;
    }
    if (code == KEY_UP && f->sel > 0) --f->sel;
    else if (code == KEY_DOWN && f->sel < f->n - 1) ++f->sel;
    else if (code == KEY_HOME) f->sel = 0;
    else if (code == KEY_END) f->sel = f->n - 1;
    else if (code == KEY_PGUP) { f->sel = f->sel - 10; if (f->sel < 0) f->sel = 0; }
    else if (code == KEY_PGDN) { f->sel = f->sel + 10; if (f->sel >= f->n) f->sel = f->n - 1; }
    else if (code == KEY_ENTER) open_sel(w);
    else if (code == KEY_BACK) up(w);
    else if (code == KEY_DEL) files_command(w, 0, 2);
    else if (code == KEY_F2) files_command(w, 0, 3);
    else if (code == KEY_F5) files_command(w, 1, 0);
}

void files_command(struct win *w, int menu, int item) {
    struct files *f;
    f = F(w);
    if (menu == 0) {
        if (item == 0) open_sel(w);
        else if (item == 1) inputbox(w, "New Folder", "Name of the new folder:", "New Folder", A_NEWDIR);
        else if (item == 2 && f->n) {
            ui_cpy(tbuf, "Are you sure you want to delete '");
            ui_cat(tbuf, ename(f, f->sel));
            ui_cat(tbuf, "'?");
            msgbox(w, f->isdir[f->sel] ? "Confirm Folder Delete" : "Confirm File Delete", tbuf, 1, A_DELETE);
        }
        else if (item == 3 && f->n) inputbox(w, "Rename", "New name:", ename(f, f->sel), A_RENAME);
        else if (item == 5) win_close(win_index(w));
    } else if (menu == 1) { load(f); tree_load(f); }
    else if (menu == 2) up(w);
}

void files_answer(struct win *w, int action, int yes) {
    struct files *f;
    char path[160];
    f = F(w);
    if (!yes) return;
    if (action == A_DELETE && f->n) {
        join(path, f->path, ename(f, f->sel));
        if (uunlink(path) < 0)
            msgbox(w, "Error Deleting", f->isdir[f->sel] ? "Cannot delete: the folder is not empty." : "Cannot delete: the file is in use or is part of the disk.", 0, A_NONE);
        load(f); tree_load(f);
    } else if (action == A_RENAME && f->n) {
        join(path, f->path, ename(f, f->sel));
        if (urename(path, dialog_text()) < 0) msgbox(w, "Error Renaming", "Cannot rename: that name is taken or not allowed.", 0, A_NONE);
        load(f); tree_load(f);
    } else if (action == A_NEWDIR) {
        join(path, f->path, dialog_text());
        if (umkdir(path) < 0) msgbox(w, "Error", "Cannot create that folder.", 0, A_NONE);
        load(f); tree_load(f);
    }
}
