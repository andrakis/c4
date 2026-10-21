// term.c -- Command Prompt: a terminal running c4ix-sh over pipes.
// docs/c4ix-desktop.md.
//
// The shell is a real task whose stdin and stdout are pipes this desktop
// holds. Its output is drawn through a VT100 subset; typing is edited
// here a line at a time, as a tty in cooked mode does, and sent down the
// pipe on Enter. Ctrl-C interrupts the job running under this window's
// shell and nothing else.
//
// A program may also make the window its canvas (include/window.h): it
// writes drawing commands as escape sequences, ESC _ G ... ESC \, and
// while it does the window shows the last frame it presented and sends
// the mouse and keys down its stdin instead of editing a line. The
// window becomes a terminal again when the program says so, when Ctrl-C
// stops it, or when the shell's prompt comes back.

#include "desktop.h"

enum { COLS = 80, ROWS = 25, CW = 8, CH = 16, FONT_PX = 14, LINE_MAX = 250, IOBUF = 8192 };
// graphics mode: the canvas (the grid's own area), a command buffer, and
// the two display lists -- the one being built, and the one shown
enum { GW = 640, GH = 400, APC_MAX = 300, GLIST = 6000 };
// the row cache: what term_draw emits for each row, at most RUNMAX runs
enum { RUNMAX = 24 };
// line editing's history (HIST_N lines), and the scrollback: SB_ROWS rows
// that scrolled off the top, beside the grid with its scrollbar (SB_W)
enum { HIST_N = 16, SB_ROWS = 300, SB_W = 16 };

struct term {
    int pid, in_w, out_r, ended;
    int cx, cy, wrap, fg, bg, bold, rev, cursor;
    int esc, np, param[8], priv;
    int llen;
    char line[256];
    char pending[128];            // a command to type at the first prompt
    int cell[2000];               // ROWS * COLS: ch | fg << 8 | bg << 16, xterm-256 indices
    int gfx, buttons;             // a canvas now; mouse buttons held on it
    char title[48];               // the window's own title, while a program's shows
    int alen;
    char abuf[304];               // an ESC _ ... ESC \ command being read
    int ncur, nshow;
    int gcur[6000];               // GLIST words: [len, op, numbers..., text]
    int gshow[6000];
    // The row cache (row_build): each row's background rectangles and
    // runs of text, as [first column, columns, kind << 8 | colour] with
    // kind 0 a rectangle and 1 text. rowok[r] is 0 until it is built, and
    // again whenever a cell of the row changes; rowk[r] is its run count,
    // or -1 for a row with more than RUNMAX, drawn from the cells.
    int rowok[25], rowk[25];
    int runs[1800];               // ROWS * RUNMAX * 3
    // Line editing (term_key): the cursor's place in the line being
    // typed, and where on the grid that line starts (erow may go above
    // the top as the line scrolls). The history: hn lines, oldest first;
    // hpos is the one shown, hn for the line being typed, kept in saved
    // while the history is browsed.
    int lpos, erow, ecol;
    int hn, hpos, slen;
    char hist[4096];              // HIST_N * 256
    char saved[256];
    // The scrollback: a ring of rows that scrolled off the top, sbn of
    // them, the next going in at sbhead; view is how many rows back the
    // window shows (0, the live grid). Allocated when the window opens.
    int *sb;
    int sbn, sbhead, view;
};

static int pal[256];
static char tmp[128];
static char iobuf[IOBUF];

static struct term *T(struct win *w) { return (struct term *)w->st; }
static struct win *feeding;       // the window whose output feed() is reading

// Output changed only the terminal it went to (wm.c's damage), when that
// is the window being fed; anything else -- an echo, a say -- comes with
// input, which draws the whole scene anyway.
static void feed_dirty(struct term *t) {
    if (feeding && T(feeding) == t) mark_win_dirty(feeding);
    else mark_dirty();
}

