// C4IX userland: desktop. A Windows 95-style desktop on the libjs display,
// and a terminal emulator in its windows. docs/c4ix-desktop.md.
//
// One task owns the display. It draws the desktop, the windows and the
// taskbar, reads mouse and keys from the display, and for every terminal
// window runs a real c4ix-sh as its own task, with that shell's stdin and
// stdout being pipes the desktop holds. Everything the shell and its
// programs print arrives through the pipe and is drawn through a VT100
// subset; everything typed is edited here, a line at a time, the way a
// tty in cooked mode does, and sent down the other pipe on Enter.
//
// It never blocks: output pipes are polled with uavail(), the display's
// event ring with gui_poll(), and when nothing happened the task sleeps.
//
// On a machine without the display it prints "desktop: not fitted".

#include "c4ix_user.h"

#define printf uprintf
#define malloc ualloc
#include "libjs/guest/gui.h"

enum { SW = 1024, SH = 768, TASK_H = 30, BORDER = 4, TITLE_H = 18,
       COLS = 80, ROWS = 24, CW = 8, CH = 16, FONT_PX = 14,
       MAXW = 6, LINE_MAX = 250, SIGTERM = 15 };
enum { K_TERM = 1, K_ABOUT = 2 };
enum { C_DESK = 0x008080, C_FACE = 0xc0c0c0, C_LIGHT = 0xffffff, C_SHADOW = 0x808080,
       C_DARK = 0x000000, C_TITLE = 0x000080, C_TITLE2 = 0x1084d0, C_ITITLE = 0x808080,
       C_ITITLE2 = 0xb4b4b4, C_SEL = 0x000080 };

struct win {
    int used, kind, x, y, w, h;
    int min, max, rx, ry, rw, rh;
    char title[40];
    // terminal
    int pid, in_w, out_r, ended;
    int cx, cy, wrap, fg, bg, bold, rev, cursor;
    int *cell;                    // ROWS*COLS: ch | fg << 8 | bg << 12
    char line[256];
    int llen;
    int esc, np, param[8], priv;
};

struct win *wins;
int order[8], nz;                 // z-order, bottom first; order[nz-1] has focus
int dirty, running;
int menu_open, menu_hot;
int drag, drag_dx, drag_dy;       // drag: window index + 1, 0 when not dragging
int last_click_t, last_click_icon;
int *ev;
char *iobuf;
int pal[16];
int clock_min;
char tmp[96];

// ---- small helpers ------------------------------------------------------

int slen(char *s) { int n; n = 0; while (s[n]) ++n; return n; }
void scpy(char *d, char *s) { while (*s) { *d = *s; ++d; ++s; } *d = 0; }
int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}
char *itoa2(char *p, int v) { *p = '0' + (v / 10) % 10; ++p; *p = '0' + v % 10; ++p; *p = 0; return p; }

// ---- drawing primitives ----------------------------------------------------

// The Windows 95 bevel: light top-left, dark bottom-right, two pixels deep.
void bevel(int x, int y, int w, int h, int raised) {
    int a, b, c, d;
    if (raised) { a = C_LIGHT; b = C_DARK; c = C_FACE; d = C_SHADOW; }
    else        { a = C_SHADOW; b = C_LIGHT; c = C_DARK; d = C_FACE; }
    gui_rect(x, y, w, 1, a);           gui_rect(x, y, 1, h, a);
    gui_rect(x, y + h - 1, w, 1, b);   gui_rect(x + w - 1, y, 1, h, b);
    gui_rect(x + 1, y + 1, w - 2, 1, c); gui_rect(x + 1, y + 1, 1, h - 2, c);
    gui_rect(x + 1, y + h - 2, w - 2, 1, d); gui_rect(x + w - 2, y + 1, 1, h - 2, d);
}

void button(int x, int y, int w, int h, int pressed) {
    gui_rect(x, y, w, h, C_FACE);
    bevel(x, y, w, h, !pressed);
}

void text(int x, int y, int rgb, int font, char *s) { gui_text2(x, y, rgb, 11, font, 0, s, 0 - 1); }

