// term.c -- Command Prompt: a terminal running c4ix-sh over pipes.
// docs/c4ix-desktop.md.
//
// The shell is a real task whose stdin and stdout are pipes this desktop
// holds. Its output is drawn through a VT100 subset; typing is edited
// here a line at a time, as a tty in cooked mode does, and sent down the
// pipe on Enter. Ctrl-C interrupts the job running under this window's
// shell and nothing else.

#include "desktop.h"

enum { COLS = 80, ROWS = 24, CW = 8, CH = 16, FONT_PX = 14, LINE_MAX = 250 };

struct term {
    int pid, in_w, out_r, ended;
    int cx, cy, wrap, fg, bg, bold, rev, cursor;
    int esc, np, param[8], priv;
    int llen;
    char line[256];
    char pending[128];            // a command to type at the first prompt
    int cell[1920];               // ROWS * COLS: ch | fg << 8 | bg << 12
};

static int pal[16];
static char tmp[128];
static char iobuf[1024];

static struct term *T(struct win *w) { return (struct term *)w->st; }

static void palette() {
    pal[0] = 0x000000; pal[1] = 0xaa0000; pal[2] = 0x00aa00; pal[3] = 0xaa5500;
    pal[4] = 0x0000aa; pal[5] = 0xaa00aa; pal[6] = 0x00aaaa; pal[7] = 0xc0c0c0;
    pal[8] = 0x555555; pal[9] = 0xff5555; pal[10] = 0x55ff55; pal[11] = 0xffff55;
    pal[12] = 0x5555ff; pal[13] = 0xff55ff; pal[14] = 0x55ffff; pal[15] = 0xffffff;
}

// ---- the grid -------------------------------------------------------------------

static void clear(struct term *t, int from, int to) {
    int k;
    k = from;
    while (k < to) { t->cell[k] = ' ' | (7 << 8); ++k; }
}

static void newline(struct term *t) {
    t->cx = 0; t->wrap = 0;
    ++t->cy;
    if (t->cy >= ROWS) {
        memcpy(t->cell, t->cell + COLS, (ROWS - 1) * COLS * sizeof(int));
        clear(t, (ROWS - 1) * COLS, ROWS * COLS);
        t->cy = ROWS - 1;
    }
}

static int attr(struct term *t) {
    int f, b, x;
    f = t->fg; b = t->bg;
    if (t->bold && f < 8) f = f + 8;
    if (t->rev) { x = f; f = b; b = x; }
    return (f << 8) | (b << 12);
}

static void putc_(struct term *t, int c) {
    if (t->wrap) newline(t);
    t->cell[t->cy * COLS + t->cx] = (c & 255) | attr(t);
    ++t->cx;
    if (t->cx >= COLS) { t->cx = COLS - 1; t->wrap = 1; }
}

static void sgr(struct term *t) {
    int k, p;
    if (!t->np) { t->np = 1; t->param[0] = 0; }
    k = 0;
    while (k < t->np) {
        p = t->param[k];
        if (p == 0) { t->fg = 7; t->bg = 0; t->bold = 0; t->rev = 0; }
        else if (p == 1) t->bold = 1;
        else if (p == 22) t->bold = 0;
        else if (p == 7) t->rev = 1;
        else if (p == 27) t->rev = 0;
        else if (p >= 30 && p <= 37) t->fg = p - 30;
        else if (p == 39) t->fg = 7;
        else if (p >= 40 && p <= 47) t->bg = p - 40;
        else if (p == 49) t->bg = 0;
        else if (p >= 90 && p <= 97) t->fg = p - 90 + 8;
        else if (p >= 100 && p <= 107) t->bg = p - 100 + 8;
        else if (p == 38 || p == 48) {
            // only the 16 colours exist here: take an index that is one of them
            if (k + 2 < t->np && t->param[k + 1] == 5) {
                if (t->param[k + 2] < 16) { if (p == 38) t->fg = t->param[k + 2]; else t->bg = t->param[k + 2]; }
                k = k + 2;
            } else if (k + 1 < t->np && t->param[k + 1] == 2) k = k + 4;
        }
        ++k;
    }
}

