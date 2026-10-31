// notepad.c -- Notepad: a plain text editor, as a window kit class.
// docs/grin-plan.md 4; it was the C4IX desktop's Text Editor (apps.c).
//
// The whole file in one buffer, edited in place, which grows as it must:
// a file is as big as memory allows, and one that will not fit is refused
// rather than cut short (the desktop review's B6). Lines are found by
// scanning for newlines, and drawn in a fixed-width face so the caret can
// be placed exactly. Files open from, and save to, the host's (wk_fopen).
//
// A selection runs from an anchor to the caret: Shift with the moving
// keys, or the mouse dragged, makes one, and typing replaces it. Edit's
// Cut, Copy and Paste (and Ctrl+X, C and V, or Shift+Del, Ctrl+Ins and
// Shift+Ins) go through the kit's clipboard, which every window shares.
//
// Format: Word Wrap, on until it is turned off, folds a long line onto the
// rows below; off, the text scrolls across under a bar. And the face and
// size the text is drawn in, the kit's fixed-width ones. The choices are
// every Notepad window's, and kept in notepad_cfg when the host names one.

#include "winkit.h"

enum { CHUNK = 4096 };
enum { D_OPEN = 1, D_SAVEAS = 2 };                // its dialogs
enum { N_NEW = 0, N_OPEN = 1, N_SAVE = 2, N_SAVEAS = 3, N_EXIT = 5 };   // File's items
enum { E_CUT = 0, E_COPY = 1, E_PASTE = 2, E_DELETE = 3, E_ALL = 5 };    // Edit's
enum { FM_WRAP = 0, FM_FACE = 2, FM_SIZE = 6, FM_N = 9 };                // Format's: then the faces, the sizes

char *notepad_name = "Notepad";
char *notepad_about = "Notepad: edits plain text.";
char *notepad_home = "/";                         // where Open starts
char *notepad_untitled = "untitled.txt";          // where Save As offers to put a new one
int notepad_filebox = 0;                          // Open and Save As: the kit's file box, not a typed path
char *notepad_filter = "*";                       // ... and the types it offers (wk_filebox)
char *notepad_cfg = 0;                            // where Format's choices are kept, or 0

struct note {
    char path[WK_PATH];
    char ask[WK_PATH];                            // a dialog's path, in and out
    int len, cap, caret, top, modified;           // top: the first row shown
    int want_col;                                 // the column up/down tries to keep
    int anchor;                                   // the selection's other end (anchor == caret: none)
    int dragging;                                 // the mouse is making a selection
    char *text;                                   // cap bytes, len of them text and a 0
    int left;                                     // without Word Wrap: the first column shown
    int lcaret;                                   // the caret when it last drew (the view follows it when it moves)
    int rows, cols, maxw;                         // as it last drew: rows and columns shown, the longest line
    int lwrap;                                    // ... and whether it folded
};

// Format's choices: Word Wrap, the face and the size; the text grid they make
static int wrap = 1;
static int nface;
static int nsize = 1;
static int cfg_read;
static int nfont = F_MONO;
static int nfs = 12;
static int ncw = 7;
static int nch = 15;

static char buf[WK_PATH + 64];
static char about_item[64];
static char about_title[64];
static char *file_items[7] = { "New", "Open...", "Save", "Save As...", "-", "Exit", 0 };
static char *edit_items[7] = { "Cut", "Copy", "Paste", "Delete", "-", "Select All", 0 };
static char *format_items[FM_N + 1];
static char fmt_buf[FM_N * 20];
static char *help_items[2];
static struct wk_menu menus[4];
static char cfgb[72];

static struct note *N(int w) { return (struct note *)wk_state(w); }

static void retitle(int w) {
    struct note *n;
    n = N(w);
    if (n->path[0]) ui_ncpy(buf, ui_base(n->path), WK_PATH);
    else ui_cpy(buf, "Untitled");
    if (n->modified) ui_cat(buf, " *");
    ui_cat(buf, " - ");
    ui_ncat(buf, notepad_name, WK_PATH + 64);
    wk_title(w, buf);
}

