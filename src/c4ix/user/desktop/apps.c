// apps.c -- Notepad, Calculator, About. docs/c4ix-desktop.md.

#include "desktop.h"

static char buf[200];

// ---- Notepad -------------------------------------------------------------------------
//
// The whole file in one buffer, edited in place. Lines are found by
// scanning for newlines -- files here are small -- and drawn in a fixed-
// width face so the caret can be placed exactly. Files open from, and
// save to, C4IX's filesystem.

enum { NBUF = 65000, NCW = 7, NCH = 15, NFONT = 12 };

struct note {
    char path[128];
    int len, caret, top, modified;
    int want_col;                             // the column up/down tries to keep
    char text[NBUF];
};

static struct note *N(struct win *w) { return (struct note *)w->st; }
static char *note_menu_titles[2];
static char *note_file_items[7];
static char *note_help_items[2];
static char **note_menu_items[2];

static void note_retitle(struct win *w) {
    struct note *n;
    int k;
    n = N(w);
    if (n->path[0]) {
        k = ui_len(n->path);
        while (k > 0 && n->path[k - 1] != '/') --k;
        ui_cpy(buf, n->path + k);
    } else ui_cpy(buf, "Untitled");
    if (n->modified) ui_cat(buf, " *");
    ui_cat(buf, " - Notepad");
    win_title(w, buf);
}

static int note_load(struct note *n, char *path) {
    int fd, r;
    if ((fd = uopen(path, O_RD)) < 0) return 0;
    n->len = 0;
    while (n->len < NBUF - 1) {
        r = uread(fd, n->text + n->len, NBUF - 1 - n->len);
        if (r <= 0) break;
        n->len = n->len + r;
    }
    uclose(fd);
    n->text[n->len] = 0;
    n->caret = 0; n->top = 0; n->modified = 0;
    ui_cpy(n->path, path);
    return 1;
}

static int note_save(struct note *n) {
    int fd, r;
    if ((fd = uopen(n->path, O_WR | O_CREATE | O_TRUNCATE)) < 0) return 0;
    r = write(fd, n->text, n->len);
    uclose(fd);
    if (r != n->len) return 0;
    n->modified = 0;
    return 1;
}

int note_open(char *path) {
    int i;
    struct win *w;
    i = win_open(K_NOTE, "Untitled - Notepad", 560, 400);
    if (i < 0) return i;
    w = win_get(i);
    note_menu_titles[0] = "File"; note_menu_titles[1] = "Help";
    note_file_items[0] = "New"; note_file_items[1] = "Open..."; note_file_items[2] = "Save";
    note_file_items[3] = "Save As..."; note_file_items[4] = "-"; note_file_items[5] = "Exit"; note_file_items[6] = 0;
    note_help_items[0] = "About Notepad"; note_help_items[1] = 0;
    note_menu_items[0] = note_file_items; note_menu_items[1] = note_help_items;
    win_menus(w, 2, note_menu_titles, note_menu_items);
    if (path && !note_load(N(w), path)) {
        ui_cpy(N(w)->path, path);
        msgbox(w, "Notepad", "Cannot open that file; starting a new one with its name.", 0, A_NONE);
    }
    note_retitle(w);
    return i;
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
    if (n->len >= NBUF - 1) return;
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

void note_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    struct note *n;
    int rows, cols, i, line, col, y, start, cl, cc, total;
    n = N(w);
    d_rect(ox, oy, cw, ch, C_WHITE);
    d_bevel(ox, oy, cw, ch, B_SUNKEN);
    rows = (ch - 6) / NCH;
    cols = (cw - 26) / NCW;
    cl = line_of(n, n->caret, &cc);
    if (cl < n->top) n->top = cl;
    if (cl >= n->top + rows) n->top = cl - rows + 1;
    // walk to the first line shown, then draw line by line
    i = pos_of(n, n->top, 0);
    line = n->top;
    y = oy + 4;
    while (line < n->top + rows && i <= n->len) {
        start = i;
        while (i < n->len && n->text[i] != 10) ++i;
        if (i > start) d_textn(ox + 4, y, C_DARK, NFONT, F_MONO, NCW, n->text + start, i - start > cols ? cols : i - start);
        if (focused && line == cl && cc <= cols) d_rect(ox + 4 + cc * NCW, y, 1, NCH - 1, C_DARK);
        y = y + NCH;
        ++line;
        if (i >= n->len) break;
        ++i;
    }
    total = lines(n);
    ui_scrollbar(ox + cw - 18, oy + 2, ch - 4, total, rows, n->top);
}

