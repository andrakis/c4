// term.c -- Terminal: a terminal running c4ix-sh over pipes.
// docs/c4ix-desktop.md.
//
// The shell is a real task whose stdin and stdout are pipes this desktop
// holds. Its output is drawn in the window kit's text grid (textgrid.c,
// a VT100 subset); typing is edited there a line at a time, as a tty in
// cooked mode does, and sent down the pipe on Enter. Ctrl-C interrupts
// the job running under this window's shell and nothing else, and drops
// whatever was typed that it had not read.
//
// A program may also make the window its canvas (include/window.h): it
// writes drawing commands as escape sequences, ESC _ G ... ESC \, and
// while it does the window shows the last frame it presented and sends
// the mouse and keys down its stdin instead of editing a line. The
// window becomes a terminal again when the program says so, when Ctrl-C
// stops it, or when the shell's prompt comes back.
//
// A canvas may show a framebuffer, the 'f' op: 8-bit pixels and a
// palette in the program's own memory, which GUI_BLIT8 has the host read
// and scale to the canvas. Once a frame with one has been drawn the
// program hears 'f' back, and may draw the next; and it hears keys let
// go ('K') as well as pressed, which a game needs.
//
// Its Edit menu is the desktop's clipboard, as a console window's is: the
// mouse selects (a run of lines, or with Alt held a block), Copy takes
// the selection and Copy All the scrollback and the screen, and Paste
// types the clipboard in, a key a character. Ctrl+Shift+C and V, Ctrl+Ins
// and Shift+Ins do the same; Ctrl+C copies when there is a selection, and
// is ^C when there is none.

#include "desktop.h"
#include "textgrid.h"

enum { IOBUF = 8192 };
// graphics mode: the canvas (the grid's own area), and the two display
// lists -- the one being built, and the one shown
enum { GW = 640, GH = 400, GLIST = 6000 };

struct term {
    int win;                      // its window's number
    int pid, in_w, out_r, ended;
    struct tgrid *g;              // the text
    char pending[128];            // a command to type at the first prompt
    int gfx, buttons;             // a canvas now; mouse buttons held on it
    int fpend;                    // a frame with a framebuffer presented, not yet drawn
    char title[48];               // the window's own title, while a program's shows
    int ncur, nshow;
    int gcur[GLIST];              // [len, op, numbers..., text]
    int gshow[GLIST];
};

static char tmp[128];
static char iobuf[IOBUF];
static char *edit_items[7] = { "Copy   Ctrl+Shift+C", "Paste   Ctrl+Shift+V", "-", "Select All", "Copy All", 0 };
static struct wk_menu menus[1];
enum { T_COPY = 0, T_PASTE = 1, T_ALL = 3, T_COPYALL = 4 };

static struct term *T(struct win *w) { return (struct term *)w->st; }

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

static void gfx_mode(struct term *t, int on) {
    if (on && !t->gfx) ui_ncpy(t->title, wk_get_title(t->win), 48);
    if (!on && t->gfx && t->title[0]) wk_title(t->win, t->title);
    if (on && t->g) tg_unselect(t->g);
    t->gfx = on;
    t->ncur = 0; t->nshow = 0; t->buttons = 0; t->fpend = 0;
    mark_dirty();
}

// One command, ESC _ and ESC \ already stripped: G, the op, numbers
// separated by commas, and for text a ';' and the string.
static void apc(char *ctx, char *abuf, int alen) {
    struct term *t;
    int i, n, v, neg, op, len, *g, k, nums[8];
    char *s;
    t = (struct term *)ctx;
    if (alen < 2 || abuf[0] != 'G') return;
    op = abuf[1];
    i = 2; n = 0;
    while (i < alen && abuf[i] != ';' && n < 8) {
        v = 0; neg = 0;
        if (abuf[i] == '-') { neg = 1; ++i; }
        while (i < alen && abuf[i] >= '0' && abuf[i] <= '9') { v = v * 10 + (abuf[i] - '0'); ++i; }
        nums[n] = neg ? 0 - v : v; ++n;
        if (i < alen && abuf[i] == ',') ++i;
        else break;
    }
    s = 0;
    if (i < alen && abuf[i] == ';') s = abuf + i + 1;
    if (op == 'o') {
        gfx_mode(t, 1);
        if (s && *s) { if (ui_len(s) > 44) s[44] = 0; wk_title(t->win, s); }
        event(t, 's', GW, GH, 0);
        return;
    }
    if (op == 'q') { gfx_mode(t, 0); return; }
    if (!t->gfx) return;
    if (op == 'p') {
        memcpy(t->gshow, t->gcur, t->ncur * sizeof(int));
        t->nshow = t->ncur;
        t->ncur = 0;
        // a framebuffer in it: tell the program once it has been drawn
        k = 0;
        while (k < t->nshow && t->gshow[k] >= 2) {
            if (t->gshow[k + 1] == 'f') t->fpend = 1;
            k = k + t->gshow[k];
        }
        wk_invalidate(t->win, 0, 0, 0, 0);
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
        else if (op == 'f' && n >= 5) d_blit8(ox, oy, g[3], g[4], (char *)g[2], g[5], (int *)g[6], GW, GH);
        if (g[0] < 2) break;
        i = i + g[0];
    }
}

