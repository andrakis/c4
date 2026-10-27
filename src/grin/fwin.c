// fwin.c -- a DOS program's frames in a grin window (docs/grin-plan.md 3.6,
// G6): how the games play beside grin's applications.
//
// A game's bare platform layer finds grin's table in C4DOS's slot 25
// (gkit.h) and, instead of taking the whole display, calls three entries
// of it:
//   fwin_open(title, w, h, flags)   a window for w x h frames: 1, or 0
//   fwin_frame(pixels, w, h, pitch, pal)   a frame: 8-bit, through 256
//                                   colours 0x00RRGGBB
//   fwin_poll(ev)                   grin's loop once (grin_pump), then the
//                                   window's next event in gui.h's form:
//                                   1, 0 for none, -1 once it has closed
// The frame is copied into grin's buffer and the window damaged; the blit
// goes out with grin's next redraw, which the poll makes at once, and the
// host copies the pixels at the bell. Keys reach the game only while its
// window has the focus; a game that steers with the mouse (FW_MOUSE) has
// it locked to the display by a click on the window, and Esc lets it go.
// Mouse positions are in the frame's pixels.
//
// The game is a DOS program: one at a time (dos_busy), run from the top of
// grin's loop (fwin_run, from grin.c) without the display being given up,
// or typed in a DOS Prompt. While it runs, grin's loop runs only inside
// its polls, which are DOS's code calling grin's: so each poll runs in
// grin's heap generation (3.3) and as grin's image (C4DOS v8, image()),
// so that what grin allocates, and an application started meanwhile, are
// grin's and not the game's. Its window is closed at the top of the loop
// once the program has ended, whichever way it ended.

#include <stdlib.h>
#include <string.h>
#include "shell.h"
#include "wkgui.h"

enum { FW_MOUSE = 1 };                // fwin_open's flags: the game steers with the mouse
enum { QN = 128, OUT_N = 200, SAY_N = 320 };

static int win;                       // the game's window, or 0
static int fw, fh, flags, scale;      // its frames' size, its flags, 1x or 2x
static int closed;                    // the window has gone: polls say -1
static int ever;                      // a window was opened in this run (fwin_run)
static char *pix;                     // the last frame, grin's copy
static int *pal;
static int fresh;                     // a frame waits to be drawn
static int q[QN * 4];                 // events for the game
static int qh, qt;
static int pumped;                    // the loop ran since the poll said "none"
static int locked;                    // the mouse is locked to the display, for the game
static int rx, ry, mx, my;            // motion short of a frame pixel; the pointer, in the frame
static int held[8];                   // the keys the game was told are down, a bit each
static int had_focus;
static int me;                        // grin's image (C4DOS v8), 0 without

static void push(int t, int a, int b, int c) {
    int n;
    n = (qt + 1) % QN;
    if (n == qh) return;              // full: dropped
    q[qt * 4] = t; q[qt * 4 + 1] = a; q[qt * 4 + 2] = b; q[qt * 4 + 3] = c;
    qt = n;
}

// every key the game holds let go, as its window lost them
static void let_go() {
    int c;
    c = 0;
    while (c < 256) {
        if (held[c >> 5] & (1 << (c & 31))) push(GUI_EV_KEYUP, c, 0, 0);
        ++c;
    }
    c = 0;
    while (c < 8) { held[c] = 0; ++c; }
}

static void unlock() {
    if (!locked) return;
    locked = 0;
    gui_cursor(GUI_PTR_ARROW);        // the display lets the mouse go
    wk_repoint();
}

// ---- the window ----------------------------------------------------------------------

static int clampx(int x) { return x < 0 ? 0 : x >= fw ? fw - 1 : x; }
static int clampy(int y) { return y < 0 ? 0 : y >= fh ? fh - 1 : y; }

static void fw_draw(int w, int cw, int ch, int focused) {
    if (!pix) { d_rect(0, 0, cw, ch, 0x000000); return; }
    d_blit8(0, 0, fw, fh, pix, fw, pal, fw * scale, fh * scale);
    fresh = 0;
}

// the mouse over the window, not locked: in the frame's pixels, for a
// game that wants it (a double click is its first click and a second)
static void fw_mouse(int w, int ev, int x, int y, int b) {
    if (!(flags & FW_MOUSE)) return;
    mx = clampx(x / scale); my = clampy(y / scale);
    if (ev == E_MOVE) push(GUI_EV_MOVE, mx, my, b);
    else if (ev == E_DOWN) push(GUI_EV_DOWN, mx, my, b);
    else if (ev == E_UP) push(GUI_EV_UP, mx, my, b);
}

// a game steered by the mouse: over its frame the pointer says "click to
// capture" (GUI_PTR_LOCK), and the click locks it (libjs/display.js)
static int fw_pointer(int w, int x, int y) { return flags & FW_MOUSE ? GUI_PTR_LOCK : PTR_ARROW; }