// Room for `more` bytes past len, and the 0 after them: 1 if there is.
static int room(struct note *n, int more) {
    char *t;
    int cap;
    if (n->len + more < n->cap) return 1;
    cap = n->cap ? n->cap : CHUNK;
    while (n->len + more >= cap) cap = cap * 2;
    if (!(t = wk_alloc(cap))) return 0;
    if (n->text) { memcpy(t, n->text, n->len + 1); wk_free(n->text); }
    n->text = t; n->cap = cap;
    return 1;
}

static void empty(struct note *n) {
    n->len = 0; n->caret = 0; n->anchor = 0; n->top = 0; n->modified = 0; n->left = 0;
    if (n->text) n->text[0] = 0;
}

// 1 loaded; 0 cannot open it; -1 no memory to hold it all (the buffer is then empty)
static int load(struct note *n, char *path) {
    int fd, r;
    if ((fd = wk_fopen(path, 0)) < 0) return 0;
    empty(n);
    while (1) {
        if (!room(n, CHUNK)) { wk_fclose(fd); empty(n); return 0 - 1; }
        r = wk_fread(fd, n->text + n->len, n->cap - 1 - n->len);
        if (r <= 0) break;
        n->len = n->len + r;
    }
    wk_fclose(fd);
    n->text[n->len] = 0;
    ui_ncpy(n->path, path, WK_PATH);
    return 1;
}

static int save(struct note *n) {
    int fd, r;
    if ((fd = wk_fopen(n->path, 1)) < 0) return 0;
    r = n->len ? wk_fwrite(fd, n->text, n->len) : 0;
    wk_fclose(fd);
    if (r != n->len) return 0;
    n->modified = 0;
    return 1;
}

static void say(int w, char *s) { wk_msgbox(w, 0, notepad_name, s, WK_OK); }

static void open_file(int w, char *path, int fresh) {
    int r;
    r = load(N(w), path);
    if (r < 0) say(w, "There is not enough memory to open all of that file.");
    else if (!r && fresh) { ui_ncpy(N(w)->path, path, WK_PATH); say(w, "Cannot open that file; starting a new one with its name."); }
    else if (!r) say(w, "Cannot open that file.");
}

// ---- Format's choices ------------------------------------------------------------

static void use_font() {
    nfont = wk_mono_font(nface);
    wk_grid(nsize, &nfs, &ncw, &nch);
}

// notepad_cfg: "wrap W font F size S", each a digit
static void cfg_load() {
    int fd, r, i;
    cfg_read = 1;
    if (notepad_cfg && (fd = wk_fopen(notepad_cfg, 0)) >= 0) {
        r = wk_fread(fd, cfgb, 60);
        wk_fclose(fd);
        if (r < 0) r = 0;
        cfgb[r] = 0;
        i = 0;
        while (i < r) {
            if (cfgb[i] == 'w' && cfgb[i + 1] == 'r' && cfgb[i + 4] == ' ') wrap = cfgb[i + 5] != '0';
            if (cfgb[i] == 'f' && cfgb[i + 1] == 'o' && cfgb[i + 4] == ' ' && cfgb[i + 5] >= '0' && cfgb[i + 5] < '0' + WK_FACES) nface = cfgb[i + 5] - '0';
            if (cfgb[i] == 's' && cfgb[i + 1] == 'i' && cfgb[i + 4] == ' ' && cfgb[i + 5] >= '0' && cfgb[i + 5] < '0' + WK_SIZES) nsize = cfgb[i + 5] - '0';
            ++i;
        }
    }
    use_font();
}

