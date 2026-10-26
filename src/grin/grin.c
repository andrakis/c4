// grin.c -- grin, a graphical shell for C4DOS in the manner of Windows
// 3.1 (docs/grin-plan.md). It is a C4DOS program, grin.c4r, started by
// GRIN at the A> prompt (the page's own disk ends its AUTOEXEC.BAT with
// it). It takes the display, runs the window kit's loop in the Windows
// 3.1 look, and when the session ends (Program Manager's File > Exit
// grin) gives the display back and returns to A>.
//
// This file: the start and the end, the loop, what grin draws under the
// windows (the desktop's colour and pattern), what it takes before them
// (Ctrl+Esc, and a double click on the desktop: the Task List),
// GRIN.INI, and what the kit asks of its host -- memory, and files
// through libc and bare.c, which are C4DOS's. The applications grin runs
// (Clock, Calculator) are apps.c's; the loop gives them their turn. DOS
// programs run from the top of the loop, one at a time: full screen here
// (grin_full), or in a DOS Prompt window (dosprmpt.c).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "shell.h"
#include "wkgui.h"
#include "libjs/guest/snd.h"

static int running;
struct grin_ini ini;

// ---- GRIN.INI ------------------------------------------------------------------
//
//   [grin]
//   width=800
//   height=600
//   scheme=0
//   pattern=0
//   saver=0
//   wait=5
//
// Read when grin starts: from the RAM disk if this session wrote one,
// else from the disk, else the defaults. A line it does not know is
// passed over; a value out of range keeps the default.

enum { INI_MAX = 1024 };
static char inibuf[INI_MAX];

static void ini_defaults() {
    ini.sw = 640; ini.sh = 480; ini.scheme = 0; ini.pattern = 0; ini.saver = 0; ini.wait = 5;
}

static int ini_num(char *s) {
    int n;
    n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); ++s; }
    return n;
}

// "key=" at p: the number after it, or -1
static int ini_key(char *p, char *key) {
    int k;
    k = 0;
    while (key[k]) { if ((p[k] | 32) != key[k]) return 0 - 1; ++k; }
    if (p[k] != '=') return 0 - 1;
    return ini_num(p + k + 1);
}

static void ini_load() {
    int fd, n, i, v, w, h;
    char *p;
    ini_defaults();
    if ((fd = open("GRIN.INI", O_RDONLY)) < 0) return;
    memset(inibuf, 0, INI_MAX);
    n = read(fd, inibuf, INI_MAX - 16);
    close(fd);
    if (n <= 0) return;
    w = 0; h = 0;
    i = 0;
    while (i < n) {
        p = inibuf + i;
        if ((v = ini_key(p, "width")) >= 0) w = v;
        if ((v = ini_key(p, "height")) >= 0) h = v;
        if ((v = ini_key(p, "scheme")) >= 0 && v < wk_scheme_count()) ini.scheme = v;
        if ((v = ini_key(p, "pattern")) >= 0 && v < pattern_count()) ini.pattern = v;
        if ((v = ini_key(p, "saver")) >= 0 && v < saver_count()) ini.saver = v;
        if ((v = ini_key(p, "wait")) >= 1 && v <= 60) ini.wait = v;
        while (i < n && inibuf[i] != '\n') ++i;
        ++i;
    }
    if (w == 800 && h == 600) { ini.sw = 800; ini.sh = 600; }
}

static void ini_line(char *key, int v) {
    char num[24];
    ui_ncat(inibuf, key, INI_MAX);
    ui_ncat(inibuf, "=", INI_MAX);
    ui_ncat(inibuf, ui_num(num, v), INI_MAX);
    ui_ncat(inibuf, "\n", INI_MAX);
}

void ini_save() {
    int fd;
    inibuf[0] = 0;
    ui_ncat(inibuf, "[grin]\n", INI_MAX);
    ini_line("width", ini.sw);
    ini_line("height", ini.sh);
    ini_line("scheme", ini.scheme);
    ini_line("pattern", ini.pattern);
    ini_line("saver", ini.saver);
    ini_line("wait", ini.wait);
    if ((fd = open("GRIN.INI", O_WRONLY | O_CREAT | O_TRUNC)) < 0) return;
    write(fd, inibuf, ui_len(inibuf));
    close(fd);
}