// ---- the shell --------------------------------------------------------------------

// Our ends of the pipes are close-on-spawn, so no shell -- this one or a
// later window's -- inherits them: that is what lets a shell see end of
// file, and keeps each window's pipes its own.
static char *sh_argv[3] = { "c4ix-sh.c4r", "-p", 0 };   // -p: the prompt on the input line
static int start(struct term *t) {
    int pin[2], pout[2], s0, s1, s2;
    if (upipe(pin) < 0) return 0;
    if (upipe(pout) < 0) { uclose(pin[0]); uclose(pin[1]); return 0; }
    ucloexec(pin[1], 1);
    ucloexec(pout[0], 1);
    s0 = udup(STDIN); s1 = udup(STDOUT); s2 = udup(STDERR);
    ucloexec(s0, 1); ucloexec(s1, 1); ucloexec(s2, 1);
    udup2(pin[0], STDIN);
    udup2(pout[1], STDOUT);
    udup2(pout[1], STDERR);
    t->pid = spawn("c4ix-sh.c4r", 2, sh_argv);
    udup2(s0, STDIN); udup2(s1, STDOUT); udup2(s2, STDERR);
    uclose(s0); uclose(s1); uclose(s2);
    uclose(pin[0]); uclose(pout[1]);
    t->in_w = pin[1];
    t->out_r = pout[0];
    if (t->pid < 0) { uclose(t->in_w); uclose(t->out_r); return 0; }
    return 1;
}

// A new Terminal; with cmd, that command is typed at its first
// prompt (Run, and Files running a program).
int term_open(char *cmd) {
    int i, k;
    struct win *w;
    struct term *t;
    i = win_open(K_TERM, "Terminal", TG_W + 2 * BORDER + TG_SB_W, TG_H + 2 * BORDER + TITLE_H + 1 + MENU_H, sizeof(struct term));
    if (i < 0) return 0 - 1;
    w = win_get(i);
    w->resizable = 0;
    menus[0].title = "Edit"; menus[0].items = edit_items;
    wk_menu(i, menus, 1);
    t = T(w);
    t->win = i;
    if (!(t->g = tg_new(i))) { t->ended = 1; win_close(i); return 0 - 1; }   // no memory left for it
    t->g->apc = apc; t->g->ctx = (char *)t;
    if (cmd) ui_ncpy(t->pending, cmd, 128);
    if (!start(t)) { tg_say(t->g, "desktop: could not start a shell\n"); t->ended = 1; t->pid = 0 - 1; return i; }
    // "Terminal - task N": the id ps shows for this window's shell
    ui_cpy(tmp, "Terminal - task ");
    k = ui_len(tmp);
    ui_num(tmp + k, t->pid);
    win_title(w, tmp);
    return i;
}

// The window is going: stop the job running in it, then the shell.
void term_close(struct win *w) {
    struct term *t;
    t = T(w);
    if (t->g) { tg_free(t->g); t->g = 0; }
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
    n = uavail(t->out_r);
    got = 0;
    // Take everything the program has written before drawing: a full-screen
    // program writes a whole frame at once, and drawing half of one is flicker.
    while (n > 0 && got < 65536) {
        if (n > IOBUF) n = IOBUF;
        n = uread(t->out_r, iobuf, n);
        if (n <= 0) break;
        got = got + n;
        tg_feed(t->g, iobuf, n);
        // the shell's prompt ends "$ " (c4ix-sh -p) and is the last thing it writes
        prompt = n >= 2 && iobuf[n - 2] == '$' && iobuf[n - 1] == ' ';
        // the shell's prompt is back: whatever made this a canvas has ended
        if (t->gfx && prompt) gfx_mode(t, 0);
        // ... and time to type a waiting command
        if (t->pending[0] && prompt) {
            k = ui_len(t->pending);
            tg_say(t->g, t->pending);
            t->pending[k] = 10;
            write(t->in_w, t->pending, k + 1);
            tg_echo(t->g, 10);
            t->pending[0] = 0;
        }
        n = uavail(t->out_r);
    }
    if (n < 0) {
        // every writer is gone: the shell exited
        uclose(t->in_w);
        uclose(t->out_r);
        uwait(t->pid);
        t->ended = 1;
        tg_say(t->g, "\n[the shell has exited; close this window]\n");
    }
}

// A key let go: only a canvas hears it.
void term_keyup(struct win *w, int code, int ch, int mods) {
    struct term *t;
    t = T(w);
    if (t->ended || !t->gfx) return;
    event(t, 'K', code, ch, mods);
}

// The desktop has drawn: a framebuffer it showed has been read.
void term_drawn(struct win *w) {
    struct term *t;
    t = T(w);
    if (!t->fpend) return;
    t->fpend = 0;
    if (t->gfx && !t->ended) event(t, 'f', 0, 0, 0);
}

