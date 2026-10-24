// textgrid.c -- a VT100 text grid, with scrollback and a line editor.
// docs/grin-plan.md 3.5; it was the C4IX desktop's term.c.
//
// Output is drawn through a VT100 subset: SGR colours (the 16, xterm's
// 256, and 24-bit as the nearest of those), cursor movement, erasing.
// ESC _ ... ESC \ commands are handed to the window's code (g->apc): the
// desktop's Terminal draws window.h programs' canvases with them.
//
// Typing is edited here a line at a time, as a tty in cooked mode does:
// the cursor keys move within the line, Backspace and Delete take out a
// character, Up and Down bring back earlier lines, and Enter hands the
// line to the window's code to send. The line is drawn straight into the
// grid -- the window is its own tty -- from where it began.

#include "winkit.h"
#include "textgrid.h"

enum { COLS = TG_COLS, ROWS = TG_ROWS, CW = TG_CW, CH = TG_CH, RUNMAX = TG_RUNMAX,
       HIST_N = TG_HIST, HIST_W = TG_HIST_W, SB_ROWS = TG_SB_ROWS };

int tg_pal[256];
static char tmp[128];

// xterm's 256 colours: the 16 VGA ones, a 6x6x6 cube, 24 greys.
static int vga[16] = { 0x000000, 0xaa0000, 0x00aa00, 0xaa5500, 0x0000aa, 0xaa00aa, 0x00aaaa, 0xc0c0c0,
                       0x555555, 0xff5555, 0x55ff55, 0xffff55, 0x5555ff, 0xff55ff, 0x55ffff, 0xffffff };
static int level(int v) { if (!v) return 0; return 55 + 40 * v; }
static void palette() {
    int i, r, g, b, v;
    if (tg_pal[15]) return;
    i = 0;
    while (i < 16) { tg_pal[i] = vga[i]; ++i; }
    while (i < 232) {
        r = (i - 16) / 36; g = ((i - 16) / 6) % 6; b = (i - 16) % 6;
        tg_pal[i] = (level(r) << 16) | (level(g) << 8) | level(b);
        ++i;
    }
    while (i < 256) { v = 8 + 10 * (i - 232); tg_pal[i] = (v << 16) | (v << 8) | v; ++i; }
}

// A 24-bit colour, as the nearest step of the cube.
static int cube_step(int c) { if (c < 48) return 0; if (c < 115) return 1; return (c - 35) / 40; }

// ---- the grid -------------------------------------------------------------------

static void clear(struct tgrid *t, int from, int to) {
    int k;
    if (from >= to) return;
    k = from;
    while (k < to) { t->cell[k] = ' ' | (7 << 8) | (t->bg << 16); ++k; }
    k = from / COLS;
    while (k <= (to - 1) / COLS) { t->rowok[k] = 0; ++k; }
}

struct tgrid *tg_new(int win) {
    struct tgrid *t;
    palette();
    if (!(t = (struct tgrid *)wk_alloc(sizeof(struct tgrid)))) return 0;
    if (!(t->sb = (int *)wk_alloc(SB_ROWS * COLS * sizeof(int)))) { wk_free((char *)t); return 0; }
    t->win = win;
    clear(t, 0, ROWS * COLS);
    t->fg = 7; t->cursor = 1;
    return t;
}

void tg_free(struct tgrid *t) {
    if (!t) return;
    wk_free((char *)t->sb);
    wk_free((char *)t);
}