// A horizontal gradient in bands, as Windows 98 draws its title bars.
int mix(int a, int b, int t, int n) {
    int r, g, bl;
    r = ((a >> 16) & 255) + ((((b >> 16) & 255) - ((a >> 16) & 255)) * t) / n;
    g = ((a >> 8) & 255) + ((((b >> 8) & 255) - ((a >> 8) & 255)) * t) / n;
    bl = (a & 255) + (((b & 255) - (a & 255)) * t) / n;
    return (r << 16) | (g << 8) | bl;
}
void gradient(int x, int y, int w, int h, int a, int b) {
    int i, n, bw;
    n = 16; bw = (w + n - 1) / n;
    i = 0;
    while (i < n) {
        if (i * bw < w) gui_rect(x + i * bw, y, (i + 1) * bw > w ? w - i * bw : bw, h, mix(a, b, i, n - 1));
        ++i;
    }
}

// ---- windows ----------------------------------------------------------------

struct win *win_at(int i) { return wins + i; }
int client_x(struct win *w) { return w->x + BORDER; }
int client_y(struct win *w) { return w->y + BORDER + TITLE_H + 1; }
int top_index() { if (nz) return order[nz - 1]; return 0 - 1; }

void raise(int i) {
    int k, j;
    k = 0;
    while (k < nz && order[k] != i) ++k;
    if (k == nz) { order[nz] = i; ++nz; dirty = 1; return; }
    j = k;
    while (j < nz - 1) { order[j] = order[j + 1]; ++j; }
    order[nz - 1] = i;
    dirty = 1;
}

void unlink_z(int i) {
    int k, j;
    k = 0;
    while (k < nz && order[k] != i) ++k;
    if (k == nz) return;
    j = k;
    while (j < nz - 1) { order[j] = order[j + 1]; ++j; }
    --nz;
}

// the topmost window that is showing, or -1
int focus_index() {
    int k;
    k = nz - 1;
    while (k >= 0) { if (!win_at(order[k])->min) return order[k]; --k; }
    return 0 - 1;
}

int new_window(int kind, char *title, int w, int h) {
    int i, n;
    struct win *p;
    i = 0;
    while (i < MAXW && win_at(i)->used) ++i;
    if (i == MAXW) return 0 - 1;
    p = win_at(i);
    p->used = 1; p->kind = kind; p->min = 0; p->max = 0;
    n = 0; while (n < MAXW) { if (win_at(n)->used) ++n; else break; }
    p->x = 60 + i * 28; p->y = 30 + i * 24;
    p->w = w; p->h = h;
    scpy(p->title, title);
    raise(i);
    return i;
}

// ---- the terminal: a grid, fed through a VT100 subset -----------------

void term_clear(struct win *w, int from, int to) {
    int k;
    k = from;
    while (k < to) { w->cell[k] = ' ' | (7 << 8); ++k; }
}

void term_scroll(struct win *w) {
    memcpy(w->cell, w->cell + COLS, (ROWS - 1) * COLS * sizeof(int));
    term_clear(w, (ROWS - 1) * COLS, ROWS * COLS);
}

void term_newline(struct win *w) {
    w->cx = 0; w->wrap = 0;
    ++w->cy;
    if (w->cy >= ROWS) { term_scroll(w); w->cy = ROWS - 1; }
}

int attr(struct win *w) {
    int f, b, t;
    f = w->fg; b = w->bg;
    if (w->bold && f < 8) f = f + 8;
    if (w->rev) { t = f; f = b; b = t; }
    return (f << 8) | (b << 12);
}

void term_putc(struct win *w, int c) {
    if (w->wrap) term_newline(w);
    w->cell[w->cy * COLS + w->cx] = (c & 255) | attr(w);
    ++w->cx;
    if (w->cx >= COLS) { w->cx = COLS - 1; w->wrap = 1; }
}

void sgr(struct win *w) {
    int k, p;
    if (!w->np) { w->np = 1; w->param[0] = 0; }
    k = 0;
    while (k < w->np) {
        p = w->param[k];
        if (p == 0) { w->fg = 7; w->bg = 0; w->bold = 0; w->rev = 0; }
        else if (p == 1) w->bold = 1;
        else if (p == 22) w->bold = 0;
        else if (p == 7) w->rev = 1;
        else if (p == 27) w->rev = 0;
        else if (p >= 30 && p <= 37) w->fg = p - 30;
        else if (p == 39) w->fg = 7;
        else if (p >= 40 && p <= 47) w->bg = p - 40;
        else if (p == 49) w->bg = 0;
        else if (p >= 90 && p <= 97) w->fg = p - 90 + 8;
        else if (p >= 100 && p <= 107) w->bg = p - 100 + 8;
        else if (p == 38 || p == 48) {
            // 256-colour and truecolour: the 16 colours are all this grid
            // has, so take the index when it is one of those and skip the rest
            if (k + 2 < w->np && w->param[k + 1] == 5) {
                if (w->param[k + 2] < 16) { if (p == 38) w->fg = w->param[k + 2]; else w->bg = w->param[k + 2]; }
                k = k + 2;
            } else if (k + 1 < w->np && w->param[k + 1] == 2) k = k + 4;
        }
        ++k;
    }
}