void note_mouse(struct win *w, int ev, int x, int y, int cw, int ch) {
    struct note *n;
    int rows, t, col;
    n = N(w);
    rows = (ch - 6) / NCH;
    if (ev == E_WHEEL) {
        n->top = n->top + y * 3;
        if (n->top > lines(n) - rows) n->top = lines(n) - rows;
        if (n->top < 0) n->top = 0;
        // keep the caret on screen, as Notepad does when it scrolls
        n->caret = pos_of(n, n->top + (rows / 2), 0);
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

void note_key(struct win *w, int code, int ch, int mods) {
    struct note *n;
    int line, col, was;
    n = N(w);
    was = n->modified;
    if (mods & M_CTRL) {
        if (ch == 's' || ch == 'S' || code == 83) note_command(w, 0, 2);
        else if (ch == 'o' || ch == 'O' || code == 79) note_command(w, 0, 1);
        else if (ch == 'n' || ch == 'N' || code == 78) note_command(w, 0, 0);
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
    if (n->modified != was) note_retitle(w);
}

void note_command(struct win *w, int menu, int item) {
    struct note *n;
    n = N(w);
    if (menu == 1) { msgbox(w, "About Notepad", "Notepad for C4IX: edits text in the RAM filesystem.", 0, A_NONE); return; }
    if (item == 0) { n->len = 0; n->text[0] = 0; n->caret = 0; n->top = 0; n->modified = 0; n->path[0] = 0; note_retitle(w); }
    else if (item == 1) inputbox(w, "Open", "File to open:", n->path[0] ? n->path : "/", A_OPEN);
    else if (item == 2) {
        if (!n->path[0]) inputbox(w, "Save As", "Save as:", "/ram/untitled.txt", A_SAVEAS);
        else if (!note_save(n)) msgbox(w, "Notepad", "Cannot save that file.", 0, A_NONE);
        note_retitle(w);
    }
    else if (item == 3) inputbox(w, "Save As", "Save as:", n->path[0] ? n->path : "/ram/untitled.txt", A_SAVEAS);
    else if (item == 5) win_close(win_index(w));
}

void note_answer(struct win *w, int action, int yes) {
    struct note *n;
    n = N(w);
    if (!yes) return;
    if (action == A_OPEN) {
        if (!note_load(n, dialog_text())) msgbox(w, "Notepad", "Cannot open that file.", 0, A_NONE);
    } else if (action == A_SAVEAS) {
        ui_cpy(n->path, dialog_text());
        if (!note_save(n)) msgbox(w, "Notepad", "Cannot save that file.", 0, A_NONE);
    }
    note_retitle(w);
}

// ---- Calculator ---------------------------------------------------------------------
//
// A programmer's calculator: 32-bit integers shown in Hex, Dec, Oct or
// Bin. C4IX's compiler has no floating point worth the name, and on a
// machine made of words this is the calculator one actually wants.

struct calc {
    int acc, cur, op, radix, fresh, err;
};

static struct calc *C(struct win *w) { return (struct calc *)w->st; }

// the key grid: 6 columns, 5 rows
static char *keys[30];
static int calc_ready;

static void calc_keys() {
    int k;
    char *all;
    if (calc_ready) return;
    calc_ready = 1;
    k = 0;
    keys[k] = "D"; ++k; keys[k] = "E"; ++k; keys[k] = "F"; ++k; keys[k] = "Mod"; ++k; keys[k] = "And"; ++k; keys[k] = "Not"; ++k;
    keys[k] = "A"; ++k; keys[k] = "B"; ++k; keys[k] = "C"; ++k; keys[k] = "/"; ++k; keys[k] = "Or"; ++k; keys[k] = "Xor"; ++k;
    keys[k] = "7"; ++k; keys[k] = "8"; ++k; keys[k] = "9"; ++k; keys[k] = "*"; ++k; keys[k] = "Lsh"; ++k; keys[k] = "Rsh"; ++k;
    keys[k] = "4"; ++k; keys[k] = "5"; ++k; keys[k] = "6"; ++k; keys[k] = "-"; ++k; keys[k] = "+/-"; ++k; keys[k] = "Back"; ++k;
    keys[k] = "1"; ++k; keys[k] = "2"; ++k; keys[k] = "3"; ++k; keys[k] = "+"; ++k; keys[k] = "0"; ++k; keys[k] = "="; ++k;
}

int calc_open() {
    int i;
    struct win *w;
    calc_keys();
    i = win_open(K_CALC, "Calculator", 360, 300);
    if (i < 0) return i;
    w = win_get(i);
    w->resizable = 0;
    C(w)->radix = 10;
    C(w)->fresh = 1;
    return i;
}

static int apply(struct calc *c, int a, int b, int op) {
    if (op == '+') return a + b;
    if (op == '-') return a - b;
    if (op == '*') return a * b;
    if (op == '/' || op == '%') {
        if (!b) { c->err = 1; return 0; }
        return op == '/' ? a / b : a % b;
    }
    if (op == '&') return a & b;
    if (op == '|') return a | b;
    if (op == '^') return a ^ b;
    if (op == '<') return a << (b & 31);
    if (op == '>') {                         // logical: zeros come in from the top
        b = b & 31;
        if (!b) return a;
        return (a >> b) & (0x7fffffff >> (b - 1));
    }
    return b;
}

// Text for the display: the value in the current radix.
static char *show(struct calc *c, char *out) {
    char t[40];
    int n, v, k, neg, d;
    if (c->err) { ui_cpy(out, "Cannot divide by zero"); return out; }
    v = c->fresh && c->op ? c->acc : c->cur;
    if (c->radix == 10) return ui_num(out, v);
    n = 0;
    neg = 0;
    if (v == 0) { t[0] = '0'; n = 1; }
    while (v != 0 && n < 32) {
        d = v & (c->radix - 1);
        t[n] = d < 10 ? '0' + d : 'A' + d - 10;
        ++n;
        if (c->radix == 16) v = (v >> 4) & 0x0fffffff;
        else if (c->radix == 8) v = (v >> 3) & 0x1fffffff;
        else v = (v >> 1) & 0x7fffffff;
    }
    k = 0;
    while (n) { --n; out[k] = t[n]; ++k; }
    out[k] = 0;
    return out;
}

static void press(struct calc *c, char *key) {
    int d, op;
    d = 0 - 1;
    if (key[1] == 0 && key[0] >= '0' && key[0] <= '9') d = key[0] - '0';
    if (key[1] == 0 && key[0] >= 'A' && key[0] <= 'F') d = key[0] - 'A' + 10;
    if (d >= 0) {
        if (d >= c->radix) return;
        if (c->err) { c->err = 0; c->acc = 0; c->op = 0; }
        if (c->fresh) { c->cur = 0; c->fresh = 0; }
        c->cur = c->cur * c->radix + d;
        return;
    }
    if (ui_eq(key, "C")) { c->acc = 0; c->cur = 0; c->op = 0; c->fresh = 1; c->err = 0; return; }
    if (ui_eq(key, "CE")) { c->cur = 0; c->fresh = 1; c->err = 0; return; }
    if (ui_eq(key, "Back")) { if (!c->fresh) c->cur = c->cur / c->radix; return; }
    if (ui_eq(key, "+/-")) { c->cur = 0 - c->cur; c->fresh = 0; return; }
    if (ui_eq(key, "Not")) { c->cur = ~c->cur; c->fresh = 0; return; }
    op = 0;
    if (ui_eq(key, "+")) op = '+';
    else if (ui_eq(key, "-")) op = '-';
    else if (ui_eq(key, "*")) op = '*';
    else if (ui_eq(key, "/")) op = '/';
    else if (ui_eq(key, "Mod")) op = '%';
    else if (ui_eq(key, "And")) op = '&';
    else if (ui_eq(key, "Or")) op = '|';
    else if (ui_eq(key, "Xor")) op = '^';
    else if (ui_eq(key, "Lsh")) op = '<';
    else if (ui_eq(key, "Rsh")) op = '>';
    else if (ui_eq(key, "=")) op = '=';
    if (!op) return;
    // a pending operation finishes first; two operators in a row replace
    if (c->op && !c->fresh) c->acc = apply(c, c->acc, c->cur, c->op);
    else if (!c->op) c->acc = c->cur;
    if (op == '=') { c->cur = c->acc; c->op = 0; c->fresh = 1; return; }
    c->op = op;
    c->fresh = 1;
}

enum { KW = 50, KH = 30, KX = 12, KY = 96 };
static char *radix_names[4];
static int radix_values[4];

void calc_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    struct calc *c;
    int k, x, y;
    c = C(w);
    calc_keys();
    radix_names[0] = "Hex"; radix_names[1] = "Dec"; radix_names[2] = "Oct"; radix_names[3] = "Bin";
    radix_values[0] = 16; radix_values[1] = 10; radix_values[2] = 8; radix_values[3] = 2;
    d_rect(ox, oy, cw, ch, C_FACE);
    // the display: right-aligned in a sunken white field
    d_rect(ox + 10, oy + 10, cw - 20, 24, C_WHITE);
    d_bevel(ox + 10, oy + 10, cw - 20, 24, B_SUNKEN);
    show(c, buf);
    d_textn(ox + cw - 16 - ui_len(buf) * 8, oy + 15, C_DARK, 14, F_MONO, 8, buf, 0 - 1);
    // the radix
    ui_group(ox + 10, oy + 40, 214, 44, "");
    k = 0;
    while (k < 4) { ui_radio(ox + 20 + k * 50, oy + 60, radix_names[k], c->radix == radix_values[k]); ++k; }
    ui_button(ox + cw - 118, oy + 50, 50, 28, "CE", 0, 0);
    ui_button(ox + cw - 62, oy + 50, 50, 28, "C", 0, 0);
    k = 0;
    while (k < 30) {
        x = ox + KX + (k % 6) * (KW + 6);
        y = oy + KY + (k / 6) * (KH + 6);
        ui_button(x, y, KW, KH, keys[k], 0, 0);
        // the digits the radix cannot use look disabled
        ++k;
    }
    if (c->op) { buf[0] = c->op == '<' ? 'L' : c->op == '>' ? 'R' : c->op; buf[1] = 0; d_text(ox + 14, oy + 16, C_SHADOW, F_SANS, buf); }
}

void calc_mouse(struct win *w, int ev, int x, int y, int cw, int ch) {
    struct calc *c;
    int k;
    c = C(w);
    if (ev != E_DOWN) return;
    k = 0;
    while (k < 4) {
        if (ui_hit(x, y, 20 + k * 50, 58, 46, 16)) { c->radix = radix_values[k]; return; }
        ++k;
    }
    if (ui_hit(x, y, cw - 118, 50, 50, 28)) { press(c, "CE"); return; }
    if (ui_hit(x, y, cw - 62, 50, 50, 28)) { press(c, "C"); return; }
    k = 0;
    while (k < 30) {
        if (ui_hit(x, y, KX + (k % 6) * (KW + 6), KY + (k / 6) * (KH + 6), KW, KH)) { press(c, keys[k]); return; }
        ++k;
    }
}

void calc_key(struct win *w, int code, int ch, int mods) {
    struct calc *c;
    char k[2];
    c = C(w);
    k[1] = 0;
    if (ch >= 'a' && ch <= 'f') ch = ch - 32;
    if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F')) { k[0] = ch; press(c, k); return; }
    if (ch == '+' || ch == '-' || ch == '*' || ch == '/') { k[0] = ch; press(c, k); return; }
    if (ch == '=' || code == KEY_ENTER) { press(c, "="); return; }
    if (ch == '%') { press(c, "Mod"); return; }
    if (ch == '&') { press(c, "And"); return; }
    if (ch == '|') { press(c, "Or"); return; }
    if (ch == '^') { press(c, "Xor"); return; }
    if (ch == '~') { press(c, "Not"); return; }
    if (ch == '<') { press(c, "Lsh"); return; }
    if (ch == '>') { press(c, "Rsh"); return; }
    if (code == KEY_BACK) { press(c, "Back"); return; }
    if (code == KEY_ESC) { press(c, "C"); return; }
    if (code == KEY_DEL) { press(c, "CE"); return; }
}

// ---- About ---------------------------------------------------------------------------------

void about_open() {
    int i, k;
    struct win *w;
    k = 0;
    while (k < MAXW) { w = win_get(k); if (w->used && w->kind == K_ABOUT) { win_raise(k); return; } ++k; }
    i = win_open(K_ABOUT, "About C4IX", 440, 180);
    if (i < 0) return;
    w = win_get(i);
    w->resizable = 0;
    w->x = (SW - 440) / 2; w->y = (SH - 180) / 2 - 60;
}

void about_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    d_rect(ox, oy, cw, ch, C_FACE);
    d_rect(ox + 16, oy + 16, 36, 36, C_TITLE);
    d_rect(ox + 20, oy + 20, 13, 13, 0xff2020); d_rect(ox + 35, oy + 20, 13, 13, 0x20c020);
    d_rect(ox + 20, oy + 35, 13, 13, 0x2040ff); d_rect(ox + 35, oy + 35, 13, 13, 0xffd020);
    d_text(ox + 68, oy + 16, C_DARK, F_BOLD, "C4IX Desktop");
    d_text(ox + 68, oy + 36, C_DARK, F_SANS, "Running on c4m.js: the c4m virtual machine in");
    d_text(ox + 68, oy + 52, C_DARK, F_SANS, "JavaScript, in a Web Worker.");
    d_text(ox + 68, oy + 76, C_DARK, F_SANS, "Every Command Prompt is a real c4ix-sh task; its");
    d_text(ox + 68, oy + 92, C_DARK, F_SANS, "stdin and stdout are pipes the desktop holds.");
    d_text(ox + 68, oy + 116, C_SHADOW, F_SANS, "Enter or Esc closes this window.");
}