static void cfg_save() {
    int fd;
    if (!notepad_cfg || (fd = wk_fopen(notepad_cfg, 1)) < 0) return;
    ui_cpy(cfgb, "wrap 0 font 0 size 0\n");
    cfgb[5] = '0' + wrap; cfgb[12] = '0' + nface; cfgb[19] = '0' + nsize;
    wk_fwrite(fd, cfgb, ui_len(cfgb));
    wk_fclose(fd);
}

// Format's items, a check beside each choice in use
static void fmt_item(int k, char *name, int on) {
    char *p;
    p = fmt_buf + k * 20;
    p[0] = WK_CHECKED;
    ui_ncpy(on ? p + 1 : p, name, 19);
    format_items[k] = p;
}
static void format_menu() {
    int k;
    fmt_item(FM_WRAP, "Word Wrap", wrap);
    format_items[FM_FACE - 1] = "-";
    k = 0;
    while (k < WK_FACES) { fmt_item(FM_FACE + k, wk_mono_name(k), k == nface); ++k; }
    format_items[FM_SIZE - 1] = "-";
    k = 0;
    while (k < WK_SIZES) { fmt_item(FM_SIZE + k, wk_size_name(k), k == nsize); ++k; }
    format_items[FM_N] = 0;
}

// a choice made: in every Notepad window at once, and kept
static void format_pick(int item) {
    if (item == FM_WRAP) wrap = !wrap;
    else if (item >= FM_FACE && item < FM_FACE + WK_FACES) nface = item - FM_FACE;
    else if (item >= FM_SIZE && item < FM_SIZE + WK_SIZES) nsize = item - FM_SIZE;
    else return;
    use_font();
    format_menu();
    cfg_save();
    wk_redraw_all();
}

static void note_open_hook(int w, char *path) {
    struct note *n;
    n = N(w);
    help_items[0] = about_item; help_items[1] = 0;
    format_menu();
    menus[0].title = "File"; menus[0].items = file_items;
    menus[1].title = "Edit"; menus[1].items = edit_items;
    menus[2].title = "Format"; menus[2].items = format_items;
    menus[3].title = "Help"; menus[3].items = help_items;
    wk_menu(w, menus, 4);
    if (!room(n, 1)) { wk_close(w); return; }
    empty(n);
    if (path) open_file(w, path, 1);
    retitle(w);
}

static void note_destroy(int w) {
    struct note *n;
    n = N(w);
    if (n->text) { wk_free(n->text); n->text = 0; }
}

int notepad_open(char *path) {
    if (!cfg_read) cfg_load();
    ui_cpy(about_item, "About ");
    ui_ncat(about_item, notepad_name, 64);
    ui_cpy(about_title, about_item);
    ui_cpy(buf, "Untitled - ");
    ui_ncat(buf, notepad_name, WK_PATH + 64);
    notepad_class.size = sizeof(struct note);
    return wk_open(&notepad_class, buf, 560, 400, 0, path);
}

// ---- rows: the lines as they are drawn --------------------------------------------
//
// Without Word Wrap a row is a line. With it, a line wider than the
// window folds onto the rows below: after the last space that fits (a
// space just past the edge is the fold itself, and not drawn), or where
// the row is full if it has none. A row is known by where it starts; the
// next starts where it ends, or past the newline or space it ended at. A
// position is on the row it is at or after whose next is past it: the end
// of a line is on its last row, a fold's position on the row after.

static int wcols;      // a row's columns while folding (the window's, as it last drew); 0 not folding

static void lay(struct note *n) { wcols = wrap ? n->cols : 0; }

// where the row from S ends, and where the next starts (past the text: len + 1)
static int row_end(struct note *n, int s, int *next) {
    int e, k;
    e = s;
    while (e < n->len && n->text[e] != 10 && (!wcols || e < s + wcols)) ++e;
    if (e >= n->len || n->text[e] == 10 || n->text[e] == ' ') { *next = e + 1; return e; }
    k = e;
    while (k > s && n->text[k - 1] != ' ') --k;
    if (k > s) e = k;
    *next = e;
    return e;
}