// A scroll moves the cache with the cells: the rows are the same rows.
// (The copies overlap; MCPY copies forwards, to a lower address, so each
// word is read before it is written.)
static void newline(struct tgrid *t) {
    t->cx = 0; t->wrap = 0;
    ++t->cy;
    if (t->cy >= ROWS) {
        memcpy(t->sb + t->sbhead * COLS, t->cell, COLS * sizeof(int));
        t->sbhead = (t->sbhead + 1) % SB_ROWS;
        if (t->sbn < SB_ROWS) ++t->sbn;
        // looking back: stay on the same rows while new ones arrive
        if (t->view && t->view < t->sbn) ++t->view;
        if (t->llen) --t->erow;
        memcpy(t->cell, t->cell + COLS, (ROWS - 1) * COLS * sizeof(int));
        memcpy(t->runs, t->runs + RUNMAX * 3, (ROWS - 1) * RUNMAX * 3 * sizeof(int));
        memcpy(t->rowok, t->rowok + 1, (ROWS - 1) * sizeof(int));
        memcpy(t->rowk, t->rowk + 1, (ROWS - 1) * sizeof(int));
        clear(t, (ROWS - 1) * COLS, ROWS * COLS);
        t->cy = ROWS - 1;
    }
}

static int attr(struct tgrid *t) {
    int f, b, x;
    f = t->fg; b = t->bg;
    if (t->bold && f < 8) f = f + 8;
    if (t->rev) { x = f; f = b; b = x; }
    return (f << 8) | (b << 16);
}

static void putc_(struct tgrid *t, int c) {
    if (t->wrap) newline(t);
    t->cell[t->cy * COLS + t->cx] = (c & 255) | attr(t);
    t->rowok[t->cy] = 0;
    ++t->cx;
    if (t->cx >= COLS) { t->cx = COLS - 1; t->wrap = 1; }
}