// ---- the desktop: its colour, and a pattern over it ----------------------------------
//
// Windows 3.1's patterns are 8x8, black over the desktop's colour. grin
// draws one as a 64x64 image (the host caches it) tiled over the damage.

enum { N_PATTERNS = 5, TILE = 64, TILE_ID = 31 };
static char *pattern_names[N_PATTERNS] = { "(None)", "50% Gray", "Boxes", "Diamonds", "Weave" };
static int patterns[N_PATTERNS * 8] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55,
    0x00, 0x7e, 0x42, 0x42, 0x42, 0x42, 0x7e, 0x00,
    0x10, 0x38, 0x7c, 0xfe, 0x7c, 0x38, 0x10, 0x00,
    0x88, 0x54, 0x22, 0x45, 0x88, 0x15, 0x22, 0x51
};
static int *tile;
static int tile_made;                 // the pattern and colour the image was made for, + 1

int pattern_count() { return N_PATTERNS; }
char *pattern_name(int i) { return i >= 0 && i < N_PATTERNS ? pattern_names[i] : ""; }

static void tile_make() {
    int x, y, bits;
    if (!tile && !(tile = (int *)malloc(TILE * TILE * 4))) return;
    y = 0;
    while (y < TILE) {
        bits = patterns[ini.pattern * 8 + (y & 7)];
        x = 0;
        while (x < TILE) { tile[y * TILE + x] = (bits >> (7 - (x & 7))) & 1 ? C_DARK : C_DESK; ++x; }
        ++y;
    }
    gui_imgdef(TILE_ID, TILE, TILE, tile);
    tile_made = 1;
}

static void desktop(int x, int y, int w, int h) {
    int tx, ty;
    if (!ini.pattern) { if (wk_part) gui_rect(x, y, w, h, C_DESK); return; }
    if (!tile_made) tile_make();
    if (!tile) { gui_rect(x, y, w, h, C_DESK); return; }
    ty = y - y % TILE;
    while (ty < y + h) {
        tx = x - x % TILE;
        while (tx < x + w) { gui_image(TILE_ID, tx, ty); tx = tx + TILE; }
        ty = ty + TILE;
    }
}

void grin_set_scheme(int scheme) {
    if (scheme < 0 || scheme >= wk_scheme_count()) return;
    ini.scheme = scheme;
    wk_scheme_use(scheme, 0 - 1);
    tile_made = 0;
    ini_save();
    wk_redraw_all();
}

void grin_set_pattern(int pattern) {
    if (pattern < 0 || pattern >= N_PATTERNS) return;
    ini.pattern = pattern;
    tile_made = 0;
    ini_save();
    wk_redraw_all();
}

// 640x480 or 800x600, at once: the kit asks the display and lays the
// windows out again (maximised ones fill the new screen)
void grin_set_screen(int sw, int sh) {
    if (!((sw == 640 && sh == 480) || (sw == 800 && sh == 600))) return;
    ini.sw = sw; ini.sh = sh;
    ini_save();
    if (sw != wk.sw || sh != wk.sh) wk_screen(sw, sh);
    wk_redraw_all();
}

// ---- the screen saver ---------------------------------------------------------------

static int saver_grace;
void grin_saver_test(int which) {
    if (which <= 0 || which >= saver_count()) return;
    saver_start(which);
    gui_cursor(GUI_PTR_NONE);
    saver_grace = __time() + 150;
}

// ---- items: a picture and a label ---------------------------------------------------

// greyed: every other line of a 32x32 picture in the window's white
static void k_grey(int x, int y) {
    int j;
    j = 0;
    while (j < 32) { d_rect(x, y + j, 32, 1, C_WHITE); j = j + 2; }
}