// the row a buffer position is on, and its column there
static int row_of(struct note *n, int pos, int *col) {
    int s, next, row;
    s = 0; row = 0;
    while (1) {
        row_end(n, s, &next);
        if (pos < next || next > n->len) { *col = pos - s; return row; }
        s = next; ++row;
    }
    return row;
}

// where row R starts (the last row's start, past the last)
static int row_start(struct note *n, int r) {
    int s, next;
    s = 0;
    while (r > 0) {
        row_end(n, s, &next);
        if (next > n->len) return s;
        s = next; --r;
    }
    return s;
}

// the buffer position at a column of row R, the nearest the row has
static int row_pos(struct note *n, int r, int col) {
    int s, e, next;
    s = row_start(n, r);
    e = row_end(n, s, &next);
    if (col < 0) col = 0;
    if (s + col > e) col = e - s;
    if (s + col >= next) col = next - 1 - s;     // folded where it was full: its last character
    return s + col;
}

static int row_count(struct note *n) {
    int s, next, r;
    s = 0; r = 1;
    while (1) {
        row_end(n, s, &next);
        if (next > n->len) return r;
        s = next; ++r;
    }
    return r;
}

// the longest line, which the bar across scrolls over
static int widest(struct note *n) {
    int i, start, w;
    w = 0; start = 0; i = 0;
    while (i <= n->len) {
        if (i == n->len || n->text[i] == 10) { if (i - start > w) w = i - start; start = i + 1; }
        ++i;
    }
    return w;
}

static void insert(struct note *n, int c) {
    int i;
    if (!room(n, 1)) return;
    i = n->len;
    while (i > n->caret) { n->text[i] = n->text[i - 1]; --i; }
    n->text[n->caret] = c;
    ++n->len; ++n->caret;
    n->text[n->len] = 0;
    n->modified = 1;
}

static void erase(struct note *n, int at) {
    int i;
    if (at < 0 || at >= n->len) return;
    i = at;
    while (i < n->len) { n->text[i] = n->text[i + 1]; ++i; }
    --n->len;
    n->modified = 1;
}

// ---- the selection, and the clipboard ------------------------------------------

static int sel0(struct note *n) { return n->anchor < n->caret ? n->anchor : n->caret; }
static int sel1(struct note *n) { return n->anchor < n->caret ? n->caret : n->anchor; }
static int has_sel(struct note *n) { return n->anchor != n->caret && n->anchor >= 0 && n->anchor <= n->len; }

// the selection taken out of the text: 1 if there was one
static int cut_sel(struct note *n) {
    int a, b, i;
    if (!has_sel(n)) { n->anchor = n->caret; return 0; }
    a = sel0(n); b = sel1(n);
    i = a;
    while (i + (b - a) <= n->len) { n->text[i] = n->text[i + (b - a)]; ++i; }
    n->len = n->len - (b - a);
    n->caret = a; n->anchor = a;
    n->modified = 1;
    return 1;
}

static void copy_sel(struct note *n) { if (has_sel(n)) wk_clip_set(n->text + sel0(n), sel1(n) - sel0(n)); }

// the clipboard's text at the caret, in place of the selection (its
// carriage returns dropped: a line ends in a newline here)
static void paste(struct note *n) {
    char *c;
    int cn, k, m, i;
    c = wk_clip_get(&cn);
    if (cn <= 0) return;
    if (!room(n, cn)) return;
    cut_sel(n);
    m = 0;
    k = 0;
    while (k < cn) { if (c[k] != 13 && c[k]) ++m; ++k; }
    i = n->len;
    while (i >= n->caret) { n->text[i + m] = n->text[i]; --i; }
    k = 0;
    while (k < cn) { if (c[k] != 13 && c[k]) { n->text[n->caret] = c[k]; ++n->caret; } ++k; }
    n->len = n->len + m;
    n->anchor = n->caret;
    n->modified = 1;
}