void csi(struct win *w, int fin) {
    int p0, p1, n;
    p0 = w->np > 0 ? w->param[0] : 0;
    p1 = w->np > 1 ? w->param[1] : 0;
    n = p0 ? p0 : 1;
    w->wrap = 0;
    if (fin == 'm') sgr(w);
    else if (fin == 'H' || fin == 'f') { w->cy = (p0 ? p0 : 1) - 1; w->cx = (p1 ? p1 : 1) - 1; }
    else if (fin == 'A') w->cy = w->cy - n;
    else if (fin == 'B') w->cy = w->cy + n;
    else if (fin == 'C') w->cx = w->cx + n;
    else if (fin == 'D') w->cx = w->cx - n;
    else if (fin == 'G') w->cx = n - 1;
    else if (fin == 'd') w->cy = n - 1;
    else if (fin == 'J') {
        if (p0 == 2 || p0 == 3) term_clear(w, 0, ROWS * COLS);
        else if (p0 == 1) term_clear(w, 0, w->cy * COLS + w->cx + 1);
        else term_clear(w, w->cy * COLS + w->cx, ROWS * COLS);
    }
    else if (fin == 'K') {
        if (p0 == 2) term_clear(w, w->cy * COLS, (w->cy + 1) * COLS);
        else if (p0 == 1) term_clear(w, w->cy * COLS, w->cy * COLS + w->cx + 1);
        else term_clear(w, w->cy * COLS + w->cx, (w->cy + 1) * COLS);
    }
    else if ((fin == 'h' || fin == 'l') && w->priv && p0 == 25) w->cursor = fin == 'h';
    if (w->cx < 0) w->cx = 0;
    if (w->cy < 0) w->cy = 0;
    if (w->cx >= COLS) w->cx = COLS - 1;
    if (w->cy >= ROWS) w->cy = ROWS - 1;
}

void term_feed(struct win *w, char *buf, int n) {
    int i, c;
    i = 0;
    while (i < n) {
        c = buf[i] & 255;
        ++i;
        if (w->esc == 1) {
            if (c == '[') { w->esc = 2; w->np = 0; w->priv = 0; w->param[0] = 0; }
            else w->esc = 0;
            continue;
        }
        if (w->esc == 2) {
            if (c == '?') { w->priv = 1; continue; }
            if (c >= '0' && c <= '9') {
                if (!w->np) w->np = 1;
                w->param[w->np - 1] = w->param[w->np - 1] * 10 + (c - '0');
                continue;
            }
            if (c == ';') { if (!w->np) w->np = 1; if (w->np < 8) { w->param[w->np] = 0; ++w->np; } continue; }
            w->esc = 0;
            if (c >= 64 && c <= 126) csi(w, c);
            continue;
        }
        if (c == 27) w->esc = 1;
        else if (c == 10) term_newline(w);          // a tty turns \n into \r\n
        else if (c == 13) { w->cx = 0; w->wrap = 0; }
        else if (c == 8) { if (w->cx > 0) --w->cx; w->wrap = 0; }
        else if (c == 9) { w->cx = (w->cx + 8) & ~7; if (w->cx >= COLS) w->cx = COLS - 1; }
        else if (c >= 32) term_putc(w, c);
    }
    dirty = 1;
}

void term_echo(struct win *w, int c) { tmp[0] = c; term_feed(w, tmp, 1); }
void term_say(struct win *w, char *s) { term_feed(w, s, slen(s)); }