// the pictures, 32x32, client-relative
void grin_picture(int pic, int x, int y) {
    int k;
    if (pic == PIC_FILES) {
        // a filing cabinet's drawer, open, with a folder in it
        d_rect(x + 3, y + 4, 26, 26, C_DARK);
        d_rect(x + 4, y + 5, 24, 24, 0xc0a060);
        d_rect(x + 6, y + 8, 20, 9, C_DARK); d_rect(x + 7, y + 9, 18, 7, 0xe0c080);
        d_rect(x + 13, y + 11, 6, 2, C_DARK);
        d_rect(x + 6, y + 19, 20, 9, C_DARK); d_rect(x + 7, y + 20, 18, 7, 0xe0c080);
        d_rect(x + 13, y + 22, 6, 2, C_DARK);
        d_rect(x + 9, y + 1, 14, 7, 0xf0e070); d_recto(x + 9, y + 1, 13, 6, C_DARK);
    } else if (pic == PIC_DOS) {
        // a screen with a prompt on it
        d_rect(x + 1, y + 3, 30, 24, C_DARK);
        d_rect(x + 3, y + 5, 26, 20, 0x000000);
        d_textn(x + 4, y + 8, 0xc0c0c0, 9, F_MONO, 5, "C>_", 3);
        d_rect(x + 10, y + 27, 12, 3, C_SHADOW);
    } else if (pic == PIC_NOTEPAD) {
        // a pad of ruled paper, its spiral along the top
        d_rect(x + 5, y + 3, 22, 28, C_DARK);
        d_rect(x + 6, y + 4, 20, 26, C_WHITE);
        k = 0;
        while (k < 6) { d_rect(x + 8, y + 10 + k * 3, 16, 1, 0x8080c0); ++k; }
        k = 0;
        while (k < 5) { d_rect(x + 8 + k * 4, y + 1, 2, 5, C_SHADOW); ++k; }
    } else if (pic == PIC_GAME) {
        // a joystick
        d_rect(x + 3, y + 20, 26, 9, C_DARK);
        d_rect(x + 4, y + 21, 24, 7, 0x606060);
        d_rect(x + 15, y + 8, 3, 13, C_DARK);
        d_disc(x + 16, y + 7, 5, 0xc02020);
        d_disc(x + 24, y + 18, 2, 0xe0e020);
    } else if (pic == PIC_APP) {
        // a window
        d_rect(x + 2, y + 4, 28, 24, C_DARK);
        d_rect(x + 3, y + 5, 26, 22, C_WHITE);
        d_rect(x + 3, y + 5, 26, 5, C_TITLE);
    } else if (pic == PIC_DISPLAY) {
        d_rect(x + 2, y + 2, 28, 21, C_DARK);
        d_rect(x + 4, y + 4, 24, 17, 0x0000a0);
        d_rect(x + 6, y + 6, 10, 6, C_WHITE);
        d_rect(x + 12, y + 23, 8, 3, C_SHADOW);
        d_rect(x + 7, y + 26, 18, 3, C_FACE); d_recto(x + 7, y + 26, 17, 2, C_DARK);
    } else if (pic == PIC_COLORS) {
        d_disc(x + 16, y + 16, 14, C_DARK);
        d_disc(x + 16, y + 16, 13, 0xf0e0b0);
        d_disc(x + 10, y + 11, 4, 0xe02020); d_disc(x + 20, y + 9, 4, 0x20a020);
        d_disc(x + 24, y + 18, 4, 0x2040e0); d_disc(x + 12, y + 22, 4, 0xf0d000);
    } else if (pic == PIC_DESKTOP) {
        d_rect(x + 1, y + 3, 30, 24, C_DARK);
        d_rect(x + 2, y + 4, 28, 22, 0x008080);
        d_rect(x + 5, y + 7, 14, 10, C_FACE); d_rect(x + 5, y + 7, 14, 3, C_TITLE);
        d_rect(x + 4, y + 21, 6, 4, C_FACE); d_rect(x + 12, y + 21, 6, 4, C_FACE);
    } else if (pic == PIC_CLOCK) {
        d_disc(x + 16, y + 16, 15, C_DARK);
        d_disc(x + 16, y + 16, 13, C_WHITE);
        d_rect(x + 15, y + 4, 2, 3, C_DARK); d_rect(x + 15, y + 26, 2, 3, C_DARK);
        d_rect(x + 4, y + 15, 3, 2, C_DARK); d_rect(x + 26, y + 15, 3, 2, C_DARK);
        d_line(x + 16, y + 16, x + 16, y + 7, C_DARK); d_line(x + 17, y + 16, x + 17, y + 7, C_DARK);
        d_line(x + 16, y + 16, x + 22, y + 20, C_DARK);
        d_line(x + 16, y + 16, x + 10, y + 24, 0xc02020);
    } else if (pic == PIC_CALC) {
        d_rect(x + 5, y + 1, 22, 30, C_DARK);
        d_rect(x + 6, y + 2, 20, 28, C_FACE);
        d_rect(x + 8, y + 4, 16, 6, 0x80a080); d_recto(x + 8, y + 4, 15, 5, C_SHADOW);
        k = 0;
        while (k < 12) { d_rect(x + 8 + (k % 3) * 6, y + 13 + (k / 3) * 4, 4, 3, k % 3 == 2 ? C_TITLE : C_DARK); ++k; }
    } else {
        // Control Panel: a window with sliders
        d_rect(x + 1, y + 2, 30, 28, C_FACE); d_recto(x + 1, y + 2, 29, 27, C_DARK);
        d_rect(x + 2, y + 3, 28, 5, C_TITLE);
        d_rect(x + 6, y + 13, 20, 2, C_DARK); d_rect(x + 10, y + 11, 4, 6, C_SHADOW);
        d_rect(x + 6, y + 22, 20, 2, C_DARK); d_rect(x + 18, y + 20, 4, 6, C_SHADOW);
    }
}