static void fw_destroy(int w) {
    if (w != win) return;
    closed = 1;
    unlock();
}

// the window's size for frames at scale s
static int win_w(int s) { return fw * s + 2 * BORDER; }
static int win_h(int s) { return fh * s + 2 * BORDER + TITLE_H + 1 + MENU_H; }
static int fits(int s) { return win_w(s) <= wk.aw && win_h(s) <= wk.ah; }

static void resize(int s) {
    struct win *p;
    int x, y;
    if (!(p = win_get(win)) || s == scale) return;
    if (fw * s > WK_MAX_W || fh * s > WK_MAX_H) return;
    scale = s;
    x = p->x; y = p->y;
    if (x + win_w(s) > wk.ax + wk.aw) x = wk.ax + wk.aw - win_w(s);
    if (y + win_h(s) > wk.ay + wk.ah) y = wk.ay + wk.ah - win_h(s);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    wk_place(win, x, y, win_w(s), win_h(s));
    wk_redraw_all();
}

static char *size_items[3] = { "1x  320 x 200", "2x  640 x 400", 0 };
static struct wk_menu fw_menus[1] = { { "Size", size_items } };

static void fw_command(int w, int id) {
    if (id == WK_CMD(0, 0)) resize(1);
    else if (id == WK_CMD(0, 1)) resize(2);
}

static struct wk_class fw_class = {
    .kind = ICON_TERM, .draw = fw_draw, .mouse = fw_mouse, .command = fw_command,
    .destroy = fw_destroy, .pointer = fw_pointer
};

// ---- before the kit: what is the game's of the display's events ----------------------
//
// Called by grin's loop for every event before the kit sees it; 1 when it
// was the game's. Keys go to the game while its window has the focus and
// no dialog or menu holds them, all but Ctrl+Esc (the Task List). They go
// straight to it, not through the kit, which draws the whole scene again
// after every key. A locked mouse is the game's alone: its motion (it
// comes as GUI_EV_REL, as grin's event mask asks) and its buttons. The
// first plain move after it says Esc let it go.
int fwin_event(int *ev) {
    int t, c, dx, dy;
    if (!win || closed) return 0;
    t = ev[0];
    if (t == GUI_EV_REL) {
        locked = 1;
        rx = rx + ev[1]; ry = ry + ev[2];
        dx = rx / scale; dy = ry / scale;
        rx = rx - dx * scale; ry = ry - dy * scale;
        mx = clampx(mx + dx); my = clampy(my + dy);
        if (dx || dy) push(GUI_EV_REL, dx, dy, ev[3]);
        return 1;
    }
    if (locked) {
        if (t == GUI_EV_MOVE) { locked = 0; wk_repoint(); return 0; }
        if (t == GUI_EV_DOWN || t == GUI_EV_UP) { push(t, mx, my, ev[3]); return 1; }
        if (t == GUI_EV_WHEEL) return 1;
    }
    if (t == GUI_EV_FOCUS && !ev[1]) { let_go(); return 0; }   // the page lost the keyboard
    if ((t == GUI_EV_KEYDOWN || t == GUI_EV_KEYUP) && wk_focus() == win && !wk_modal() && !wk_menu_open()) {
        if (ev[1] == KEY_ESC && (ev[3] & M_CTRL)) return 0;
        wk.mods = ev[3];
        c = ev[1] & 255;
        if (t == GUI_EV_KEYDOWN) held[c >> 5] = held[c >> 5] | (1 << (c & 31));
        else held[c >> 5] = held[c >> 5] & ~(1 << (c & 31));
        push(t, ev[1], ev[2], ev[3]);
        return 1;
    }
    return 0;
}

// a frame waits to be drawn: grin's loop draws at once, not 25 ms later
int fwin_fresh() { return win && fresh; }

// After a pass: the game told when its window gains or loses the focus,
// and what it held let go when it loses it (and the mouse with it)
static void focus_check() {
    int f;
    f = !closed && wk_focus() == win;
    if (f == had_focus) return;
    had_focus = f;
    if (!f) { let_go(); unlock(); }
    push(GUI_EV_FOCUS, f, 0, 0);
}

// ---- the table's entries (gkit.h), called by the game ---------------------------------

