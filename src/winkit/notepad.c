// notepad.c -- Notepad: a plain text editor, as a window kit class.
// docs/grin-plan.md 4; it was the C4IX desktop's Text Editor (apps.c).
//
// The whole file in one buffer, edited in place, which grows as it must:
// a file is as big as memory allows, and one that will not fit is refused
// rather than cut short (the desktop review's B6). Lines are found by
// scanning for newlines, and drawn in a fixed-width face so the caret can
// be placed exactly. Files open from, and save to, the host's (wk_fopen).

#include "winkit.h"

enum { NCW = 7, NCH = 15, NFONT = 12, CHUNK = 4096 };
enum { D_OPEN = 1, D_SAVEAS = 2 };                // its dialogs
enum { N_NEW = 0, N_OPEN = 1, N_SAVE = 2, N_SAVEAS = 3, N_EXIT = 5 };   // File's items

char *notepad_name = "Notepad";
char *notepad_about = "Notepad: edits plain text.";
char *notepad_home = "/";                         // where Open starts
char *notepad_untitled = "untitled.txt";          // where Save As offers to put a new one
int notepad_filebox = 0;                          // Open and Save As: the kit's file box, not a typed path

struct note {
    char path[WK_PATH];
    char ask[WK_PATH];                            // a dialog's path, in and out
    int len, cap, caret, top, modified;
    int want_col;                                 // the column up/down tries to keep
    char *text;                                   // cap bytes, len of them text and a 0
};

static char buf[WK_PATH + 64];
static char about_item[64];
static char about_title[64];
static char *file_items[7] = { "New", "Open...", "Save", "Save As...", "-", "Exit", 0 };
static char *help_items[2];
static struct wk_menu menus[2];

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
    n->len = 0; n->caret = 0; n->top = 0; n->modified = 0;
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

static void note_open_hook(int w, char *path) {
    struct note *n;
    n = N(w);
    help_items[0] = about_item; help_items[1] = 0;
    menus[0].title = "File"; menus[0].items = file_items;
    menus[1].title = "Help"; menus[1].items = help_items;
    wk_menu(w, menus, 2);
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
    ui_cpy(about_item, "About ");
    ui_ncat(about_item, notepad_name, 64);
    ui_cpy(about_title, about_item);
    ui_cpy(buf, "Untitled - ");
    ui_ncat(buf, notepad_name, WK_PATH + 64);
    notepad_class.size = sizeof(struct note);
    return wk_open(&notepad_class, buf, 560, 400, 0, path);
}

// line and column of a buffer position
static int line_of(struct note *n, int pos, int *col) {
    int i, line, start;
    line = 0; start = 0;
    i = 0;
    while (i < pos) { if (n->text[i] == 10) { ++line; start = i + 1; } ++i; }
    *col = pos - start;
    return line;
}

// the buffer position of a line, and of a column in it (clamped to the line)
static int pos_of(struct note *n, int line, int col) {
    int i, l;
    i = 0; l = 0;
    while (i < n->len && l < line) { if (n->text[i] == 10) ++l; ++i; }
    while (col > 0 && i < n->len && n->text[i] != 10) { ++i; --col; }
    return i;
}