static void note_draw(int w, int cw, int ch, int focused) {
    struct note *n;
    int rows, cols, i, row, y, start, end, next, cr, cc, a, b, k, hb, left;
    n = N(w);
    hb = !wrap;                                    // the bar across
    d_rect(0, 0, cw, ch, C_WHITE);
    d_bevel(0, 0, cw, ch, B_SUNKEN);
    rows = (ch - 6 - (hb ? 16 : 0)) / nch;
    cols = (cw - 26) / ncw;
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    n->rows = rows; n->cols = cols;
    lay(n);
    cr = row_of(n, n->caret, &cc);
    if (cr < n->top) n->top = cr;
    if (cr >= n->top + rows) n->top = cr - rows + 1;
    n->maxw = hb ? widest(n) : 0;
    if (hb) {
        // across, the view follows the caret only when it has moved (or
        // Word Wrap has just gone off), so the bar can be scrolled away from it
        if (n->caret != n->lcaret || n->lwrap) {
            if (cc < n->left) n->left = cc;
            if (cc >= n->left + cols) n->left = cc - cols + 1;
        }
        if (n->left > n->maxw + 1 - cols) n->left = n->maxw + 1 - cols;
        if (n->left < 0) n->left = 0;
    } else n->left = 0;
    n->lcaret = n->caret;
    n->lwrap = wrap;
    left = n->left;
    // walk to the first row shown, then draw row by row
    i = row_start(n, n->top);
    row = n->top;
    y = 4;
    while (row < n->top + rows && i <= n->len) {
        start = i;
        end = row_end(n, start, &next);
        k = end - start - left;
        if (k > cols) k = cols;
        if (k > 0) d_textn(4, y, C_DARK, nfs, nfont, ncw, n->text + start + left, k);
        // the selection's part of the row, white on the selection's colour
        if (has_sel(n) && sel0(n) < next && sel1(n) > start) {
            a = sel0(n) > start ? sel0(n) - start : 0;
            b = sel1(n) < next ? sel1(n) - start : next - start;   // past the end: its newline or fold, a cell wide
            a = a - left; b = b - left;
            if (a < 0) a = 0;
            if (b > cols) b = cols;
            if (b > a) {
                d_rect(4 + a * ncw, y, (b - a) * ncw, nch - 1, C_SEL);
                k = (b < end - start - left ? b : end - start - left) - a;
                if (k > 0) d_textn(4 + a * ncw, y, C_WHITE, nfs, nfont, ncw, n->text + start + left + a, k);
            }
        }
        if (focused && row == cr && cc >= left && cc - left <= cols) d_rect(4 + (cc - left) * ncw, y, 1, nch - 1, C_DARK);
        y = y + nch;
        ++row;
        i = next;
    }
    ui_scrollbar(cw - 18, 2, ch - 4 - (hb ? 16 : 0), row_count(n), rows, n->top);
    if (hb) {
        ui_hscrollbar(2, ch - 18, cw - 20, n->maxw + 1, cols, left);
        d_rect(cw - 18, ch - 18, 16, 16, C_FACE);
    }
}

// the buffer position under a point of the text, the nearest on its row
static int pos_at(struct note *n, int x, int y) {
    int col, row;
    col = (x - 4 + ncw / 2) / ncw;
    if (col < 0) col = 0;
    row = (y - 4) / nch;
    if (row < 0) row = 0;
    return row_pos(n, n->top + row, n->left + col);
}