void grin_item_dim(int cx, int y, int pic, char *label, int sel, int dim) {
    int lw;
    grin_picture(pic, cx - 16, y);
    // not available: the picture greyed over, as 3.1 greyed a menu item
    if (dim) { k_grey(cx - 16, y); }
    lw = ui_len(label) * 6 + 4;
    if (sel) d_rect(cx - lw / 2, y + 35, lw, 14, C_SEL);
    d_text(cx - lw / 2 + 2, y + 36, sel ? C_WHITE : dim ? C_SHADOW : C_DARK, F_SANS, label);
}

void grin_item(int cx, int y, int pic, char *label, int sel) { grin_item_dim(cx, y, pic, label, sel, 0); }

int grin_item_hit(int px, int py, int cx, int y) { return ui_hit(px, py, cx - ITEM_W / 2, y, ITEM_W, 52); }

int grin_dialog(struct wk_class *c, int size, char *title, int w, int h) {
    int i;
    c->size = size;
    i = wk_open(c, title, w, h, WK_DIALOG | WK_FIXED | WK_CENTER, 0);
    return i;
}

// ---- what grin takes before the windows, and where there are none --------------------

static int desk_t, desk_x, desk_y;

// Ctrl+Esc: the Task List, from anywhere but a dialog
static int key(int down, int code, int ch, int mods) {
    if (code == KEY_ESC && (mods & M_CTRL) && !(mods & M_SHIFT)) {
        if (!wk_modal()) tasklist_open();
        return 1;
    }
    return 0;
}

// a double click on the desktop: the Task List
static void mouse_desk(int ev, int x, int y, int b) {
    int t;
    if (ev != E_DOWN) return;
    t = wk_now();
    if (desk_t && t - desk_t < 500 && x - desk_x < 5 && desk_x - x < 5 && y - desk_y < 5 && desk_y - y < 5) {
        desk_t = 0;
        tasklist_open();
        return;
    }
    desk_t = t; desk_x = x; desk_y = y;
}

static struct wk_shell shell = { .desktop = desktop, .mouse_desk = mouse_desk, .key = key };

// the session ends -- but not while a DOS program runs, which would be
// left with nothing to return to (and no DOS Prompt has one then)
void grin_quit() {
    if (dos_busy) { dos_refuse(); return; }
    running = 0;
}

// ---- what the kit asks of its host -------------------------------------------------
//
// Memory from libc. Files are disk.c's: through libc's bare.c, which is
// C4DOS's API, with the file box's folders the drives.

char *wk_alloc(int n) {
    char *p;
    if (n <= 0) n = 1;
    if ((p = (char *)malloc(n))) memset(p, 0, n);
    return p;
}
void wk_free(char *p) { if (p) free(p); }