static int lines(struct note *n) {
    int i, l;
    l = 1; i = 0;
    while (i < n->len) { if (n->text[i] == 10) ++l; ++i; }
    return l;
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

static void note_draw(int w, int cw, int ch, int focused) {
    struct note *n;
    int rows, cols, i, line, y, start, cl, cc, total;
    n = N(w);
    d_rect(0, 0, cw, ch, C_WHITE);
    d_bevel(0, 0, cw, ch, B_SUNKEN);
    rows = (ch - 6) / NCH;
    cols = (cw - 26) / NCW;
    cl = line_of(n, n->caret, &cc);
    if (cl < n->top) n->top = cl;
    if (cl >= n->top + rows) n->top = cl - rows + 1;
    // walk to the first line shown, then draw line by line
    i = pos_of(n, n->top, 0);
    line = n->top;
    y = 4;
    while (line < n->top + rows && i <= n->len) {
        start = i;
        while (i < n->len && n->text[i] != 10) ++i;
        if (i > start) d_textn(4, y, C_DARK, NFONT, F_MONO, NCW, n->text + start, i - start > cols ? cols : i - start);
        if (focused && line == cl && cc <= cols) d_rect(4 + cc * NCW, y, 1, NCH - 1, C_DARK);
        y = y + NCH;
        ++line;
        if (i >= n->len) break;
        ++i;
    }
    total = lines(n);
    ui_scrollbar(cw - 18, 2, ch - 4, total, rows, n->top);
}

static void note_mouse(int w, int ev, int x, int y, int b) {
    struct note *n;
    int rows, t, col, cw, ch;
    n = N(w);
    wk_size(w, &cw, &ch);
    rows = (ch - 6) / NCH;
    if (ev == E_WHEEL) {
        n->top = n->top + b * 3;
        if (n->top > lines(n) - rows) n->top = lines(n) - rows;
        if (n->top < 0) n->top = 0;
        // keep the caret on screen, as editors do when they scroll
        n->caret = pos_of(n, n->top + (rows / 2), 0);
        return;
    }
    if (ev == E_MOVE) {
        t = ui_scroll_drag(cw - 18, 2, y);
        if (t >= 0) { n->top = t; n->caret = pos_of(n, t, 0); }
        return;
    }
    if (ev != E_DOWN) return;
    t = ui_scroll_click(x, y, cw - 18, 2, ch - 4, lines(n), rows, n->top);
    if (t >= 0) { n->top = t; n->caret = pos_of(n, t, 0); return; }
    col = (x - 4 + NCW / 2) / NCW;
    if (col < 0) col = 0;
    n->caret = pos_of(n, n->top + (y - 4) / NCH, col);
    line_of(n, n->caret, &n->want_col);
}

static void note_command(int w, int id);

static void note_key(int w, int down, int code, int ch, int mods) {
    struct note *n;
    int line, col, was;
    if (!down) return;
    n = N(w);
    was = n->modified;
    if (mods & M_CTRL) {
        if (ch == 's' || ch == 'S' || code == 'S') note_command(w, WK_CMD(0, N_SAVE));
        else if (ch == 'o' || ch == 'O' || code == 'O') note_command(w, WK_CMD(0, N_OPEN));
        else if (ch == 'n' || ch == 'N' || code == 'N') note_command(w, WK_CMD(0, N_NEW));
        return;
    }
    line = line_of(n, n->caret, &col);
    if (code == KEY_LEFT) { if (n->caret > 0) --n->caret; line_of(n, n->caret, &n->want_col); }
    else if (code == KEY_RIGHT) { if (n->caret < n->len) ++n->caret; line_of(n, n->caret, &n->want_col); }
    else if (code == KEY_UP) { if (line > 0) n->caret = pos_of(n, line - 1, n->want_col); }
    else if (code == KEY_DOWN) n->caret = pos_of(n, line + 1, n->want_col);
    else if (code == KEY_HOME) { n->caret = pos_of(n, line, 0); n->want_col = 0; }
    else if (code == KEY_END) { n->caret = pos_of(n, line, 100000); line_of(n, n->caret, &n->want_col); }
    else if (code == KEY_PGUP) n->caret = pos_of(n, line > 20 ? line - 20 : 0, n->want_col);
    else if (code == KEY_PGDN) n->caret = pos_of(n, line + 20, n->want_col);
    else if (code == KEY_BACK) { if (n->caret > 0) { --n->caret; erase(n, n->caret); } line_of(n, n->caret, &n->want_col); }
    else if (code == KEY_DEL) erase(n, n->caret);
    else if (code == KEY_ENTER) { insert(n, 10); n->want_col = 0; }
    else if (code == KEY_TAB) { insert(n, ' '); insert(n, ' '); insert(n, ' '); insert(n, ' '); line_of(n, n->caret, &n->want_col); }
    else if (ch >= 32 && ch < 127) { insert(n, ch); line_of(n, n->caret, &n->want_col); }
    if (n->modified != was) retitle(w);
}

static void save_as(int w, struct note *n) {
    ui_ncpy(n->ask, n->path[0] ? n->path : notepad_untitled, WK_PATH);
    if (notepad_filebox) wk_filebox(w, D_SAVEAS, 1, "*", n->ask, WK_PATH);
    else wk_inputbox(w, D_SAVEAS, "Save As", "Save as:", n->ask, WK_PATH);
}

static void note_command(int w, int id) {
    struct note *n;
    int item;
    n = N(w);
    item = id % WK_CMD_STEP;
    if (id / WK_CMD_STEP == 1) { wk_msgbox(w, 0, about_title, notepad_about, WK_OK); return; }
    if (item == N_NEW) { empty(n); n->path[0] = 0; retitle(w); }
    else if (item == N_OPEN) {
        ui_ncpy(n->ask, n->path[0] ? n->path : notepad_home, WK_PATH);
        if (notepad_filebox) wk_filebox(w, D_OPEN, 0, "*", n->ask, WK_PATH);
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
    return x < cw - 18 ? PTR_TEXT : PTR_ARROW;
}

struct wk_class notepad_class = {
    .kind = ICON_NOTE, .open = note_open_hook, .draw = note_draw, .mouse = note_mouse,
    .key = note_key, .command = note_command, .answer = note_answer, .destroy = note_destroy,
    .pointer = note_pointer
};