static void csi(struct term *t, int fin) {
    int p0, p1, n;
    p0 = t->np > 0 ? t->param[0] : 0;
    p1 = t->np > 1 ? t->param[1] : 0;
    n = p0 ? p0 : 1;
    t->wrap = 0;
    if (fin == 'm') sgr(t);
    else if (fin == 'H' || fin == 'f') { t->cy = (p0 ? p0 : 1) - 1; t->cx = (p1 ? p1 : 1) - 1; }
    else if (fin == 'A') t->cy = t->cy - n;
    else if (fin == 'B') t->cy = t->cy + n;
    else if (fin == 'C') t->cx = t->cx + n;
    else if (fin == 'D') t->cx = t->cx - n;
    else if (fin == 'G') t->cx = n - 1;
    else if (fin == 'd') t->cy = n - 1;
    else if (fin == 'J') {
        if (p0 == 2 || p0 == 3) clear(t, 0, ROWS * COLS);
        else if (p0 == 1) clear(t, 0, t->cy * COLS + t->cx + 1);
        else clear(t, t->cy * COLS + t->cx, ROWS * COLS);
    }
    else if (fin == 'K') {
        if (p0 == 2) clear(t, t->cy * COLS, (t->cy + 1) * COLS);
        else if (p0 == 1) clear(t, t->cy * COLS, t->cy * COLS + t->cx + 1);
        else clear(t, t->cy * COLS + t->cx, (t->cy + 1) * COLS);
    }
    else if ((fin == 'h' || fin == 'l') && t->priv && p0 == 25) t->cursor = fin == 'h';
    if (t->cx < 0) t->cx = 0;
    if (t->cy < 0) t->cy = 0;
    if (t->cx >= COLS) t->cx = COLS - 1;
    if (t->cy >= ROWS) t->cy = ROWS - 1;
}

static void feed(struct term *t, char *buf, int n) {
    int i, c;
    i = 0;
    while (i < n) {
        c = buf[i] & 255;
        ++i;
        if (t->esc == 1) {
            if (c == '[') { t->esc = 2; t->np = 0; t->priv = 0; t->param[0] = 0; }
            else t->esc = 0;
            continue;
        }
        if (t->esc == 2) {
            if (c == '?') { t->priv = 1; continue; }
            if (c >= '0' && c <= '9') {
                if (!t->np) t->np = 1;
                t->param[t->np - 1] = t->param[t->np - 1] * 10 + (c - '0');
                continue;
            }
            if (c == ';') { if (!t->np) t->np = 1; if (t->np < 8) { t->param[t->np] = 0; ++t->np; } continue; }
            t->esc = 0;
            if (c >= 64 && c <= 126) csi(t, c);
            continue;
        }
        if (c == 27) t->esc = 1;
        else if (c == 10) newline(t);              // a tty turns \n into \r\n
        else if (c == 13) { t->cx = 0; t->wrap = 0; }
        else if (c == 8) { if (t->cx > 0) --t->cx; t->wrap = 0; }
        else if (c == 9) { t->cx = (t->cx + 8) & ~7; if (t->cx >= COLS) t->cx = COLS - 1; }
        else if (c >= 32) putc_(t, c);
    }
    mark_dirty();
}

static void echo(struct term *t, int c) { tmp[0] = c; feed(t, tmp, 1); }
static void say(struct term *t, char *s) { feed(t, s, ui_len(s)); }

// ---- the shell --------------------------------------------------------------------

// Our ends of the pipes are close-on-spawn, so no shell -- this one or a
// later window's -- inherits them: that is what lets a shell see end of
// file, and keeps each window's pipes its own.
static int start(struct term *t) {
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
    t->pid = spawn("c4ix-sh.c4r", 1, argv);
    udup2(s0, STDIN); udup2(s1, STDOUT); udup2(s2, STDERR);
    uclose(s0); uclose(s1); uclose(s2);
    uclose(pin[0]); uclose(pout[1]);
    t->in_w = pin[1];
    t->out_r = pout[0];
    if (t->pid < 0) { uclose(t->in_w); uclose(t->out_r); return 0; }
    return 1;
}

// A new Command Prompt; with cmd, that command is typed at its first
// prompt (Run, and Explorer running a program).
int term_open(char *cmd) {
    int i, k;
    struct win *w;
    struct term *t;
    palette();
    i = win_open(K_TERM, "Command Prompt", COLS * CW + 2 * BORDER + 4, ROWS * CH + 2 * BORDER + TITLE_H + 5);
    if (i < 0) return 0 - 1;
    w = win_get(i);
    w->resizable = 0;
    t = T(w);
    clear(t, 0, ROWS * COLS);
    t->fg = 7; t->cursor = 1;
    if (cmd) ui_cpy(t->pending, cmd);
    if (!start(t)) { say(t, "desktop: could not start a shell\n"); t->ended = 1; t->pid = 0 - 1; return i; }
    // "Command Prompt - task N": the id ps shows for this window's shell
    ui_cpy(tmp, "Command Prompt - task ");
    k = ui_len(tmp);
    ui_num(tmp + k, t->pid);
    win_title(w, tmp);
    return i;
}