// xterm's 256 colours: the 16 VGA ones, a 6x6x6 cube, 24 greys.
static int level(int v) { if (!v) return 0; return 55 + 40 * v; }
static void palette() {
    int i, r, g, b, v;
    pal[0] = 0x000000; pal[1] = 0xaa0000; pal[2] = 0x00aa00; pal[3] = 0xaa5500;
    pal[4] = 0x0000aa; pal[5] = 0xaa00aa; pal[6] = 0x00aaaa; pal[7] = 0xc0c0c0;
    pal[8] = 0x555555; pal[9] = 0xff5555; pal[10] = 0x55ff55; pal[11] = 0xffff55;
    pal[12] = 0x5555ff; pal[13] = 0xff55ff; pal[14] = 0x55ffff; pal[15] = 0xffffff;
    i = 16;
    while (i < 232) {
        r = (i - 16) / 36; g = ((i - 16) / 6) % 6; b = (i - 16) % 6;
        pal[i] = (level(r) << 16) | (level(g) << 8) | level(b);
        ++i;
    }
    while (i < 256) { v = 8 + 10 * (i - 232); pal[i] = (v << 16) | (v << 8) | v; ++i; }
}

// A 24-bit colour, as the nearest step of the cube.
static int cube_step(int c) { if (c < 48) return 0; if (c < 115) return 1; return (c - 35) / 40; }

// ---- the grid -------------------------------------------------------------------

static void clear(struct term *t, int from, int to) {
    int k;
    if (from >= to) return;
    k = from;
    while (k < to) { t->cell[k] = ' ' | (7 << 8) | (t->bg << 16); ++k; }
    k = from / COLS;
    while (k <= (to - 1) / COLS) { t->rowok[k] = 0; ++k; }
}

// A scroll moves the cache with the cells: the rows are the same rows.
static void newline(struct term *t) {
    t->cx = 0; t->wrap = 0;
    ++t->cy;
    if (t->cy >= ROWS) {
        if (t->sb) {
            memcpy(t->sb + t->sbhead * COLS, t->cell, COLS * sizeof(int));
            t->sbhead = (t->sbhead + 1) % SB_ROWS;
            if (t->sbn < SB_ROWS) ++t->sbn;
            // looking back: stay on the same rows while new ones arrive
            if (t->view && t->view < t->sbn) ++t->view;
        }
        if (t->llen) --t->erow;
        memcpy(t->cell, t->cell + COLS, (ROWS - 1) * COLS * sizeof(int));
        memcpy(t->runs, t->runs + RUNMAX * 3, (ROWS - 1) * RUNMAX * 3 * sizeof(int));
        memcpy(t->rowok, t->rowok + 1, (ROWS - 1) * sizeof(int));
        memcpy(t->rowk, t->rowk + 1, (ROWS - 1) * sizeof(int));
        clear(t, (ROWS - 1) * COLS, ROWS * COLS);
        t->cy = ROWS - 1;
    }
}

static int attr(struct term *t) {
    int f, b, x;
    f = t->fg; b = t->bg;
    if (t->bold && f < 8) f = f + 8;
    if (t->rev) { x = f; f = b; b = x; }
    return (f << 8) | (b << 16);
}

static void putc_(struct term *t, int c) {
    if (t->wrap) newline(t);
    t->cell[t->cy * COLS + t->cx] = (c & 255) | attr(t);
    t->rowok[t->cy] = 0;
    ++t->cx;
    if (t->cx >= COLS) { t->cx = COLS - 1; t->wrap = 1; }
}