// A shell, with its stdin and stdout on pipes. Our ends are marked
// close-on-spawn, so neither this shell nor any later one inherits them:
// that is what lets a shell see end of file, and lets each window's pipes
// belong to that window alone.
int term_start(struct win *w) {
    int pin[2], pout[2], s0, s1, s2;
    char **argv;
    if (upipe(pin) < 0) return 0;
    if (upipe(pout) < 0) { uclose(pin[0]); uclose(pin[1]); return 0; }
    ucloexec(pin[1], 1);
    ucloexec(pout[0], 1);
    s0 = udup(STDIN); s1 = udup(STDOUT); s2 = udup(STDERR);
    ucloexec(s0, 1); ucloexec(s1, 1); ucloexec(s2, 1);
    udup2(pin[0], STDIN);
    udup2(pout[1], STDOUT);
    udup2(pout[1], STDERR);
    argv = (char **)ualloc(2 * sizeof(char *));
    argv[0] = "c4ix-sh.c4r";
    argv[1] = 0;
    w->pid = spawn("c4ix-sh.c4r", 1, argv);
    udup2(s0, STDIN); udup2(s1, STDOUT); udup2(s2, STDERR);
    uclose(s0); uclose(s1); uclose(s2);
    uclose(pin[0]); uclose(pout[1]);
    w->in_w = pin[1];
    w->out_r = pout[0];
    if (w->pid < 0) { uclose(w->in_w); uclose(w->out_r); return 0; }
    return 1;
}

int open_terminal() {
    int i, k;
    struct win *w;
    i = new_window(K_TERM, "Terminal", COLS * CW + 2 * BORDER + 4, ROWS * CH + 2 * BORDER + TITLE_H + 5);
    if (i < 0) return 0 - 1;
    w = win_at(i);
    if (!w->cell) w->cell = (int *)ualloc(ROWS * COLS * sizeof(int));
    term_clear(w, 0, ROWS * COLS);
    w->cx = 0; w->cy = 0; w->wrap = 0; w->fg = 7; w->bg = 0; w->bold = 0; w->rev = 0;
    w->cursor = 1; w->llen = 0; w->esc = 0; w->ended = 0;
    if (!term_start(w)) { term_say(w, "desktop: could not start a shell\n"); w->ended = 1; w->pid = 0 - 1; }
    else {
        // "Terminal (task N)": the id ps shows for this window's shell
        scpy(w->title, "Terminal (task ");
        k = slen(w->title);
        if (w->pid >= 10) { w->title[k] = '0' + (w->pid / 10) % 10; ++k; }
        w->title[k] = '0' + w->pid % 10; ++k;
        w->title[k] = ')'; ++k; w->title[k] = 0;
    }
    return i;
}

// The window is going: stop what runs in it, then the shell.
void term_stop(struct win *w) {
    if (w->ended) return;
    uintr(w->pid);
    ukill(w->pid, SIGTERM);
    uclose(w->in_w);
    uclose(w->out_r);
    uwait(w->pid);
    w->ended = 1;
}

// Output waiting in a terminal's pipe; never blocks.
void term_poll(struct win *w) {
    int n;
    if (w->ended) return;
    n = uavail(w->out_r);
    if (n > 0) {
        if (n > 1024) n = 1024;
        n = uread(w->out_r, iobuf, n);
        if (n > 0) term_feed(w, iobuf, n);
    } else if (n < 0) {
        // every writer is gone: the shell exited
        uclose(w->in_w);
        uclose(w->out_r);
        uwait(w->pid);
        w->ended = 1;
        term_say(w, "\n[the shell has exited; close this window]\n");
    }
}

// A key typed at a terminal: cooked-mode line editing, as a tty does it.
void term_key(struct win *w, int code, int ch, int mods) {
    if (w->ended) return;
    if ((mods & 2) && (ch == 'c' || ch == 'C' || code == 67)) {
        term_say(w, "^C\n");
        w->llen = 0;
        // Nothing running below the shell: give it an empty line, which
        // is what brings a fresh prompt.
        if (uintr(w->pid) <= 0) write(w->in_w, "\n", 1);
        return;
    }
    if ((mods & 2) && (ch == 'l' || ch == 'L' || code == 76)) {
        term_clear(w, 0, ROWS * COLS); w->cx = 0; w->cy = 0; dirty = 1;
        return;
    }
    if (mods & 2) return;
    if (code == 13 || ch == 10) {
        w->line[w->llen] = 10;
        write(w->in_w, w->line, w->llen + 1);
        w->llen = 0;
        term_echo(w, 10);
        return;
    }
    if (code == 8) {
        if (w->llen > 0) { --w->llen; term_echo(w, 8); term_echo(w, ' '); term_echo(w, 8); }
        return;
    }
    if (ch >= 32 && ch < 127 && w->llen < LINE_MAX) {
        w->line[w->llen] = ch;
        ++w->llen;
        term_echo(w, ch);
    }
}

