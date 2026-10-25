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
// through libc and bare.c, which are C4DOS's.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "shell.h"
#include "wkgui.h"

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

// the pictures, 32x32, client-relative
static void picture(int pic, int x, int y) {
    if (pic == PIC_DISPLAY) {
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
    } else {
        // Control Panel: a window with sliders
        d_rect(x + 1, y + 2, 30, 28, C_FACE); d_recto(x + 1, y + 2, 29, 27, C_DARK);
        d_rect(x + 2, y + 3, 28, 5, C_TITLE);
        d_rect(x + 6, y + 13, 20, 2, C_DARK); d_rect(x + 10, y + 11, 4, 6, C_SHADOW);
        d_rect(x + 6, y + 22, 20, 2, C_DARK); d_rect(x + 18, y + 20, 4, 6, C_SHADOW);
    }
}

void grin_item(int cx, int y, int pic, char *label, int sel) {
    int lw;
    picture(pic, cx - 16, y);
    lw = ui_len(label) * 6 + 4;
    if (sel) d_rect(cx - lw / 2, y + 35, lw, 14, C_SEL);
    d_text(cx - lw / 2 + 2, y + 36, sel ? C_WHITE : C_DARK, F_SANS, label);
}

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

void grin_quit() { running = 0; }

// ---- what the kit asks of its host -------------------------------------------------
//
// Memory from libc; files through libc's bare.c, which is C4DOS's API:
// written to the RAM disk, read from it or else from the disk. Listing a
// folder comes with File Manager (G4): until then there is nothing to list.

char *wk_alloc(int n) {
    char *p;
    if (n <= 0) n = 1;
    if ((p = (char *)malloc(n))) memset(p, 0, n);
    return p;
}
void wk_free(char *p) { if (p) free(p); }
int wk_fopen(char *path, int wr) { return open(path, wr ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY); }
int wk_fread(int fd, char *buf, int n) { return read(fd, buf, n); }
int wk_fwrite(int fd, char *buf, int n) { return write(fd, buf, n); }
void wk_fclose(int fd) { close(fd); }
int wk_readdir(char *dir, int i, char *name) { return 0 - 1; }

// ---- the session ----------------------------------------------------------------------

enum { RING = 65536, EVENTS = GUI_M_MOVE | GUI_M_BUTTONS | GUI_M_KEYS | GUI_M_WHEEL | GUI_M_WINDOW,
       NAP = 50 };

int main(int argc, char **argv) {
    int *ring, ev[4], busy, input, k, t, start_ms, second_draw, last_input, last_frame, last_draw;

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
    progman_open();

    running = 1;
    wk_redraw_all();
    start_ms = __time(); second_draw = 0;
    last_input = __time(); last_frame = 0; last_draw = 0;
    while (running) {
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
        if (!saver_active() && ini.saver && __time() - last_input >= ini.wait * 60000) {
            saver_start(ini.saver);
            gui_cursor(GUI_PTR_NONE);
        }
        if (saver_active()) {
            if (__time() - last_frame >= SAVER_MS) { saver_frame(); gui_show(); last_frame = __time(); }
            k = SAVER_MS - (__time() - last_frame);
            if (k < 1) k = 1;
            __c4_usleep(k * 1000);
            continue;
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

    // the display back as it was found: no ring, so the console is DOS's again
    wk_close_all();
    if (saver_active()) saver_stop();
    gui_clear(C_DARK);
    gui_text2(wk.sw / 2 - 150, wk.sh / 2 - 10, 0xc0c0c0, 14, F_SANS, 0, "grin has ended. GRIN at A> starts it again.", 0 - 1);
    gui_show();
    gui_detach();
    free((char *)ring);
    if (tile) free((char *)tile);
    return 0;
}
