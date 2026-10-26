// dosprmpt.c -- DOS Prompt windows (docs/grin-plan.md 3.7), and the rule
// they share with the full-screen launch (grin.c, grin_full): one DOS
// program at a time.
//
// A DOS Prompt is C4DOS's own command interpreter in a text window (the
// kit's textgrid.c). A line typed in it runs through DOS's exec (slot
// 19) with the console's sink (slot 24) pointed at the window, so DIR,
// TYPE, COPY, batch files and redirection behave exactly as at A>: they
// are the same code. The line runs from the top of grin's loop
// (dosprompt_top), never from inside the kit's dispatch. While it runs:
//   - what DOS and a bare.c program write appears as it is written;
//   - a bare.c program's read (conread) pumps grin's loop (grin_pump)
//     until a line is typed in its window, so the other windows carry on;
//   - a c4cc program never comes back to grin until it ends: grin waits,
//     and what it printed comes out of the console tap (REDIR.SYS TAP)
//     then, as in Windows 3.x's standard mode;
//   - the other DOS Prompts take typing but say "busy" on Enter, and a
//     full-screen program has to wait.
// EXIT closes the window. DOS's exec takes EXIT to end the exec, not
// DOS, so the window looks for the word itself.

#include "shell.h"
#include "textgrid.h"

enum { DP_LINE = TG_HIST_W, DP_IN = 1024, BUSY_N = 64 };

struct dosprompt {
    struct tgrid *g;
    int num;                          // "DOS Prompt 2": the lowest number free when it opened
    int drive;                        // this window's drive (DOS has one, for every window)
    int running;                      // its line is the one DOS is running
    int eof;                          // Ctrl+Z typed for its program: end of input, once
    int inlen;                        // typed for its program and not read yet
    char in[DP_IN];
    char cmd[DP_LINE];                // its line, waiting or running
};

int  dos_busy;                        // a DOS program, or a DOS Prompt's line, is running
char dos_busy_name[BUSY_N];           // what, for the message that says so
static int queued;                    // the window whose line waits for the top of the loop, or 0
static int last_draw;

static struct dosprompt *D(int w) { return (struct dosprompt *)wk_state(w); }