// The grid, as runs: a rectangle for each stretch of non-black background
// and a line of text for each stretch of one colour.
void term_draw(struct win *w, int ox, int oy, int focused) {
    int r, c, k, a, f, b, start, *row;
    char *s;
    gui_rect(ox, oy, COLS * CW, ROWS * CH, pal[0]);
    r = 0;
    while (r < ROWS) {
        row = w->cell + r * COLS;
        c = 0;
        while (c < COLS) {
            b = (row[c] >> 12) & 15;
            start = c;
            while (c < COLS && ((row[c] >> 12) & 15) == b) ++c;
            if (b) gui_rect(ox + start * CW, oy + r * CH, (c - start) * CW, CH, pal[b]);
        }
        c = 0;
        while (c < COLS) {
            f = (row[c] >> 8) & 15;
            start = c;
            s = tmp;
            while (c < COLS && ((row[c] >> 8) & 15) == f && c - start < 90) { *s = row[c] & 255; ++s; ++c; }
            *s = 0;
            k = 0; a = 0;
            while (tmp[k]) { if (tmp[k] != ' ') a = 1; ++k; }
            if (a) gui_text2(ox + start * CW, oy + r * CH + 1, pal[f], FONT_PX, GUI_MONO, CW, tmp, c - start);
        }
        ++r;
    }
    if (focused && w->cursor && !w->ended)
        gui_rect(ox + w->cx * CW, oy + w->cy * CH + CH - 3, CW, 2, pal[7]);
}

// ---- the About box ----------------------------------------------------------

void about_draw(int ox, int oy, int w, int h) {
    gui_rect(ox, oy, w, h, C_FACE);
    gui_rect(ox + 16, oy + 16, 32, 32, C_TITLE);
    gui_rect(ox + 20, oy + 20, 11, 11, 0xff2020); gui_rect(ox + 33, oy + 20, 11, 11, 0x20c020);
    gui_rect(ox + 20, oy + 33, 11, 11, 0x2040ff); gui_rect(ox + 33, oy + 33, 11, 11, 0xffd020);
    text(ox + 64, oy + 16, C_DARK, GUI_SANS_BOLD, "C4IX Desktop");
    text(ox + 64, oy + 36, C_DARK, GUI_SANS, "Running on c4m.js: the c4m virtual machine");
    text(ox + 64, oy + 52, C_DARK, GUI_SANS, "in JavaScript, in a Web Worker.");
    text(ox + 64, oy + 76, C_DARK, GUI_SANS, "Every terminal is a real c4ix-sh task. Its stdin and");
    text(ox + 64, oy + 92, C_DARK, GUI_SANS, "stdout are pipes this desktop holds.");
}

// ---- the scene --------------------------------------------------------------

void caption_buttons(struct win *w) {
    int bx, by;
    by = w->y + BORDER + 2;
    bx = w->x + w->w - BORDER - 2 - 16;
    button(bx, by, 16, 14, 0);                    // close: an X
    gui_line(bx + 4, by + 3, bx + 10, by + 9, C_DARK); gui_line(bx + 5, by + 3, bx + 11, by + 9, C_DARK);
    gui_line(bx + 10, by + 3, bx + 4, by + 9, C_DARK); gui_line(bx + 11, by + 3, bx + 5, by + 9, C_DARK);
    bx = bx - 18;
    button(bx, by, 16, 14, 0);                    // maximise / restore
    gui_recto(bx + 3, by + 2, 9, 9, C_DARK); gui_rect(bx + 3, by + 3, 9, 1, C_DARK);
    bx = bx - 16;
    button(bx, by, 16, 14, 0);                    // minimise
    gui_rect(bx + 4, by + 9, 6, 2, C_DARK);
}

void draw_window(int i, int focused) {
    struct win *w;
    int tx;
    w = win_at(i);
    gui_rect(w->x, w->y, w->w, w->h, C_FACE);
    bevel(w->x, w->y, w->w, w->h, 1);
    if (focused) gradient(w->x + BORDER, w->y + BORDER, w->w - 2 * BORDER, TITLE_H, C_TITLE, C_TITLE2);
    else gradient(w->x + BORDER, w->y + BORDER, w->w - 2 * BORDER, TITLE_H, C_ITITLE, C_ITITLE2);
    tx = w->x + BORDER + 4;
    if (w->kind == K_TERM) { gui_rect(tx, w->y + BORDER + 3, 14, 11, C_DARK); gui_text2(tx + 2, w->y + BORDER + 3, 0x3dff9e, 9, GUI_MONO, 0, ">_", 2); tx = tx + 18; }
    text(tx, w->y + BORDER + 3, focused ? C_LIGHT : C_FACE, GUI_SANS_BOLD, w->title);
    caption_buttons(w);
    if (w->kind == K_TERM) {
        gui_rect(client_x(w), client_y(w), w->w - 2 * BORDER, w->h - (client_y(w) - w->y) - BORDER, C_DARK);
        bevel(client_x(w), client_y(w), COLS * CW + 4, ROWS * CH + 4, 0);
        term_draw(w, client_x(w) + 2, client_y(w) + 2, focused);
    } else about_draw(client_x(w), client_y(w), w->w - 2 * BORDER, w->h - (client_y(w) - w->y) - BORDER);
}