static void note_mouse(int w, int ev, int x, int y, int b) {
    struct note *n;
    int rows, t, cw, ch, hb;
    n = N(w);
    wk_size(w, &cw, &ch);
    lay(n);
    hb = !wrap;
    rows = n->rows > 0 ? n->rows : 1;
    if (ev == E_WHEEL) {
        n->top = n->top + b * 3;
        if (n->top > row_count(n) - rows) n->top = row_count(n) - rows;
        if (n->top < 0) n->top = 0;
        // keep the caret on screen, as editors do when they scroll
        n->caret = row_pos(n, n->top + (rows / 2), 0);
        n->anchor = n->caret;
        return;
    }
    if (ev == E_UP) { n->dragging = 0; return; }
    if (ev == E_MOVE) {
        t = ui_scroll_drag(cw - 18, 2, y);
        if (t >= 0) { n->top = t; n->caret = row_pos(n, t, 0); n->anchor = n->caret; return; }
        t = ui_scroll_drag(2, ch - 18, x);
        if (t >= 0) { n->left = t; wk_invalidate(w, 0, 0, 0, 0); return; }
        // a selection being dragged out
        if (n->dragging && (t = pos_at(n, x, y)) != n->caret) {
            n->caret = t;
            row_of(n, n->caret, &n->want_col);
            wk_invalidate(w, 0, 0, 0, 0);
        }
        return;
    }
    if (ev != E_DOWN) return;
    t = ui_scroll_click(x, y, cw - 18, 2, ch - 4 - (hb ? 16 : 0), row_count(n), rows, n->top);
    if (t >= 0) { n->top = t; n->caret = row_pos(n, t, 0); n->anchor = n->caret; return; }
    if (hb && (t = ui_hscroll_click(x, y, 2, ch - 18, cw - 20, n->maxw + 1, n->cols, n->left)) >= 0) { n->left = t; return; }
    n->caret = pos_at(n, x, y);
    if (!(wk.mods & M_SHIFT)) n->anchor = n->caret;      // Shift+click: the selection to here
    n->dragging = 1;
    row_of(n, n->caret, &n->want_col);
}

static void note_command(int w, int id);

static void note_key(int w, int down, int code, int ch, int mods) {
    struct note *n;
    int row, col, was, moved;
    if (!down) return;
    n = N(w);
    was = n->modified;
    if (ui_key_copy(code, ch, mods)) { note_command(w, WK_CMD(1, E_COPY)); return; }
    if (ui_key_cut(code, ch, mods)) { note_command(w, WK_CMD(1, E_CUT)); return; }
    if (ui_key_paste(code, ch, mods)) { note_command(w, WK_CMD(1, E_PASTE)); return; }
    if (mods & M_CTRL) {
        if (ch == 's' || ch == 'S' || code == 'S') note_command(w, WK_CMD(0, N_SAVE));
        else if (ch == 'o' || ch == 'O' || code == 'O') note_command(w, WK_CMD(0, N_OPEN));
        else if (ch == 'n' || ch == 'N' || code == 'N') note_command(w, WK_CMD(0, N_NEW));
        else if (ch == 'a' || ch == 'A' || code == 'A') note_command(w, WK_CMD(1, E_ALL));
        return;
    }
    lay(n);
    row = row_of(n, n->caret, &col);
    // the moving keys, by rows: with Shift they take the selection along
    moved = 1;
    if (code == KEY_LEFT) { if (n->caret > 0) --n->caret; row_of(n, n->caret, &n->want_col); }
    else if (code == KEY_RIGHT) { if (n->caret < n->len) ++n->caret; row_of(n, n->caret, &n->want_col); }
    else if (code == KEY_UP) { if (row > 0) n->caret = row_pos(n, row - 1, n->want_col); }
    else if (code == KEY_DOWN) n->caret = row_pos(n, row + 1, n->want_col);
    else if (code == KEY_HOME) { n->caret = row_pos(n, row, 0); n->want_col = 0; }
    else if (code == KEY_END) { n->caret = row_pos(n, row, 100000); row_of(n, n->caret, &n->want_col); }
    else if (code == KEY_PGUP) n->caret = row_pos(n, row > 20 ? row - 20 : 0, n->want_col);
    else if (code == KEY_PGDN) n->caret = row_pos(n, row + 20, n->want_col);
    else moved = 0;
    if (moved) { if (!(mods & M_SHIFT)) n->anchor = n->caret; return; }
    // the editing keys: a selection is what they delete, or what typing replaces
    if (code == KEY_BACK) { if (!cut_sel(n) && n->caret > 0) { --n->caret; erase(n, n->caret); } row_of(n, n->caret, &n->want_col); }
    else if (code == KEY_DEL) { if (!cut_sel(n)) erase(n, n->caret); }
    else if (code == KEY_ENTER) { cut_sel(n); insert(n, 10); n->want_col = 0; }
    else if (code == KEY_TAB) { cut_sel(n); insert(n, ' '); insert(n, ' '); insert(n, ' '); insert(n, ' '); row_of(n, n->caret, &n->want_col); }
    else if (ch >= 32 && ch < 127) { cut_sel(n); insert(n, ch); row_of(n, n->caret, &n->want_col); }
    n->anchor = n->caret;
    if (n->modified != was) retitle(w);
}