static void sgr(struct tgrid *t) {
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

static void csi(struct tgrid *t, int fin) {
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

void tg_feed(struct tgrid *t, char *buf, int n) {
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
            else if (t->alen < TG_APC) { t->abuf[t->alen] = c; ++t->alen; }
            continue;
        }
        if (t->esc == 4) {
            t->esc = 0;
            if (c == '\\') { t->abuf[t->alen] = 0; if (t->apc) t->apc(t->ctx, t->abuf, t->alen); }
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
    wk_invalidate(t->win, 0, 0, 0, 0);
}

void tg_echo(struct tgrid *t, int c) { tmp[0] = c; tg_feed(t, tmp, 1); }
void tg_say(struct tgrid *t, char *s) { tg_feed(t, s, ui_len(s)); }

// ---- line editing -------------------------------------------------------------------

// Put the cursor at character i of the line being typed.
static void ed_goto(struct tgrid *t, int i) {
    int k;
    k = t->erow * COLS + t->ecol + i;
    if (k < 0) k = 0;
    if (k >= ROWS * COLS) { t->cy = ROWS - 1; t->cx = COLS - 1; t->wrap = 1; return; }
    t->cy = k / COLS; t->cx = k % COLS; t->wrap = 0;
}

// Draw the line from character `from` to its end, then `extra` blanks
// over what a longer line left there, and put the cursor back at lpos.
static void ed_tail(struct tgrid *t, int from, int extra) {
    int i;
    ed_goto(t, from);
    i = from;
    while (i < t->llen) { putc_(t, t->line[i]); ++i; }
    while (extra > 0) { putc_(t, ' '); --extra; }
    ed_goto(t, t->lpos);
    wk_redraw_all();
}

// Show s (n characters) as the line, as Up and Down do.
static void ed_replace(struct tgrid *t, char *src, int n) {
    int old, i;
    old = t->llen;
    i = 0;
    while (i < n) { t->line[i] = src[i]; ++i; }
    t->llen = n;
    t->lpos = n;
    ed_tail(t, 0, old > n ? old - n : 0);
}

// The line is done: remember it, unless it is empty or the last one again.
static void ed_remember(struct tgrid *t) {
    int i, same;
    char *h;
    if (!t->llen) return;
    if (t->hn) {
        h = t->hist + (t->hn - 1) * HIST_W;
        same = 1;
        i = 0;
        while (i < t->llen && same) { if (h[i] != t->line[i]) same = 0; ++i; }
        if (same && !h[t->llen]) return;
    }
    if (t->hn == HIST_N) {
        memcpy(t->hist, t->hist + HIST_W, (HIST_N - 1) * HIST_W);
        --t->hn;
    }
    h = t->hist + t->hn * HIST_W;
    i = 0;
    while (i < t->llen) { h[i] = t->line[i]; ++i; }
    h[i] = 0;
    ++t->hn;
}

static void ed_history(struct tgrid *t, int dir) {
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
    h = t->hist + t->hpos * HIST_W;
    n = 0;
    while (h[n]) ++n;
    ed_replace(t, h, n);
}

void tg_scroll(struct tgrid *t, int rows) {
    t->view = t->view + rows;
    if (t->view > t->sbn) t->view = t->sbn;
    if (t->view < 0) t->view = 0;
    wk_redraw_all();
}

void tg_interrupt(struct tgrid *t) {
    t->view = 0;
    if (t->llen) ed_goto(t, t->llen);
    tg_say(t, "^C\n");
    t->llen = 0; t->lpos = 0; t->hpos = t->hn;
}

void tg_clear_screen(struct tgrid *t) {
    clear(t, 0, ROWS * COLS); t->cx = 0; t->cy = 0; wk_redraw_all();
    if (t->llen) { t->erow = 0; t->ecol = 0; ed_tail(t, 0, 0); }
}

int tg_key(struct tgrid *t, int code, int ch, int mods) {
    int i, n;
    // the scrollback: PgUp and PgDn, a screen at a time
    if (!ch && code == KEY_PGUP) { tg_scroll(t, ROWS - 1); return 0; }
    if (!ch && code == KEY_PGDN) { tg_scroll(t, 1 - ROWS); return 0; }
    // anything else is typing, which happens at the bottom
    if (t->view) { t->view = 0; wk_redraw_all(); }
    // a line starts where the cursor is when its first key comes
    if (!t->llen) {
        if (t->wrap) newline(t);
        t->erow = t->cy; t->ecol = t->cx; t->lpos = 0;
    }
    if (mods & M_CTRL) {
        if (ch == 'a' || ch == 'A' || code == 'A') { t->lpos = 0; ed_goto(t, 0); wk_redraw_all(); }
        else if (ch == 'e' || ch == 'E' || code == 'E') { t->lpos = t->llen; ed_goto(t, t->llen); wk_redraw_all(); }
        else if (ch == 'u' || ch == 'U' || code == 'U') ed_replace(t, t->line, 0);
        return 0;
    }
    if (code == KEY_ENTER || ch == 10) {
        ed_goto(t, t->llen);
        ed_remember(t);
        t->hpos = t->hn;
        t->line[t->llen] = 10;
        n = t->llen + 1;
        t->llen = 0; t->lpos = 0;
        tg_echo(t, 10);
        return n;
    }
    if (!ch) {
        if (code == KEY_LEFT) { if (t->lpos > 0) { --t->lpos; ed_goto(t, t->lpos); wk_redraw_all(); } return 0; }
        if (code == KEY_RIGHT) { if (t->lpos < t->llen) { ++t->lpos; ed_goto(t, t->lpos); wk_redraw_all(); } return 0; }
        if (code == KEY_HOME) { t->lpos = 0; ed_goto(t, 0); wk_redraw_all(); return 0; }
        if (code == KEY_END) { t->lpos = t->llen; ed_goto(t, t->llen); wk_redraw_all(); return 0; }
        if (code == KEY_UP) { ed_history(t, 0 - 1); return 0; }
        if (code == KEY_DOWN) { ed_history(t, 1); return 0; }
        if (code == KEY_DEL) {
            if (t->lpos < t->llen) {
                i = t->lpos;
                while (i < t->llen - 1) { t->line[i] = t->line[i + 1]; ++i; }
                --t->llen;
                ed_tail(t, t->lpos, 1);
            }
            return 0;
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
        return 0;
    }
    if (ch >= 32 && ch < 127 && t->llen < TG_LINE) {
        i = t->llen;
        while (i > t->lpos) { t->line[i] = t->line[i - 1]; --i; }
        t->line[t->lpos] = ch;
        ++t->llen;
        ++t->lpos;
        ed_tail(t, t->lpos - 1, 0);
    }
    return 0;
}

// the scroll bar beside the grid: its top is the oldest row kept
void tg_mouse(struct tgrid *t, int ev, int x, int y) {
    int r, bx, bh;
    bx = COLS * CW + 4; bh = ROWS * CH + 4;
    if (ev == E_WHEEL) { tg_scroll(t, 0 - 3 * y); return; }
    if (ev == E_MOVE) {
        r = ui_scroll_drag(bx, 0, y);
        if (r >= 0) { t->view = t->sbn - r; wk_redraw_all(); }
        return;
    }
    if (ev == E_DOWN) {
        r = ui_scroll_click(x, y, bx, 0, bh, t->sbn + ROWS, ROWS, t->sbn - t->view);
        if (r >= 0) { t->view = t->sbn - r; if (t->view < 0) t->view = 0; if (t->view > t->sbn) t->view = t->sbn; wk_redraw_all(); }
    }
}

// ---- drawing ----------------------------------------------------------------------

// Work out row r's runs: its rectangles of background, and its stretches
// of text in one colour that are not all blank -- what tg_draw would
// draw for it -- into the row cache. A row with more than RUNMAX gets
// rowk -1 and is drawn from its cells every time.
static void row_build(struct tgrid *t, int r) {
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
            if (k == RUNMAX) { t->rowk[r] = 0 - 1; t->rowok[r] = 1; return; }
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
            if (k == RUNMAX) { t->rowk[r] = 0 - 1; t->rowok[r] = 1; return; }
            run[3 * k] = start; run[3 * k + 1] = c - start; run[3 * k + 2] = (1 << 8) | f;
            ++k;
        }
    }
    t->rowk[r] = k;
    t->rowok[r] = 1;
}

// n characters of row r from column c, into tmp
static void row_text(struct tgrid *t, int r, int c, int n) {
    int i, *row;
    row = t->cell + r * COLS + c;
    i = 0;
    while (i < n) { tmp[i] = row[i] & 255; ++i; }
    tmp[n] = 0;
}

// A row from its cells, at screen row r: the grid's own, or one from the
// scrollback.
static void row_scan(int *row, int r, int ox, int oy) {
    int c, k, a, f, b, start;
    char *s;
    c = 0;
    while (c < COLS) {
        b = (row[c] >> 16) & 255;
        start = c;
        while (c < COLS && ((row[c] >> 16) & 255) == b) ++c;
        if (b) d_rect(ox + start * CW, oy + r * CH, (c - start) * CW, CH, tg_pal[b]);
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
        if (a) d_textn(ox + start * CW, oy + r * CH + 1, tg_pal[f], TG_FONT, F_MONO, CW, tmp, c - start);
    }
}

// The grid, a row at a time from the row cache (the scan of 2,000 cells
// twice over was a third of what an idle desktop's redraw cost), at
// ox, oy inside its sunken edge, its scroll bar beside that edge.
void tg_draw(struct tgrid *t, int ox, int oy, int focused) {
    int r, k, n, g, v, *run;
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
                d_textn(ox + run[0] * CW, oy + r * CH + 1, tg_pal[run[2] & 255], TG_FONT, F_MONO, CW, tmp, run[1]);
            } else d_rect(ox + run[0] * CW, oy + r * CH, run[1] * CW, CH, tg_pal[run[2]]);
            run = run + 3;
            ++k;
        }
        ++r;
    }
    if (focused && t->cursor && !t->view)
        d_rect(ox + t->cx * CW, oy + t->cy * CH + CH - 3, CW, 2, tg_pal[7]);
}