void icon(int x, int y, char *label, int kind) {
    int lw;
    if (kind == K_TERM) {
        gui_rect(x + 8, y, 32, 26, C_FACE); bevel(x + 8, y, 32, 26, 1);
        gui_rect(x + 11, y + 3, 26, 18, C_DARK);
        gui_text2(x + 13, y + 5, 0x3dff9e, 10, GUI_MONO, 0, ">_", 2);
        gui_rect(x + 18, y + 26, 12, 3, C_SHADOW); gui_rect(x + 12, y + 29, 24, 3, C_FACE);
    } else {
        gui_rect(x + 10, y + 2, 28, 28, C_LIGHT); gui_recto(x + 10, y + 2, 28, 28, C_DARK);
        gui_text2(x + 20, y + 5, C_TITLE, 20, GUI_SANS_BOLD, 0, "i", 1);
    }
    lw = slen(label) * 6;
    text(x + 24 - lw / 2, y + 36, C_LIGHT, GUI_SANS, label);
}

char *menu_items[4];
int n_items;

void draw_menu() {
    int mx, my, mw, mh, k, iy;
    mw = 180; mh = n_items * 26 + 12;
    mx = 2; my = SH - TASK_H - mh;
    gui_rect(mx, my, mw, mh, C_FACE);
    bevel(mx, my, mw, mh, 1);
    gradient(mx + 3, my + 3, 22, mh - 6, C_TITLE, C_TITLE2);
    k = 0;
    while (k < n_items) {
        iy = my + 6 + k * 26;
        if (k == n_items - 1) { gui_rect(mx + 28, iy - 3, mw - 32, 1, C_SHADOW); gui_rect(mx + 28, iy - 2, mw - 32, 1, C_LIGHT); }
        if (k == menu_hot) gui_rect(mx + 26, iy, mw - 30, 24, C_SEL);
        text(mx + 36, iy + 6, k == menu_hot ? C_LIGHT : C_DARK, GUI_SANS, menu_items[k]);
        ++k;
    }
}

int task_button_x(int k) { return 64 + k * 150; }

void draw_taskbar() {
    int k, i, bx, secs, h, m;
    char *p;
    struct win *w;
    gui_rect(0, SH - TASK_H, SW, TASK_H, C_FACE);
    gui_rect(0, SH - TASK_H, SW, 1, C_FACE);
    gui_rect(0, SH - TASK_H + 1, SW, 1, C_LIGHT);
    button(2, SH - TASK_H + 4, 56, 22, menu_open);
    gui_rect(8, SH - TASK_H + 9, 5, 5, 0xff2020); gui_rect(14, SH - TASK_H + 9, 5, 5, 0x20c020);
    gui_rect(8, SH - TASK_H + 15, 5, 5, 0x2040ff); gui_rect(14, SH - TASK_H + 15, 5, 5, 0xffd020);
    text(23, SH - TASK_H + 9, C_DARK, GUI_SANS_BOLD, "Start");
    // one button per window, in the order they were opened
    k = 0; i = 0;
    while (i < MAXW) {
        w = win_at(i);
        if (w->used) {
            bx = task_button_x(k);
            button(bx, SH - TASK_H + 4, 146, 22, i == focus_index());
            text(bx + 8, SH - TASK_H + 9, C_DARK, i == focus_index() ? GUI_SANS_BOLD : GUI_SANS, w->title);
            ++k;
        }
        ++i;
    }
    // the tray and its clock
    gui_rect(SW - 76, SH - TASK_H + 4, 72, 22, C_FACE);
    bevel(SW - 76, SH - TASK_H + 4, 72, 22, 0);
    secs = gui_clock();
    h = secs / 3600; m = (secs / 60) % 60;
    p = tmp;
    if (h % 12 == 0) p = itoa2(p, 12); else p = itoa2(p, h % 12);
    if (tmp[0] == '0') { tmp[0] = tmp[1]; p = tmp + 1; }
    *p = ':'; ++p;
    p = itoa2(p, m);
    *p = ' '; ++p; *p = h < 12 ? 'A' : 'P'; ++p; *p = 'M'; ++p; *p = 0;
    text(SW - 64, SH - TASK_H + 9, C_DARK, GUI_SANS, tmp);
}