static void save_as(int w, struct note *n) {
    ui_ncpy(n->ask, n->path[0] ? n->path : notepad_untitled, WK_PATH);
    if (notepad_filebox) wk_filebox(w, D_SAVEAS, 1, notepad_filter, n->ask, WK_PATH);
    else wk_inputbox(w, D_SAVEAS, "Save As", "Save as:", n->ask, WK_PATH);
}

static void note_command(int w, int id) {
    struct note *n;
    int item, was;
    n = N(w);
    lay(n);
    item = id % WK_CMD_STEP;
    if (id / WK_CMD_STEP == 3) { wk_msgbox(w, 0, about_title, notepad_about, WK_OK); return; }
    if (id / WK_CMD_STEP == 2) { format_pick(item); return; }
    if (id / WK_CMD_STEP == 1) {
        was = n->modified;
        if (item == E_COPY || item == E_CUT) copy_sel(n);
        if (item == E_CUT || item == E_DELETE) cut_sel(n);
        else if (item == E_PASTE) paste(n);
        else if (item == E_ALL) { n->anchor = 0; n->caret = n->len; }
        row_of(n, n->caret, &n->want_col);
        if (n->modified != was) retitle(w);
        wk_invalidate(w, 0, 0, 0, 0);
        return;
    }
    if (item == N_NEW) { empty(n); n->path[0] = 0; retitle(w); }
    else if (item == N_OPEN) {
        ui_ncpy(n->ask, n->path[0] ? n->path : notepad_home, WK_PATH);
        if (notepad_filebox) wk_filebox(w, D_OPEN, 0, notepad_filter, n->ask, WK_PATH);
        else wk_inputbox(w, D_OPEN, "Open", "File to open:", n->ask, WK_PATH);
    } else if (item == N_SAVE) {
        if (!n->path[0]) save_as(w, n);
        else if (!save(n)) say(w, "Cannot save that file.");
        retitle(w);
    }
    else if (item == N_SAVEAS) save_as(w, n);
    else if (item == N_EXIT) wk_ask_close(w);
}

static void note_answer(int w, int dialog, int yes) {
    struct note *n;
    n = N(w);
    if (!yes) return;
    if (dialog == D_OPEN) open_file(w, n->ask, 0);
    else if (dialog == D_SAVEAS) {
        ui_ncpy(n->path, n->ask, WK_PATH);
        if (!save(n)) say(w, "Cannot save that file.");
    }
    retitle(w);
}

static int note_pointer(int w, int x, int y) {
    int cw, ch;
    wk_size(w, &cw, &ch);
    return x < cw - 18 && (wrap || y < ch - 18) ? PTR_TEXT : PTR_ARROW;
}

struct wk_class notepad_class = {
    .kind = ICON_NOTE, .open = note_open_hook, .draw = note_draw, .mouse = note_mouse,
    .key = note_key, .command = note_command, .answer = note_answer, .destroy = note_destroy,
    .pointer = note_pointer
};