static void sgr(struct term *t) {
    int k, p, v;
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
            // 38;5;N / 48;5;N: one of the 256; 38;2;R;G;B: the nearest of them
            if (k + 2 < t->np && t->param[k + 1] == 5) {
                v = t->param[k + 2] & 255;
                if (p == 38) t->fg = v; else t->bg = v;
                k = k + 2;
            } else if (k + 4 < t->np && t->param[k + 1] == 2) {
                v = 16 + 36 * cube_step(t->param[k + 2]) + 6 * cube_step(t->param[k + 3]) + cube_step(t->param[k + 4]);
                if (p == 38) t->fg = v; else t->bg = v;
                k = k + 4;
            }
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

// ---- graphics mode ------------------------------------------------------------------

static void send(struct term *t, char *s) { write(t->in_w, s, ui_len(s)); }

// An event down the program's stdin: ESC E kind a,b,c newline.
static void event(struct term *t, int kind, int a, int b, int c) {
    int k;
    tmp[0] = 27; tmp[1] = 'E'; tmp[2] = kind; tmp[3] = 0;
    ui_num(tmp + 3, a); k = ui_len(tmp); tmp[k] = ','; tmp[k + 1] = 0;
    ui_num(tmp + k + 1, b); k = ui_len(tmp); tmp[k] = ','; tmp[k + 1] = 0;
    ui_num(tmp + k + 1, c); k = ui_len(tmp); tmp[k] = 10; tmp[k + 1] = 0;
    send(t, tmp);
}

static void gfx_mode(struct win *w, struct term *t, int on) {
    if (on && !t->gfx) ui_cpy(t->title, w->title);
    if (!on && t->gfx && t->title[0]) win_title(w, t->title);
    t->gfx = on;
    t->ncur = 0; t->nshow = 0; t->buttons = 0;
    mark_dirty();
}

// One command, ESC _ and ESC \ already stripped: G, the op, numbers
// separated by commas, and for text a ';' and the string.
static void apc(struct win *w, struct term *t) {
    int i, n, v, neg, op, len, *g, k, nums[8];
    char *s;
    t->abuf[t->alen] = 0;
    if (t->alen < 2 || t->abuf[0] != 'G') return;
    op = t->abuf[1];
    i = 2; n = 0;
    while (i < t->alen && t->abuf[i] != ';' && n < 8) {
        v = 0; neg = 0;
        if (t->abuf[i] == '-') { neg = 1; ++i; }
        while (i < t->alen && t->abuf[i] >= '0' && t->abuf[i] <= '9') { v = v * 10 + (t->abuf[i] - '0'); ++i; }
        nums[n] = neg ? 0 - v : v; ++n;
        if (i < t->alen && t->abuf[i] == ',') ++i;
        else break;
    }
    s = 0;
    if (i < t->alen && t->abuf[i] == ';') s = t->abuf + i + 1;
    if (op == 'o') {
        gfx_mode(w, t, 1);
        if (s && *s) { if (ui_len(s) > 44) s[44] = 0; win_title(w, s); }
        event(t, 's', GW, GH, 0);
        return;
    }
    if (op == 'q') { gfx_mode(w, t, 0); return; }
    if (!t->gfx) return;
    if (op == 'p') {
        memcpy(t->gshow, t->gcur, t->ncur * sizeof(int));
        t->nshow = t->ncur;
        t->ncur = 0;
        feed_dirty(t);
        return;
    }
    // everything else is drawn later: into the list being built
    len = 2 + n + (s ? 1 + (ui_len(s) + 4) / 4 : 0);
    if (t->ncur + len > GLIST) return;           // a frame too big: the rest is dropped
    g = t->gcur + t->ncur;
    g[0] = len; g[1] = op;
    k = 0;
    while (k < n) { g[2 + k] = nums[k]; ++k; }
    if (s) { g[2 + n] = ui_len(s); ui_cpy((char *)(g + 3 + n), s); }
    t->ncur = t->ncur + len;
}

static void gfx_draw(struct term *t, int ox, int oy) {
    int i, *g, n, op;
    d_rect(ox, oy, GW, GH, 0);
    i = 0;
    while (i < t->nshow) {
        g = t->gshow + i;
        op = g[1];
        n = g[0] - 2;
        if (op == 'c' && n >= 1) d_rect(ox, oy, GW, GH, g[2]);
        else if (op == 'r' && n >= 5) d_rect(ox + g[2], oy + g[3], g[4], g[5], g[6]);
        else if (op == 'R' && n >= 5) d_recto(ox + g[2], oy + g[3], g[4], g[5], g[6]);
        else if (op == 'l' && n >= 5) d_line(ox + g[2], oy + g[3], ox + g[4], oy + g[5], g[6]);
        else if (op == 'd' && n >= 4) d_disc(ox + g[2], oy + g[3], g[4], g[5]);
        else if (op == 't' && n >= 6) d_textn(ox + g[2], oy + g[3], g[4], g[5], F_SANS, 0, (char *)(g + 7), g[6]);
        if (g[0] < 2) break;
        i = i + g[0];
    }
}

static void feed(struct term *t, char *buf, int n) {
    int i, c;
    i = 0;
    while (i < n) {
        c = buf[i] & 255;
        ++i;
        if (t->esc == 1) {
            if (c == '[') { t->esc = 2; t->np = 0; t->priv = 0; t->param[0] = 0; }
            else if (c == '_') { t->esc = 3; t->alen = 0; }
            else t->esc = 0;
            continue;
        }
        if (t->esc == 3) {                         // inside ESC _ ... ESC \ 
            if (c == 27) t->esc = 4;
            else if (t->alen < APC_MAX) { t->abuf[t->alen] = c; ++t->alen; }
            continue;
        }
        if (t->esc == 4) {
            t->esc = 0;
            if (c == '\\') apc(feeding, t);
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
    feed_dirty(t);
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
    argv = (char **)ualloc(3 * sizeof(char *));
    argv[0] = "c4ix-sh.c4r";
    argv[1] = "-p";                                // the prompt on the input line
    argv[2] = 0;
    t->pid = spawn("c4ix-sh.c4r", 2, argv);
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
    i = win_open(K_TERM, "Command Prompt", COLS * CW + 2 * BORDER + 4 + SB_W, ROWS * CH + 2 * BORDER + TITLE_H + 5);
    if (i < 0) return 0 - 1;
    w = win_get(i);
    w->resizable = 0;
    t = T(w);
    t->sb = (int *)ualloc(SB_ROWS * COLS * sizeof(int));
    if (!t->sb) { t->ended = 1; win_close(i); return 0 - 1; }   // no memory left for it
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
    if (t->sb) { free(t->sb); t->sb = 0; }
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
    int n, k, got, prompt;
    t = T(w);
    if (t->ended) return;
    feeding = w;
    n = uavail(t->out_r);
    got = 0;
    // Take everything the program has written before drawing: a full-screen
    // program writes a whole frame at once, and drawing half of one is flicker.
    while (n > 0 && got < 65536) {
        if (n > IOBUF) n = IOBUF;
        n = uread(t->out_r, iobuf, n);
        if (n <= 0) break;
        got = got + n;
        {
            feed(t, iobuf, n);
            // the shell's prompt ends "$ " (c4ix-sh -p) and is the last thing it writes
            prompt = n >= 2 && iobuf[n - 2] == '$' && iobuf[n - 1] == ' ';
            // the shell's prompt is back: whatever made this a canvas has ended
            if (t->gfx && prompt) gfx_mode(w, t, 0);
            // ... and time to type a waiting command
            if (t->pending[0] && prompt) {
                k = ui_len(t->pending);
                say(t, t->pending);
                t->pending[k] = 10;
                write(t->in_w, t->pending, k + 1);
                echo(t, 10);
                t->pending[0] = 0;
            }
        }
        n = uavail(t->out_r);
    }
    if (n < 0) {
        // every writer is gone: the shell exited
        uclose(t->in_w);
        uclose(t->out_r);
        uwait(t->pid);
        t->ended = 1;
        say(t, "\n[the shell has exited; close this window]\n");
    }
}

// ---- line editing -------------------------------------------------------------------
//
// Cooked mode, as a tty does it, with a line editor: the line is sent
// to the shell only when Enter is pressed, and until then the cursor
// keys move within it, Backspace and Delete take out a character there,
// and Up and Down bring back earlier lines. The line is drawn straight
// into the grid -- this terminal is its own tty -- from where it began.

// Put the cursor at character i of the line being typed.
static void ed_goto(struct term *t, int i) {
    int k;
    k = t->erow * COLS + t->ecol + i;
    if (k < 0) k = 0;
    if (k >= ROWS * COLS) { t->cy = ROWS - 1; t->cx = COLS - 1; t->wrap = 1; return; }
    t->cy = k / COLS; t->cx = k % COLS; t->wrap = 0;
}

// Draw the line from character `from` to its end, then `extra` blanks
// over what a longer line left there, and put the cursor back at lpos.
static void ed_tail(struct term *t, int from, int extra) {
    int i;
    ed_goto(t, from);
    i = from;
    while (i < t->llen) { putc_(t, t->line[i]); ++i; }
    while (extra > 0) { putc_(t, ' '); --extra; }
    ed_goto(t, t->lpos);
    mark_dirty();
}

// Show s (n characters) as the line, as Up and Down do.
static void ed_replace(struct term *t, char *src, int n) {
    int old, i;
    old = t->llen;
    i = 0;
    while (i < n) { t->line[i] = src[i]; ++i; }
    t->llen = n;
    t->lpos = n;
    ed_tail(t, 0, old > n ? old - n : 0);
}

// The line is done: remember it, unless it is empty or the last one again.
static void ed_remember(struct term *t) {
    int i, same;
    char *h;
    if (!t->llen) return;
    if (t->hn) {
        h = t->hist + (t->hn - 1) * 256;
        same = 1;
        i = 0;
        while (i < t->llen && same) { if (h[i] != t->line[i]) same = 0; ++i; }
        if (same && !h[t->llen]) return;
    }
    if (t->hn == HIST_N) {
        memcpy(t->hist, t->hist + 256, (HIST_N - 1) * 256);
        --t->hn;
    }
    h = t->hist + t->hn * 256;
    i = 0;
    while (i < t->llen) { h[i] = t->line[i]; ++i; }
    h[i] = 0;
    ++t->hn;
}

static void ed_history(struct term *t, int dir) {
    char *h;
    int n;
    if (dir < 0) {
        if (t->hpos <= 0) return;
        if (t->hpos == t->hn) {                // leaving the line being typed: keep it
            memcpy(t->saved, t->line, t->llen);
            t->slen = t->llen;
        }
        --t->hpos;
    } else {
        if (t->hpos >= t->hn) return;
        ++t->hpos;
    }
    if (t->hpos == t->hn) { ed_replace(t, t->saved, t->slen); return; }
    h = t->hist + t->hpos * 256;
    n = 0;
    while (h[n]) ++n;
    ed_replace(t, h, n);
}

// The scrollback: show `rows` rows further back (negative: forward).
static void sb_scroll(struct term *t, int rows) {
    t->view = t->view + rows;
    if (t->view > t->sbn) t->view = t->sbn;
    if (t->view < 0) t->view = 0;
    mark_dirty();
}

// A key typed here.
void term_key(struct win *w, int code, int ch, int mods) {
    struct term *t;
    int i;
    t = T(w);
    if (t->ended) return;
    if ((mods & M_CTRL) && (ch == 'c' || ch == 'C' || code == 67)) {
        // What the program wrote before the interrupt shows first, as a
        // tty's output queue would have it: drawn after the ^C, the last
        // frame of a full-screen program (top) would clear it away.
        term_tick(w);
        if (t->ended) return;
        if (t->gfx) gfx_mode(w, t, 0);
        t->view = 0;
        if (t->llen) ed_goto(t, t->llen);
        say(t, "^C\n");
        t->llen = 0; t->lpos = 0; t->hpos = t->hn;
        // nothing running under the shell: an empty line brings a fresh prompt
        if (uintr(t->pid) <= 0) write(t->in_w, "\n", 1);
        return;
    }
    if ((mods & M_CTRL) && (ch == 'l' || ch == 'L' || code == 76)) {
        clear(t, 0, ROWS * COLS); t->cx = 0; t->cy = 0; mark_dirty();
        if (t->llen) { t->erow = 0; t->ecol = 0; ed_tail(t, 0, 0); }
        return;
    }
    if (t->gfx) { event(t, 'k', code, ch, mods); return; }
    // the scrollback: PgUp and PgDn, a screen at a time
    if (!ch && code == KEY_PGUP) { sb_scroll(t, ROWS - 1); return; }
    if (!ch && code == KEY_PGDN) { sb_scroll(t, 1 - ROWS); return; }
    // anything else is typing, which happens at the bottom
    if (t->view) { t->view = 0; mark_dirty(); }
    // a line starts where the cursor is when its first key comes
    if (!t->llen) {
        if (t->wrap) newline(t);
        t->erow = t->cy; t->ecol = t->cx; t->lpos = 0;
    }
    if (mods & M_CTRL) {
        if (ch == 'a' || ch == 'A' || code == 65) { t->lpos = 0; ed_goto(t, 0); mark_dirty(); }
        else if (ch == 'e' || ch == 'E' || code == 69) { t->lpos = t->llen; ed_goto(t, t->llen); mark_dirty(); }
        else if (ch == 'u' || ch == 'U' || code == 85) ed_replace(t, t->line, 0);
        return;
    }
    if (code == KEY_ENTER || ch == 10) {
        ed_goto(t, t->llen);
        ed_remember(t);
        t->hpos = t->hn;
        t->line[t->llen] = 10;
        write(t->in_w, t->line, t->llen + 1);
        t->llen = 0; t->lpos = 0;
        echo(t, 10);
        return;
    }
    if (!ch) {
        if (code == KEY_LEFT) { if (t->lpos > 0) { --t->lpos; ed_goto(t, t->lpos); mark_dirty(); } return; }
        if (code == KEY_RIGHT) { if (t->lpos < t->llen) { ++t->lpos; ed_goto(t, t->lpos); mark_dirty(); } return; }
        if (code == KEY_HOME) { t->lpos = 0; ed_goto(t, 0); mark_dirty(); return; }
        if (code == KEY_END) { t->lpos = t->llen; ed_goto(t, t->llen); mark_dirty(); return; }
        if (code == KEY_UP) { ed_history(t, -1); return; }
        if (code == KEY_DOWN) { ed_history(t, 1); return; }
        if (code == KEY_DEL) {
            if (t->lpos < t->llen) {
                i = t->lpos;
                while (i < t->llen - 1) { t->line[i] = t->line[i + 1]; ++i; }
                --t->llen;
                ed_tail(t, t->lpos, 1);
            }
            return;
        }
    }
    if (code == KEY_BACK) {
        if (t->lpos > 0) {
            i = t->lpos - 1;
            while (i < t->llen - 1) { t->line[i] = t->line[i + 1]; ++i; }
            --t->llen;
            --t->lpos;
            ed_tail(t, t->lpos, 1);
        }
        return;
    }
    if (ch >= 32 && ch < 127 && t->llen < LINE_MAX) {
        i = t->llen;
        while (i > t->lpos) { t->line[i] = t->line[i - 1]; --i; }
        t->line[t->lpos] = ch;
        ++t->llen;
        ++t->lpos;
        ed_tail(t, t->lpos - 1, 0);
    }
}

void term_mouse(struct win *w, int ev, int x, int y, int cw, int ch) {
    struct term *t;
    int r, bx, bh;
    t = T(w);
    if (t->ended) return;
    if (!t->gfx) {
        // the scrollbar beside the grid: its top is the oldest row kept
        bx = COLS * CW + 4; bh = ROWS * CH + 4;
        if (ev == E_WHEEL) { sb_scroll(t, 0 - 3 * y); return; }
        if (ev == E_MOVE) {
            r = ui_scroll_drag(bx, 0, y);
            if (r >= 0) { t->view = t->sbn - r; mark_dirty(); }
            return;
        }
        if (ev == E_DOWN) {
            r = ui_scroll_click(x, y, bx, 0, bh, t->sbn + ROWS, ROWS, t->sbn - t->view);
            if (r >= 0) { t->view = t->sbn - r; if (t->view < 0) t->view = 0; if (t->view > t->sbn) t->view = t->sbn; mark_dirty(); }
        }
        return;
    }
    x = x - 2; y = y - 2;                          // inside the sunken edge
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x >= GW) x = GW - 1; if (y >= GH) y = GH - 1;
    if (ev == E_DOWN) { t->buttons = 1; event(t, 'd', x, y, 0); }
    else if (ev == E_UP) { if (t->buttons) event(t, 'u', x, y, 0); t->buttons = 0; }
    else if (ev == E_MOVE) event(t, 'm', x, y, t->buttons);
}

// The grid as runs: a rectangle per stretch of non-black background and a
// line of text per stretch of one colour.
// Work out row r's runs: its rectangles of background, and its stretches
// of text in one colour that are not all blank -- what term_draw would
// draw for it -- into the row cache. A row with more than RUNMAX gets
// rowk -1 and is drawn from its cells every time.
static void row_build(struct term *t, int r) {
    int c, k, f, b, start, blank, *row, *run;
    row = t->cell + r * COLS;
    run = t->runs + r * RUNMAX * 3;
    k = 0;
    c = 0;
    while (c < COLS) {
        b = (row[c] >> 16) & 255;
        start = c;
        while (c < COLS && ((row[c] >> 16) & 255) == b) ++c;
        if (b) {
            if (k == RUNMAX) { t->rowk[r] = -1; t->rowok[r] = 1; return; }
            run[3 * k] = start; run[3 * k + 1] = c - start; run[3 * k + 2] = b;
            ++k;
        }
    }
    c = 0;
    while (c < COLS) {
        f = (row[c] >> 8) & 255;
        start = c;
        blank = 1;
        while (c < COLS && ((row[c] >> 8) & 255) == f) { if ((row[c] & 255) != ' ') blank = 0; ++c; }
        if (!blank) {
            if (k == RUNMAX) { t->rowk[r] = -1; t->rowok[r] = 1; return; }
            run[3 * k] = start; run[3 * k + 1] = c - start; run[3 * k + 2] = (1 << 8) | f;
            ++k;
        }
    }
    t->rowk[r] = k;
    t->rowok[r] = 1;
}

// n characters of row r from column c, into tmp
static void row_text(struct term *t, int r, int c, int n) {
    int i, *row;
    row = t->cell + r * COLS + c;
    i = 0;
    while (i < n) { tmp[i] = row[i] & 255; ++i; }
    tmp[n] = 0;
}

// A row from its cells, as term_draw always used to do it, at screen
// row r: the grid's own, or one from the scrollback.
static void row_scan(int *row, int r, int ox, int oy) {
    int c, k, a, f, b, start;
    char *s;
    c = 0;
    while (c < COLS) {
        b = (row[c] >> 16) & 255;
        start = c;
        while (c < COLS && ((row[c] >> 16) & 255) == b) ++c;
        if (b) d_rect(ox + start * CW, oy + r * CH, (c - start) * CW, CH, pal[b]);
    }
    c = 0;
    while (c < COLS) {
        f = (row[c] >> 8) & 255;
        start = c;
        s = tmp;
        while (c < COLS && ((row[c] >> 8) & 255) == f && c - start < 90) { *s = row[c] & 255; ++s; ++c; }
        *s = 0;
        k = 0; a = 0;
        while (tmp[k]) { if (tmp[k] != ' ') a = 1; ++k; }
        if (a) d_textn(ox + start * CW, oy + r * CH + 1, pal[f], FONT_PX, F_MONO, CW, tmp, c - start);
    }
}

// The grid, a row at a time from the row cache: the scan of 2,000 cells
// twice over was a third of what an idle desktop's redraw cost, for rows
// that had not changed since the last one.
void term_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    struct term *t;
    int r, k, n, g, v, *run;
    t = T(w);
    d_rect(ox, oy, cw, ch, C_DARK);
    d_bevel(ox, oy, COLS * CW + 4, ROWS * CH + 4, B_SUNKEN);
    ox = ox + 2; oy = oy + 2;
    if (t->gfx) { gfx_draw(t, ox, oy); return; }
    // Looking back, the top `view` rows come from the scrollback, the
    // newest of them last; the grid's rows follow, lower by as many.
    ui_scrollbar(ox - 2 + COLS * CW + 4, oy - 2, ROWS * CH + 4, t->sbn + ROWS, ROWS, t->sbn - t->view);
    v = t->view;
    r = 0;
    while (r < v && r < ROWS) {
        row_scan(t->sb + ((t->sbhead - v + r + 2 * SB_ROWS) % SB_ROWS) * COLS, r, ox, oy);
        ++r;
    }
    while (r < ROWS) {
        g = r - v;
        if (!t->rowok[g]) row_build(t, g);
        n = t->rowk[g];
        if (n < 0) row_scan(t->cell + g * COLS, r, ox, oy);
        run = t->runs + g * RUNMAX * 3;
        k = 0;
        while (k < n) {
            if (run[2] >> 8) {
                row_text(t, g, run[0], run[1]);
                d_textn(ox + run[0] * CW, oy + r * CH + 1, pal[run[2] & 255], FONT_PX, F_MONO, CW, tmp, run[1]);
            } else d_rect(ox + run[0] * CW, oy + r * CH, run[1] * CW, CH, pal[run[2]]);
            run = run + 3;
            ++k;
        }
        ++r;
    }
    if (focused && t->cursor && !t->ended && !t->view)
        d_rect(ox + t->cx * CW, oy + t->cy * CH + CH - 3, CW, 2, pal[7]);
}