void redraw() {
    int k, f;
    gui_clear(C_DESK);
    icon(20, 20, "Terminal", K_TERM);
    icon(20, 100, "About", K_ABOUT);
    f = focus_index();
    k = 0;
    while (k < nz) {
        if (!win_at(order[k])->min) draw_window(order[k], order[k] == f);
        ++k;
    }
    draw_taskbar();
    if (menu_open) draw_menu();
    gui_show();
    dirty = 0;
}

// ---- actions -----------------------------------------------------------------

void close_window(int i) {
    struct win *w;
    w = win_at(i);
    if (w->kind == K_TERM) term_stop(w);
    w->used = 0;
    unlink_z(i);
    if (drag == i + 1) drag = 0;
    dirty = 1;
}

void toggle_max(struct win *w) {
    if (!w->max) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->x = 0; w->y = 0; w->w = SW; w->h = SH - TASK_H; w->max = 1;
    } else {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh; w->max = 0;
    }
    dirty = 1;
}

void open_about() {
    int i, k;
    k = 0;
    while (k < MAXW) { if (win_at(k)->used && win_at(k)->kind == K_ABOUT) { win_at(k)->min = 0; raise(k); return; } ++k; }
    i = new_window(K_ABOUT, "About C4IX Desktop", 460, 170);
    if (i >= 0) { win_at(i)->x = (SW - 460) / 2; win_at(i)->y = (SH - 170) / 2 - 60; }
}

void menu_pick(int k) {
    menu_open = 0; dirty = 1;
    if (k == 0) open_terminal();
    else if (k == 1) open_about();
    else if (k == n_items - 1) running = 0;
}

int menu_hit(int x, int y) {
    int mh, my;
    mh = n_items * 26 + 12;
    my = SH - TASK_H - mh;
    if (!in_rect(x, y, 2, my, 180, mh)) return 0 - 1;
    if (x < 28) return 0 - 1;
    return (y - my - 6) / 26 < n_items ? (y - my - 6) / 26 : 0 - 1;
}

void mouse_down(int x, int y) {
    int k, i, bx, by, t;
    struct win *w;
    if (menu_open) {
        k = menu_hit(x, y);
        menu_open = 0; dirty = 1;
        if (k >= 0) menu_pick(k);
        if (!in_rect(x, y, 2, SH - TASK_H + 4, 56, 22)) return;
        return;
    }
    if (y >= SH - TASK_H) {
        if (in_rect(x, y, 2, SH - TASK_H + 4, 56, 22)) { menu_open = 1; menu_hot = 0 - 1; dirty = 1; return; }
        k = 0; i = 0;
        while (i < MAXW) {
            if (win_at(i)->used) {
                if (in_rect(x, y, task_button_x(k), SH - TASK_H + 4, 146, 22)) {
                    w = win_at(i);
                    if (i == focus_index() && !w->min) w->min = 1;
                    else { w->min = 0; raise(i); }
                    dirty = 1;
                    return;
                }
                ++k;
            }
            ++i;
        }
        return;
    }
    // the windows, topmost first
    k = nz - 1;
    while (k >= 0) {
        i = order[k];
        w = win_at(i);
        if (!w->min && in_rect(x, y, w->x, w->y, w->w, w->h)) {
            raise(i);
            by = w->y + BORDER + 2;
            bx = w->x + w->w - BORDER - 2 - 16;
            if (in_rect(x, y, bx, by, 16, 14)) { close_window(i); return; }
            if (in_rect(x, y, bx - 18, by, 16, 14)) { toggle_max(w); return; }
            if (in_rect(x, y, bx - 34, by, 16, 14)) { w->min = 1; dirty = 1; return; }
            if (y < w->y + BORDER + TITLE_H && !w->max) { drag = i + 1; drag_dx = x - w->x; drag_dy = y - w->y; }
            return;
        }
        --k;
    }
    // the desktop icons: a double click opens
    t = gui_ticks();
    k = 0 - 1;
    if (in_rect(x, y, 16, 16, 56, 64)) k = 0;
    else if (in_rect(x, y, 16, 96, 56, 64)) k = 1;
    if (k >= 0 && k == last_click_icon && t - last_click_t < 500) {
        if (k == 0) open_terminal(); else open_about();
        last_click_icon = 0 - 1;
    } else { last_click_icon = k; last_click_t = t; }
}