// The clipboard typed in, as keys: a line at a time down the pipe, as
// typing it would send them; to a canvas, each as a key.
static void paste(struct win *w, struct term *t) {
    char *s;
    int n, k, c;
    s = wk_clip_get(&n);
    k = 0;
    while (k < n && !t->ended) {
        c = s[k] & 255;
        if (c == 9) c = ' ';
        if (t->gfx) { if (c == 10) event(t, 'k', KEY_ENTER, 13, 0); else if (c >= 32 && c < 127) event(t, 'k', 0, c, 0); }
        else if (c == 10) { if ((c = tg_key(t->g, KEY_ENTER, 0, 0)) > 0) write(t->in_w, t->g->line, c); }
        else if (c >= 32 && c < 127) tg_key(t->g, 0, c, 0);
        ++k;
    }
    wk_invalidate(w->id, 0, 0, 0, 0);
}

void term_command(struct win *w, int menu, int item) {
    struct term *t;
    t = T(w);
    if (!t->g) return;
    if (item == T_COPY) { if (tg_copy(t->g, 0)) tg_unselect(t->g); }
    else if (item == T_COPYALL) tg_copy(t->g, 1);
    else if (item == T_ALL) tg_select_all(t->g);
    else if (item == T_PASTE && !t->ended) { tg_unselect(t->g); paste(w, t); }
}

// the letter key, with Ctrl and Shift held
static int ctrl_shift(int code, int ch, int mods, int c) {
    return (mods & M_CTRL) && (mods & M_SHIFT) && (code == c || ch == c || ch == c + 32);
}

// A key typed here.
void term_key(struct win *w, int code, int ch, int mods) {
    struct term *t;
    int n;
    t = T(w);
    if (t->g && (ctrl_shift(code, ch, mods, 'C') || ((mods & M_CTRL) && code == KEY_INS) ||
                 (t->g->sel && (mods & M_CTRL) && (ch == 'c' || ch == 'C' || code == 'C')))) { term_command(w, 0, T_COPY); return; }
    if (t->g && (ctrl_shift(code, ch, mods, 'V') || ((mods & M_SHIFT) && code == KEY_INS))) { term_command(w, 0, T_PASTE); return; }
    if (t->ended) return;
    // the modifiers alone are a canvas's business; anything else typed drops a selection
    if (code == 16 || code == 17 || code == 18) { if (t->gfx) event(t, 'k', code, ch, mods); return; }
    tg_unselect(t->g);
    if ((mods & M_CTRL) && (ch == 'c' || ch == 'C' || code == 'C')) {
        // What the program wrote before the interrupt shows first, as a
        // tty's output queue would have it: drawn after the ^C, the last
        // frame of a full-screen program (top) would clear it away.
        term_tick(w);
        if (t->ended) return;
        if (t->gfx) gfx_mode(t, 0);
        tg_interrupt(t->g);
        // Lines typed at the job and not read by it go with it, as a tty
        // flushes its input on an interrupt: left in the pipe, the shell
        // would read them next and run them as commands of its own.
        udiscard(t->in_w);
        // nothing running under the shell: an empty line brings a fresh prompt
        if (uintr(t->pid) <= 0) write(t->in_w, "\n", 1);
        return;
    }
    if ((mods & M_CTRL) && (ch == 'l' || ch == 'L' || code == 'L')) { tg_clear_screen(t->g); return; }
    if (t->gfx) { event(t, 'k', code, ch, mods); return; }
    if ((n = tg_key(t->g, code, ch, mods)) > 0) write(t->in_w, t->g->line, n);
}

void term_mouse(struct win *w, int ev, int x, int y, int cw, int ch) {
    struct term *t;
    t = T(w);
    if (!t->g) return;
    if (!t->gfx) {
        // the grid selects, its scroll bar scrolls
        if (ev == E_WHEEL || ui_scroll_active() || (ev != E_UP && x >= TG_W)) { tg_mouse(t->g, ev, x, y); return; }
        tg_select(t->g, ev, x - 2, y - 2, (wk.mods & M_ALT) != 0);
        return;
    }
    if (t->ended) return;
    x = x - 2; y = y - 2;                          // inside the sunken edge
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x >= GW) x = GW - 1; if (y >= GH) y = GH - 1;
    if (ev == E_DOWN) { t->buttons = 1; event(t, 'd', x, y, 0); }
    else if (ev == E_UP) { if (t->buttons) event(t, 'u', x, y, 0); t->buttons = 0; }
    else if (ev == E_MOVE) event(t, 'm', x, y, t->buttons);
}

void term_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    struct term *t;
    t = T(w);
    d_rect(ox, oy, cw, ch, C_DARK);
    d_bevel(ox, oy, TG_W, TG_H, B_SUNKEN);
    if (t->gfx) { gfx_draw(t, ox + 2, oy + 2); return; }
    if (t->g) tg_draw(t->g, ox + 2, oy + 2, focused && !t->ended);
}