static int up(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

// the first word of a command line, in capitals: what dos_busy_name says
void dos_busy_set(char *cmdline) {
    int k;
    while (*cmdline == ' ') ++cmdline;
    k = 0;
    while (cmdline[k] && cmdline[k] != ' ' && k < BUSY_N - 1) { dos_busy_name[k] = up(cmdline[k]); ++k; }
    dos_busy_name[k] = 0;
}

// "One DOS program at a time": 1, after saying so, if one is running or
// waiting to
static char msg[BUSY_N + 160];
int dos_refuse() {
    if (!dos_busy && !queued && !grin_full_waiting()) return 0;
    msg[0] = 0;
    ui_ncat(msg, "A DOS program is running (", BUSY_N + 160);
    ui_ncat(msg, dos_busy_name, BUSY_N + 160);
    ui_ncat(msg, "). Let it finish before starting another.", BUSY_N + 160);
    wk_msgbox(0, 0, "grin", msg, WK_OK);
    return 1;
}

static void prompt(struct dosprompt *d) {
    char p[4];
    p[0] = 'A' + d->drive; p[1] = '>'; p[2] = 0;
    tg_say(d->g, p);
}

// is the line (no newline) EXIT, give or take spaces and case?
static int is_exit(char *s) {
    char *e;
    while (*s == ' ') ++s;
    e = "EXIT";
    while (*e && up(*s) == *e) { ++s; ++e; }
    while (*s == ' ') ++s;
    return !*e && !*s;
}

// ---- the window ------------------------------------------------------------------

static void dp_draw(int w, int cw, int ch, int focused) {
    struct dosprompt *d;
    d = D(w);
    d_rect(0, 0, cw, ch, C_DARK);
    d_bevel(0, 0, TG_W, TG_H, B_SUNKEN);
    if (d->g) tg_draw(d->g, 2, 2, focused);
}

static void dp_mouse(int w, int ev, int x, int y, int b) {
    struct dosprompt *d;
    d = D(w);
    if (d->g) tg_mouse(d->g, ev, x, ev == E_WHEEL ? b : y);
}

static void dp_key(int w, int down, int code, int ch, int mods) {
    struct dosprompt *d;
    int n, k;
    char *line;
    d = D(w);
    if (!down || !d->g) return;
    if ((mods & M_CTRL) && (ch == 'l' || ch == 'L' || code == 'L')) { tg_clear_screen(d->g); return; }
    // Ctrl+Z (or Ctrl+D): end of input for the program reading
    if (d->running && (mods & M_CTRL) && (ch == 'z' || ch == 'Z' || code == 'Z' || ch == 'd' || ch == 'D' || code == 'D')) {
        tg_say(d->g, "^Z\n");
        d->eof = 1;
        return;
    }
    if ((mods & M_CTRL) && (ch == 'c' || ch == 'C' || code == 'C')) { if (!d->running) { tg_interrupt(d->g); prompt(d); } return; }
    if ((n = tg_key(d->g, code, ch, mods)) <= 0) return;
    line = d->g->line;
    // its program is running: the line is the program's (dp_read)
    if (d->running) {
        k = 0;
        while (k < n && d->inlen < DP_IN) { d->in[d->inlen] = line[k]; ++d->inlen; ++k; }
        return;
    }
    // a command line
    k = 0;
    while (k < n - 1 && k < DP_LINE - 1) { d->cmd[k] = line[k]; ++k; }
    d->cmd[k] = 0;
    if (is_exit(d->cmd)) { wk_close(w); return; }
    k = 0;
    while (d->cmd[k] == ' ') ++k;
    if (!d->cmd[k]) { prompt(d); return; }
    if (dos_busy || queued || grin_full_waiting()) {
        tg_say(d->g, "grin: busy -- ");
        tg_say(d->g, dos_busy_name[0] ? dos_busy_name : "another DOS program");
        tg_say(d->g, " is running. One DOS program at a time: try again when it ends.\n");
        prompt(d);
        return;
    }
    queued = w;
}

// Closed while its line runs, the line's program would write into a
// window that is gone and wait for typing that cannot come: refused.
static int dp_close(int w) {
    struct dosprompt *d;
    d = D(w);
    if (d->running) {
        wk_msgbox(w, 0, "DOS Prompt", "A program is running in this window. End it first: Ctrl+Z gives it the end of its input.", WK_OK);
        return 0;
    }
    return 1;
}

static void dp_destroy(int w) {
    struct dosprompt *d;
    d = D(w);
    if (queued == w) queued = 0;
    if (d->g) { tg_free(d->g); d->g = 0; }
}

static struct wk_class dp_class = {
    .kind = ICON_TERM, .draw = dp_draw, .mouse = dp_mouse, .key = dp_key, .close = dp_close,
    .destroy = dp_destroy
};

// ---- the console's sink, while a window's line runs -------------------------------
//
// DOS calls these as the program that set the sink (grin), with whatever
// heap generation the program running has; reading pumps grin's loop, in
// grin's own generation, so what grin allocates meanwhile is grin's.

static int dp_write(int ctx, char *buf, int n) {
    struct dosprompt *d;
    if (n <= 0) return 0;
    if (!(d = D(ctx)) || !d->g) return n;
    tg_feed(d->g, buf, n);
    // shown as it comes, a frame at most every 50 ms: a program that
    // writes and does not read never comes back to grin's loop
    if (__time() - last_draw >= 50) { grin_draw_now(); last_draw = __time(); }
    return n;
}

static int dp_read(int ctx, char *buf, int n) {
    struct dosprompt *d;
    int k, i, old;
    if (n <= 0) return 0;
    old = gd_setgen(grin_heapgen());
    while ((d = D(ctx)) && !d->inlen && !d->eof) grin_pump();
    k = 0;
    if (d) {
        if (d->inlen) {
            while (k < d->inlen && k < n) { buf[k] = d->in[k]; ++k; if (buf[k - 1] == '\n') break; }
            i = 0;
            while (k + i < d->inlen) { d->in[i] = d->in[k + i]; ++i; }
            d->inlen = i;
        } else d->eof = 0;            // the end of input was read; the next read waits again
    }
    if (old >= 0) gd_setgen(old);
    return k;
}

// The queued line, from the top of grin's loop (no DOS program running).
static void run_line(int w) {
    struct dosprompt *d;
    char sel[4];
    queued = 0;
    if (!(d = D(w)) || !d->g) return;
    dos_busy = 1;
    dos_busy_set(d->cmd);
    d->running = 1; d->inlen = 0; d->eof = 0;
    // the window's own drive: DOS has one prompt drive for everyone
    if (gd_drive() != d->drive) { sel[0] = 'A' + d->drive; sel[1] = ':'; sel[2] = 0; gd_exec(sel); }
    last_draw = __time();
    gd_sink((int)dp_write, (int)dp_read, w);
    gd_exec(d->cmd);
    gd_sink(0, 0, 0);
    if ((d = D(w)) && d->g) {
        d->drive = gd_drive();
        d->running = 0; d->inlen = 0; d->eof = 0;
        prompt(d);
    }
    dos_busy = 0;
    dos_busy_name[0] = 0;
}

void dosprompt_top() { if (queued && !dos_busy) run_line(queued); }

// the lowest number no DOS Prompt has
static int free_num() {
    int n, w, used;
    n = 1;
    while (n < 99) {
        used = 0;
        w = 0;
        while ((w = wk_next(w))) if (wk_class_of(w) == &dp_class && D(w)->num == n) used = 1;
        if (!used) return n;
        ++n;
    }
    return n;
}

// A new DOS Prompt window; with cmd, that line is typed at its first
// prompt and runs (File Manager running a batch file).
int dosprompt_open(char *cmd) {
    struct dosprompt *d;
    int w, ww, hh, n;
    char title[24];
    dp_class.size = sizeof(struct dosprompt);
    n = free_num();
    ui_cpy(title, "DOS Prompt ");
    ui_num(title + ui_len(title), n);
    ww = TG_W + 2 * BORDER + TG_SB_W;
    hh = TG_H + 2 * BORDER + TITLE_H + 1;
    if (ww > wk.sw) ww = wk.sw;
    if (hh > wk.sh) hh = wk.sh;
    if ((w = wk_open(&dp_class, title, ww, hh, WK_FIXED, 0)) < 0) return w;
    d = D(w);
    d->num = n;
    if (!(d->g = tg_new(w))) { wk_close(w); return 0 - 1; }
    d->drive = gd_drive();
    tg_say(d->g, "C4DOS's command interpreter, in a window: DIR, TYPE, programs and batch files.\nEXIT closes it.\n\n");
    prompt(d);
    if (cmd && cmd[0]) {
        tg_say(d->g, cmd);
        tg_say(d->g, "\n");
        if (dos_busy || queued || grin_full_waiting()) {
            tg_say(d->g, "grin: busy -- another DOS program is running. Type the line again when it ends.\n");
            prompt(d);
        } else {
            ui_ncpy(d->cmd, cmd, DP_LINE);
            queued = w;
        }
    }
    return w;
}