void mouse_move(int x, int y) {
    struct win *w;
    int k;
    if (drag) {
        w = win_at(drag - 1);
        w->x = x - drag_dx; w->y = y - drag_dy;
        if (w->y < 0) w->y = 0;
        if (w->y > SH - TASK_H - TITLE_H) w->y = SH - TASK_H - TITLE_H;
        if (w->x < 0 - w->w + 60) w->x = 0 - w->w + 60;
        if (w->x > SW - 60) w->x = SW - 60;
        dirty = 1;
    }
    if (menu_open) {
        k = menu_hit(x, y);
        if (k != menu_hot) { menu_hot = k; dirty = 1; }
    }
}

void key_down(int code, int ch, int mods) {
    int f;
    f = focus_index();
    if (f < 0) return;
    if (win_at(f)->kind == K_TERM) term_key(win_at(f), code, ch, mods);
}

// ---- main ---------------------------------------------------------------------

void shutdown_screen() {
    gui_clear(C_DARK);
    gui_text2(SW / 2 - 230, SH / 2 - 20, 0xff8c1a, 26, GUI_SANS_BOLD, 0, "It's now safe to turn off your computer.", 0 - 1);
    gui_show();
}

int main(int argc, char **argv) {
    int *region, k, busy, secs;

    if (!gui_present()) { printf("desktop: not fitted\n"); return 0; }
    region = ualloc(65536);
    if (!gui_attach(region, 65536, SW, SH)) { printf("desktop: the display refused the ring\n"); return 1; }
    gui_events(GUI_M_MOVE | GUI_M_BUTTONS | GUI_M_KEYS);
    wins = (struct win *)ualloc(MAXW * sizeof(struct win));
    k = 0;
    while (k < MAXW) { win_at(k)->used = 0; win_at(k)->cell = 0; ++k; }
    ev = ualloc(4 * sizeof(int));
    iobuf = (char *)ualloc(1024);
    pal[0] = 0x000000; pal[1] = 0xaa0000; pal[2] = 0x00aa00; pal[3] = 0xaa5500;
    pal[4] = 0x0000aa; pal[5] = 0xaa00aa; pal[6] = 0x00aaaa; pal[7] = 0xc0c0c0;
    pal[8] = 0x555555; pal[9] = 0xff5555; pal[10] = 0x55ff55; pal[11] = 0xffff55;
    pal[12] = 0x5555ff; pal[13] = 0xff55ff; pal[14] = 0x55ffff; pal[15] = 0xffffff;
    menu_items[0] = "Terminal"; menu_items[1] = "About C4IX Desktop"; menu_items[2] = "Shut Down...";
    n_items = 3;
    nz = 0; drag = 0; menu_open = 0; last_click_icon = 0 - 1;
    printf("desktop: running on the display. Start > Shut Down to come back here.\n");

    open_terminal();
    running = 1; dirty = 1;
    clock_min = 0 - 1;
    while (running) {
        busy = 0;
        while (gui_poll(ev)) {
            busy = 1;
            if (ev[0] == GUI_EV_DOWN && ev[3] == 0) mouse_down(ev[1], ev[2]);
            else if (ev[0] == GUI_EV_UP) drag = 0;
            else if (ev[0] == GUI_EV_MOVE) mouse_move(ev[1], ev[2]);
            else if (ev[0] == GUI_EV_KEYDOWN) key_down(ev[1], ev[2], ev[3]);
        }
        k = 0;
        while (k < MAXW) {
            if (win_at(k)->used && win_at(k)->kind == K_TERM) term_poll(win_at(k));
            ++k;
        }
        secs = gui_clock() / 60;
        if (secs != clock_min) { clock_min = secs; dirty = 1; }
        if (dirty) { redraw(); busy = 1; }
        if (!busy) umsleep(15);
    }

    k = 0;
    while (k < MAXW) { if (win_at(k)->used) close_window(k); ++k; }
    shutdown_screen();
    gui_detach();
    printf("desktop: shut down\n");
    return 0;
}
