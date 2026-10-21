// apps.c -- Notepad and About (the Calculator is calc.c). docs/c4ix-desktop.md.

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
        if (!n->path[0]) inputbox(w, "Save As", "Save as:", "/tmp/untitled.txt", A_SAVEAS);
        else if (!note_save(n)) msgbox(w, "Notepad", "Cannot save that file.", 0, A_NONE);
        note_retitle(w);
    }
    else if (item == 3) inputbox(w, "Save As", "Save as:", n->path[0] ? n->path : "/tmp/untitled.txt", A_SAVEAS);
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

// ---- About ---------------------------------------------------------------------------------

void about_open() {
    int i, k;
    struct win *w;
    w = win_first();
    while (w) { if (w->used && w->kind == K_ABOUT) { win_raise(w->id); return; } w = w->next; }
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

// ---- Settings: the colour scheme and the desktop's colour ---------------------------
//
// Display Properties, cut down: a list of schemes and a row of desktop
// colours, each applied at once (theme_set, which also remembers them in
// /etc/desktop.cfg), beside a preview drawn in the colours in use.

static char *dflt_label;
static char sbuf[16];

int settings_open() {
    int k;
    struct win *w;
    w = win_first();
    while (w) { if (w->used && w->kind == K_SETTINGS) { win_raise(w->id); return w->id; } w = w->next; }
    dflt_label = "Defaults";
    return win_open(K_SETTINGS, "Settings", 440, 380);
}

void settings_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    int i, x, y, sel, n;
    d_rect(ox, oy, cw, ch, C_FACE);
    // the schemes
    n = theme_count();
    ui_group(ox + 8, oy + 8, 180, 20 + n * 20, "Color scheme");
    sel = theme_scheme();
    i = 0;
    while (i < n) { ui_radio(ox + 18, oy + 26 + i * 20, theme_name(i), i == sel); ++i; }
    // the desktop: the picture, the scheme's colour, or one of eight
    y = oy + 36 + n * 20;
    ui_group(ox + 8, y, 180, 94, "Desktop");
    ui_radio(ox + 18, y + 16, "Picture: C4IX", theme_wall());
    ui_radio(ox + 18, y + 36, "The scheme's colour", !theme_wall() && theme_bg() < 0);
    i = 0;
    while (i < 8) {
        x = ox + 18 + i * 20;
        sel = !theme_wall() && theme_bg() == theme_swatch(i);
        d_rect(x, y + 60, 16, 16, theme_swatch(i));
        d_recto(x - 1, y + 59, 18, 18, sel ? C_DARK : C_SHADOW);
        if (sel) d_recto(x - 2, y + 58, 20, 20, C_DARK);
        ++i;
    }
    // the preview: the desktop, an inactive window, an active one
    x = ox + 200; y = oy + 14;
    d_rect(x, y, cw - 210, 170, C_DESK);
    d_bevel(x, y, cw - 210, 170, B_SUNKEN);
    d_rect(x + 12, y + 14, 150, 70, C_FACE); d_bevel(x + 12, y + 14, 150, 70, B_RAISED);
    d_rect(x + 15, y + 17, 144, 16, C_ITITLE);
    d_text(x + 19, y + 19, C_FACE, F_BOLD, "Inactive Window");
    d_rect(x + 28, y + 50, 170, 100, C_FACE); d_bevel(x + 28, y + 50, 170, 100, B_RAISED);
    d_rect(x + 31, y + 53, 164, 16, C_TITLE);
    d_text(x + 35, y + 55, C_LIGHT, F_BOLD, "Active Window");
    d_rect(x + 36, y + 76, 150, 40, C_WHITE); d_bevel(x + 36, y + 76, 150, 40, B_SUNKEN);
    d_text(x + 42, y + 80, C_DARK, F_SANS, "Normal");
    d_rect(x + 40, y + 96, 60, 15, C_SEL);
    d_text(x + 42, y + 97, C_WHITE, F_SANS, "Selected");
    ui_button(x + 110, y + 122, 76, 22, "OK", 0, 1);
    // the screen saver: which, after how long, and a look at it now
    x = ox + 200; y = oy + 192;
    n = saver_count();
    ui_group(x, y, cw - 210, 26 + n * 18, "Screen saver");
    sel = theme_saver();
    i = 0;
    while (i < n) { ui_radio(x + 10, y + 16 + i * 18, saver_name(i), i == sel); ++i; }
    ui_button(x + 130, y + 14, 70, 22, "Preview", 0, 0);
    ui_num(sbuf, theme_wait());
    ui_cat(sbuf, " min");
    d_text(x + 130, y + 46, C_DARK, F_SANS, "Wait:");
    ui_button(x + 130, y + 62, 22, 20, "-", 0, 0);
    d_text(x + 156, y + 66, C_DARK, F_SANS, sbuf);
    ui_button(x + 192, y + 62, 22, 20, "+", 0, 0);
    ui_button(ox + 8, oy + ch - 34, 86, 24, dflt_label, 0, 0);
}

void settings_mouse(struct win *w, int ev, int x, int y, int cw, int ch) {
    int n, i, sy, k;
    if (ev != E_DOWN) return;
    n = theme_count();
    i = 0;
    while (i < n) {
        if (ui_hit(x, y, 14, 24 + i * 20, 170, 18)) { k = theme_wall(); theme_set(i, theme_bg()); theme_set_wall(k); return; }
        ++i;
    }
    sy = 36 + n * 20;
    if (ui_hit(x, y, 14, sy + 14, 170, 18)) { theme_set_wall(1); return; }
    if (ui_hit(x, y, 14, sy + 34, 170, 18)) { theme_set(theme_scheme(), 0 - 1); return; }
    i = 0;
    while (i < 8) {
        if (ui_hit(x, y, 16 + i * 20, sy + 58, 20, 20)) { theme_set(theme_scheme(), theme_swatch(i)); return; }
        ++i;
    }
    // the screen saver group (settings_draw)
    i = 0;
    while (i < saver_count()) {
        if (ui_hit(x, y, 206, 206 + i * 18, 120, 16)) { theme_set_saver(i, theme_wait()); return; }
        ++i;
    }
    if (ui_hit(x, y, 330, 206, 70, 22)) { saver_preview(); return; }
    if (ui_hit(x, y, 330, 254, 22, 20)) { theme_set_saver(theme_saver(), theme_wait() - 1); return; }
    if (ui_hit(x, y, 392, 254, 22, 20)) { theme_set_saver(theme_saver(), theme_wait() + 1); return; }
    if (ui_hit(x, y, 8, ch - 34, 86, 24)) { theme_set(0, 0 - 1); theme_set_wall(1); }
}

// Up and Down walk the schemes; Esc or Enter closes
void settings_key(struct win *w, int code, int ch, int mods) {
    int s, w2;
    s = theme_scheme();
    w2 = theme_wall();
    if (code == KEY_UP && s > 0) { theme_set(s - 1, theme_bg()); theme_set_wall(w2); }
    else if (code == KEY_DOWN && s < theme_count() - 1) { theme_set(s + 1, theme_bg()); theme_set_wall(w2); }
    else if (code == KEY_ESC || code == KEY_ENTER) win_close(win_index(w));
}