// ---- the session ----------------------------------------------------------------------

enum { RING = 65536, EVENTS = GUI_M_MOVE | GUI_M_BUTTONS | GUI_M_KEYS | GUI_M_WHEEL | GUI_M_WINDOW,
       NAP = 50 };

static int *ring;
static int start_ms, second_draw, last_input, last_frame, last_draw;

// One pass of the loop: the display's events to the windows, the
// timers, the screen saver, a redraw, and a nap when there is nothing to
// do. At the top (top = 1) it also unloads the applications that have
// gone (apps_reap). grin_pump is the same pass for a DOS program that
// waits in grin -- a DOS Prompt's program reading a line -- and so
// unloads nothing and starts no DOS program: that program, DOS and grin's
// own exec are on the stack under it.
static void pass(int top) {
    int ev[4], busy, input, k, t;
    busy = 0; input = 0;
    while (gui_poll(ev)) {
        busy = 1; input = 1;
        last_input = __time();
        // the screen saver: the first key or movement ends it, and goes
        // no further (except while a Test's click is let go)
        if (saver_active()) {
            if (ev[0] == GUI_EV_RESIZE) { wk_event(ev); continue; }
            if (__time() - saver_grace < 0) continue;
            if (ev[0] == GUI_EV_FOCUS) continue;
            saver_stop();
            gui_cursor(GUI_PTR_ARROW);
            wk_repoint();
            wk_redraw_all();
            continue;
        }
        wk_event(ev);
    }
    // the first frame once more half a second in, in case the page was
    // not yet showing the display when it came
    if (!second_draw && __time() - start_ms >= 500) { second_draw = 1; wk_redraw_all(); }
    wk_tick();
    if (top) apps_reap();
    if (!saver_active() && ini.saver && __time() - last_input >= ini.wait * 60000) {
        saver_start(ini.saver);
        gui_cursor(GUI_PTR_NONE);
    }
    if (saver_active()) {
        if (__time() - last_frame >= SAVER_MS) { saver_frame(); gui_show(); last_frame = __time(); }
        k = SAVER_MS - (__time() - last_frame);
        if (k < 1) k = 1;
        __c4_usleep(k * 1000);
        return;
    }
    // a redraw: at once for input, else at most one every 25 ms
    if (wk_dirty() && (input || __time() - last_draw >= 25)) {
        wk_redraw(0);
        last_draw = __time();
        wk_drawn();
        busy = 1;
    }
    // nothing to do: a nap until the next thing due, which an event
    // from the display cuts short
    if (!busy && running) {
        k = wk_dirty() ? 25 - (__time() - last_draw) : NAP;
        t = wk_next_timer();
        if (t >= 0 && t < k) k = t;
        if (!second_draw && k > 100) k = 100;
        if (k < 1) k = 1;
        __c4_usleep(k * 1000);
    }
}

void grin_pump() { pass(0); }

void grin_draw_now() {
    if (saver_active() || !wk_dirty()) return;
    wk_redraw(0);
    last_draw = __time();
    wk_drawn();
}

// ---- DOS programs, full screen (docs/grin-plan.md 3.6) ------------------------------
//
// grin hides its pointer and gives the display up (gui_detach); DOS runs
// the program, which takes the display for itself; and when it returns,
// grin takes the display back -- its ring again (the program took
// GUI_RING and dropped it), its size, event mask and pointer -- quiets
// the sound card, and draws everything. DOS has released the program's
// heap by then. This is the C4IX desktop's udisplay_give and take with no
// kernel in it. Always from the top of the loop, never from inside a
// window's code, where the kit is on the stack.

enum { FULL_LINE = 256 };
static char full_cmd[FULL_LINE];
static int full_queued;
static char full_dir[FULL_LINE];      // "A:\GAMES\DOOM": where it runs, or ""
static char full_back[FULL_LINE];     // ... and where that drive was