static int open_win(char *title, int w, int h, int fl) {
    int i;
    if (win || w <= 0 || h <= 0 || w > WK_MAX_W || h > WK_MAX_H) return 0;
    if (!(pix = (char *)malloc(w * h))) return 0;
    if (!(pal = (int *)malloc(256 * sizeof(int)))) { free(pix); pix = 0; return 0; }
    memset(pix, 0, w * h);
    memset((char *)pal, 0, 256 * sizeof(int));
    fw = w; fh = h; flags = fl;
    closed = 0; fresh = 0; qh = 0; qt = 0; pumped = 0;
    locked = 0; rx = 0; ry = 0; mx = w / 2; my = h / 2;
    i = 0;
    while (i < 8) { held[i] = 0; ++i; }
    scale = fits(2) ? 2 : 1;
    fw_class.size = 16;
    if ((win = wk_open(&fw_class, title, win_w(scale), win_h(scale), WK_FIXED, 0)) <= 0) {
        win = 0;
        free(pix); free((char *)pal); pix = 0; pal = 0;
        return 0;
    }
    wk_menu(win, fw_menus, 1);
    had_focus = wk_focus() == win;
    ever = 1;
    return 1;
}

int grin_fwin_open(char *title, int w, int h, int fl) {
    int old, ok;
    old = gd_setgen(grin_heapgen());
    ok = open_win(title, w, h, fl);
    if (old >= 0) gd_setgen(old);
    return ok;
}

// copied, so that a redraw for another window's sake has a whole frame to show
void grin_fwin_frame(char *p, int w, int h, int pitch, int *colours) {
    int y;
    if (!win || closed || w != fw || h != fh || !p || !colours) return;
    if (pitch == w) memcpy(pix, p, w * h);
    else { y = 0; while (y < h) { memcpy(pix + y * w, p + y * pitch, w); ++y; } }
    memcpy((char *)pal, (char *)colours, 256 * sizeof(int));
    fresh = 1;
    wk_invalidate(win, 0, 0, fw * scale, fh * scale);
}

int grin_fwin_poll(int *ev) {
    int og, oi, k;
    if (!win || closed) return 0 - 1;
    if (qh == qt && !pumped) {
        og = gd_setgen(grin_heapgen());
        oi = me ? gd_image(me) : 0;
        grin_pump_game();
        focus_check();
        if (oi) gd_image(oi);
        if (og >= 0) gd_setgen(og);
        pumped = 1;
        if (!win || closed) return 0 - 1;
    }
    if (qh == qt) { pumped = 0; return 0; }
    k = qh * 4;
    ev[0] = q[k]; ev[1] = q[k + 1]; ev[2] = q[k + 2]; ev[3] = q[k + 3];
    qh = (qh + 1) % QN;
    return 1;
}

// ---- grin's side: starting one, and after it ----------------------------------------

void fwin_init() { me = gd_image(0); }

// The window gone with the program, from the top of the loop: closed if
// the program left it open, its frame freed, the mouse let go
void fwin_end() {
    if (!win && !pix) return;
    unlock();
    if (win && wk_alive(win)) wk_close(win);
    win = 0; closed = 0; fresh = 0;
    if (pix) free(pix);
    if (pal) free((char *)pal);
    pix = 0; pal = 0;
}

// What the program says on the console meanwhile is kept, the last of it,
// for when it ends without having opened a window: then a message box
// says so, and shows it (a game that could not start, or a program that
// is not a game)
static char out[OUT_N], say[SAY_N];
static int outn;

static int cap_write(int ctx, char *buf, int n) {
    int k;
    k = 0;
    while (k < n) { out[outn % OUT_N] = buf[k]; ++outn; ++k; }
    return n;
}
static int cap_read(int ctx, char *buf, int n) { return 0; }

// the last line it said, after the words already in say
static void last_line() {
    int n, e, b, k;
    char text[OUT_N + 1];
    // the ring's last OUT_N bytes, in order
    n = outn < OUT_N ? outn : OUT_N;
    k = 0;
    while (k < n) { text[k] = out[(outn - n + k) % OUT_N]; ++k; }
    e = n;
    while (e > 0 && (text[e - 1] == '\n' || text[e - 1] == '\r' || text[e - 1] == ' ')) --e;
    b = e;
    while (b > 0 && text[b - 1] != '\n') --b;
    if (b == e) return;
    text[e] = 0;
    ui_ncat(say, " It said: ", SAY_N);
    ui_ncat(say, text + b, SAY_N);
}

// The command line, as a windowed DOS program (Program Manager's window
// items, and a DOS program Program Manager runs in one started from File
// Manager or File > Run): from the top of grin's loop, with dos_busy set
void fwin_run(char *cmdline) {
    ever = 0; outn = 0;
    gd_sink((int)cap_write, (int)cap_read, 0);
    gd_exec(cmdline);
    gd_sink(0, 0, 0);
    if (!ever) {
        ui_ncpy(say, dos_busy_name, SAY_N);
        ui_ncat(say, " ended without opening a window.", SAY_N);
        last_line();
        wk_msgbox(0, 0, "grin", say, WK_OK);
    }
    fwin_end();
}