// The window is going: stop the job running in it, then the shell.
void term_close(struct win *w) {
    struct term *t;
    t = T(w);
    if (t->ended) return;
    uintr(t->pid);
    ukill(t->pid, SIGTERM);
    uclose(t->in_w);
    uclose(t->out_r);
    uwait(t->pid);
    t->ended = 1;
}

// Output waiting in the pipe; never blocks.
void term_tick(struct win *w) {
    struct term *t;
    int n, k;
    t = T(w);
    if (t->ended) return;
    n = uavail(t->out_r);
    if (n > 0) {
        if (n > 1024) n = 1024;
        n = uread(t->out_r, iobuf, n);
        if (n > 0) {
            feed(t, iobuf, n);
            // the shell's prompt ends "$\n": time to type a waiting command
            if (t->pending[0] && n >= 2 && iobuf[n - 2] == '$' && iobuf[n - 1] == 10) {
                k = ui_len(t->pending);
                say(t, t->pending);
                t->pending[k] = 10;
                write(t->in_w, t->pending, k + 1);
                echo(t, 10);
                t->pending[0] = 0;
            }
        }
    } else if (n < 0) {
        // every writer is gone: the shell exited
        uclose(t->in_w);
        uclose(t->out_r);
        uwait(t->pid);
        t->ended = 1;
        say(t, "\n[the shell has exited; close this window]\n");
    }
}

// A key typed here: cooked-mode line editing, as a tty does it.
void term_key(struct win *w, int code, int ch, int mods) {
    struct term *t;
    t = T(w);
    if (t->ended) return;
    if ((mods & M_CTRL) && (ch == 'c' || ch == 'C' || code == 67)) {
        say(t, "^C\n");
        t->llen = 0;
        // nothing running under the shell: an empty line brings a fresh prompt
        if (uintr(t->pid) <= 0) write(t->in_w, "\n", 1);
        return;
    }
    if ((mods & M_CTRL) && (ch == 'l' || ch == 'L' || code == 76)) {
        clear(t, 0, ROWS * COLS); t->cx = 0; t->cy = 0; mark_dirty();
        return;
    }
    if (mods & M_CTRL) return;
    if (code == KEY_ENTER || ch == 10) {
        t->line[t->llen] = 10;
        write(t->in_w, t->line, t->llen + 1);
        t->llen = 0;
        echo(t, 10);
        return;
    }
    if (code == KEY_BACK) {
        if (t->llen > 0) { --t->llen; echo(t, 8); echo(t, ' '); echo(t, 8); }
        return;
    }
    if (ch >= 32 && ch < 127 && t->llen < LINE_MAX) {
        t->line[t->llen] = ch;
        ++t->llen;
        echo(t, ch);
    }
}

// The grid as runs: a rectangle per stretch of non-black background and a
// line of text per stretch of one colour.
void term_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    struct term *t;
    int r, c, k, a, f, b, start, *row;
    char *s;
    t = T(w);
    d_rect(ox, oy, cw, ch, C_DARK);
    d_bevel(ox, oy, COLS * CW + 4, ROWS * CH + 4, B_SUNKEN);
    ox = ox + 2; oy = oy + 2;
    r = 0;
    while (r < ROWS) {
        row = t->cell + r * COLS;
        c = 0;
        while (c < COLS) {
            b = (row[c] >> 12) & 15;
            start = c;
            while (c < COLS && ((row[c] >> 12) & 15) == b) ++c;
            if (b) d_rect(ox + start * CW, oy + r * CH, (c - start) * CW, CH, pal[b]);
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
            if (a) d_textn(ox + start * CW, oy + r * CH + 1, pal[f], FONT_PX, F_MONO, CW, tmp, c - start);
        }
        ++r;
    }
    if (focused && t->cursor && !t->ended)
        d_rect(ox + t->cx * CW, oy + t->cy * CH + CH - 3, CW, 2, pal[7]);
}