// A program named by a path ("\GAMES\DOOM\DOOM", "A:/GAMES/DOOM/doom.c4r")
// runs with its own directory current, as Program Manager's and File
// Manager's did in Windows when an item gave no working directory: a
// game finds its data beside it. The directory goes into full_dir, with
// the program's drive; "" for a bare word.
static void full_where(char *cmdline) {
    int k, e;
    while (*cmdline == ' ') ++cmdline;
    e = 0 - 1;
    k = 0;
    while (cmdline[k] && cmdline[k] != ' ' && k < FULL_LINE - 1) {
        if (cmdline[k] == '\\' || cmdline[k] == '/') e = k;
        ++k;
    }
    full_dir[0] = 0;
    if (e < 0) return;
    k = 0;
    if (cmdline[1] != ':') { full_dir[0] = 'A' + gd_drive(); full_dir[1] = ':'; k = 2; }
    while (e > 0 && k < FULL_LINE - 2) { full_dir[k] = *cmdline; ++k; ++cmdline; --e; }
    if (full_dir[k - 1] == ':') { full_dir[k] = '\\'; ++k; }
    full_dir[k] = 0;
}

int grin_full_waiting() { return full_queued; }

int grin_full(char *cmdline) {
    if (dos_refuse()) return 0 - 1;
    if (!ui_ncpy(full_cmd, cmdline, FULL_LINE)) return 0 - 1;
    full_where(full_cmd);
    full_queued = 1;
    return 0;
}

static void full_run() {
    full_queued = 0;
    dos_busy = 1;
    dos_busy_set(full_cmd);
    if (saver_active()) saver_stop();
    gui_cursor(GUI_PTR_NONE);
    gui_clear(0x000000);
    gui_show();
    gui_detach();
    // in the program's directory, and back
    full_back[0] = 0;
    if (full_dir[0]) {
        full_back[0] = full_dir[0]; full_back[1] = ':'; full_back[2] = 0;
        ui_ncat(full_back, gd_getcwd(full_dir[0] - 'A'), FULL_LINE);
        if (gd_chdir(full_dir) < 0) full_back[0] = 0;
    }
    gd_exec(full_cmd);
    if (full_back[0]) gd_chdir(full_back);
    // the display back, as main first took it
    if (!gui_attach(ring, RING, ini.sw, ini.sh)) printf("grin: the display refused the ring after %s\n", dos_busy_name);
    gui_events(EVENTS);
    gui_cursor(GUI_PTR_ARROW);
    if (snd_present()) snd_reset();
    dos_busy = 0;
    dos_busy_name[0] = 0;
    wk_repoint();
    wk_redraw_all();
    last_input = __time();
}

int main(int argc, char **argv) {
    // c4bb and native c4m have no display, and say so through the info
    // word; there is nothing for grin to do there
    if (!gui_present()) { printf("grin: this machine has no display, so grin cannot run here.\n"); return 1; }
    ini_load();
    if (!(ring = (int *)malloc(RING))) { printf("grin: no memory for the display's ring\n"); return 1; }
    if (!gui_attach(ring, RING, ini.sw, ini.sh)) { printf("grin: the display refused the ring\n"); free((char *)ring); return 1; }
    // GUI_M_WINDOW too: a resize or a focus change from the page means
    // the canvas may have been cleared, and the scene is drawn again
    gui_events(EVENTS);
    gui_cursor(GUI_PTR_ARROW);
    wk.style = S_WIN31;
    wk_scheme_use(ini.scheme, 0 - 1);
    wk_min_icons(1);
    wk_init(&shell);
    apps_init();
    progman_open();

    running = 1;
    wk_redraw_all();
    start_ms = __time(); second_draw = 0;
    last_input = __time(); last_frame = 0; last_draw = 0;
    while (running) {
        pass(1);
        // the top of the loop: a DOS program waiting to run, one at a time
        if (running && !dos_busy) {
            if (full_queued) full_run();
            else dosprompt_top();
        }
    }

    // the display back as it was found: no ring, so the console is DOS's again
    wk_close_all();
    apps_end();
    if (saver_active()) saver_stop();
    gui_clear(C_DARK);
    gui_text2(wk.sw / 2 - 150, wk.sh / 2 - 10, 0xc0c0c0, 14, F_SANS, 0, "grin has ended. GRIN at the prompt starts it again.", 0 - 1);
    gui_show();
    gui_detach();
    free((char *)ring);
    if (tile) free((char *)tile);
    return 0;
}
