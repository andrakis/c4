// shell.c -- the C4IX desktop's shell, over the window kit (src/winkit).
// docs/c4ix-desktop.md.
//
// It owns the display: it takes it from the kernel, attaches the rings,
// and runs the loop that reads events, keeps the windows' programs going
// and draws. It draws what is the desktop's own -- its picture and icons
// under the windows, the taskbar and the Go menu over them -- and keeps
// Settings' choices in /etc/desktop.cfg. The windows are the kit's.

#include "desktop.h"
#define printf uprintf
#include "wkgui.h"

enum { START_W = 200, N_ICONS = 6 };
static int running;
static int start_open, start_hot;
static int icon_t, icon_k;
static int cpu_shown, cpu_n;
static char tbuf[128];

// ---- Settings' choices -------------------------------------------------------
//
// The colour scheme (the kit's), the desktop's own colour, its picture, the
// screen saver and its wait, and the look.
static int swatches[8] = { 0x008080, 0x000040, 0x000000, 0x3a4a5a, 0x205030, 0x502850, 0xa09070, 0x606060 };
enum { T_MODERN = 8 };
static int cur_scheme, cur_bg;
static int cur_wall;               // the picture (wall.c) under everything, or the colour
static int cur_saver, cur_wait;    // the screen saver (savers.c, 0 none) and after how many minutes

static void theme_init() {
    cur_scheme = T_MODERN; cur_bg = 0 - 1; cur_wall = 1; cur_saver = 1; cur_wait = 5; wk.style = S_FLAT;
}
static void theme_apply() { wk_scheme_use(cur_scheme, cur_bg); }
int theme_count() { return wk_scheme_count(); }
char *theme_name(int i) { return wk_scheme_name(i); }
int theme_scheme() { return cur_scheme; }
int theme_bg() { return cur_bg; }
int theme_wall() { return cur_wall; }
int theme_saver() { return cur_saver; }
int theme_wait() { return cur_wait; }
int theme_swatch(int i) { return i >= 0 && i < 8 ? swatches[i] : 0; }

// /etc/desktop.cfg: "scheme N bg V wall W saver S wait M style L", read
// when the desktop starts, so a choice outlives Power Off (and, with
// persist, a reload of the page). Read into a buffer longer than any
// file it takes, zeroed, so looking a few bytes ahead stays inside it.
static char cfg[128];
static int cfg_num(char *s, int *v) {
    int n, neg, k;
    n = 0; neg = 0; k = 0;
    if (s[0] == '-') { neg = 1; ++s; ++k; }
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); ++s; ++k; }
    *v = neg ? 0 - n : n;
    return k;
}
static void theme_load() {
    int fd, n, i, v;
    if ((fd = uopen("/etc/desktop.cfg", O_RD)) < 0) return;
    memset(cfg, 0, 128);
    n = uread(fd, cfg, 111);
    uclose(fd);
    if (n <= 0) return;
    cfg[n] = 0;
    i = 0;
    while (i < n) {
        if (cfg[i] == 's' && cfg[i + 1] == 'c' && cfg[i + 6] == ' ') {
            cfg_num(cfg + i + 7, &v);
            if (v >= 0 && v < wk_scheme_count()) cur_scheme = v;
        }
        if (cfg[i] == 'b' && cfg[i + 1] == 'g' && cfg[i + 2] == ' ') { cfg_num(cfg + i + 3, &v); cur_bg = v; }
        if (cfg[i] == 'w' && cfg[i + 1] == 'a' && cfg[i + 2] == 'l' && cfg[i + 4] == ' ') { cfg_num(cfg + i + 5, &v); cur_wall = v != 0; }
        if (cfg[i] == 's' && cfg[i + 1] == 'a' && cfg[i + 5] == ' ') { cfg_num(cfg + i + 6, &v); if (v >= 0 && v < saver_count()) cur_saver = v; }
        if (cfg[i] == 's' && cfg[i + 1] == 't' && cfg[i + 5] == ' ') { cfg_num(cfg + i + 6, &v); if (v >= 0 && v < N_STYLES) wk.style = v; }
        if (cfg[i] == 'w' && cfg[i + 1] == 'a' && cfg[i + 2] == 'i' && cfg[i + 4] == ' ') { cfg_num(cfg + i + 5, &v); if (v >= 1 && v <= 60) cur_wait = v; }
        ++i;
    }
    theme_apply();
}
static void theme_save() {
    int fd;
    umkdir("/etc");
    if ((fd = uopen("/etc/desktop.cfg", O_WR + O_CREATE + O_TRUNCATE)) < 0) return;
    ui_cpy(cfg, "scheme ");
    ui_num(cfg + ui_len(cfg), cur_scheme);
    ui_cat(cfg, " bg ");
    if (cur_bg < 0) ui_cat(cfg, "-1"); else ui_num(cfg + ui_len(cfg), cur_bg);
    ui_cat(cfg, cur_wall ? " wall 1 saver " : " wall 0 saver ");
    ui_num(cfg + ui_len(cfg), cur_saver);
    ui_cat(cfg, " wait ");
    ui_num(cfg + ui_len(cfg), cur_wait);
    ui_cat(cfg, " style ");
    ui_num(cfg + ui_len(cfg), wk.style);
    ui_cat(cfg, "\n");
    write(fd, cfg, ui_len(cfg));
    uclose(fd);
}
void theme_set(int scheme, int bg) {
    if (scheme >= 0 && scheme < wk_scheme_count()) cur_scheme = scheme;
    cur_bg = bg;
    cur_wall = 0;
    theme_apply();
    theme_save();
    wk_redraw_all();
}
// the picture on or off; the colour stays what it was
void theme_set_wall(int on) {
    cur_wall = on != 0;
    theme_save();
    wk_redraw_all();
}
void theme_set_style(int style) {
    wk.style = style >= 0 && style < N_STYLES ? style : S_FLAT;
    theme_save();
    wk_redraw_all();
}
void theme_set_saver(int which, int wait) {
    if (which >= 0 && which < saver_count()) cur_saver = which;
    if (wait >= 1 && wait <= 60) cur_wait = wait;
    theme_save();
    wk_redraw_all();
}

// Settings' Preview: the saver now, past the click that asked for it
static int saver_grace;
void saver_preview() {
    if (!cur_saver) return;
    saver_start(cur_saver);
    saver_grace = __time() + 150;
}

// ---- the desktop: its picture, and its icons ----------------------------------------

static char *icon_labels[N_ICONS] = { "Filesystem", "Terminal", "Text Editor", "IDE", "Calculator", "System Monitor" };
static int icon_kinds[N_ICONS] = { 0, K_TERM, K_NOTE, K_EDIT, K_CALC, K_TASKS };

static void icon(int x, int y, char *label, int kind) {
    int lw;
    if (kind == 0) {                               // Filesystem
        gui_rect(x + 10, y, 28, 20, C_FACE); d_bevel(x + 10, y, 28, 20, B_RAISED);
        gui_rect(x + 13, y + 3, 22, 14, 0x008080);
        gui_rect(x + 8, y + 22, 32, 8, C_FACE); d_bevel(x + 8, y + 22, 32, 8, B_RAISED);
    } else if (kind == K_TERM) {
        gui_rect(x + 8, y, 32, 26, C_FACE); d_bevel(x + 8, y, 32, 26, B_RAISED);
        gui_rect(x + 11, y + 3, 26, 18, C_DARK);
        gui_text2(x + 13, y + 5, 0xc0c0c0, 10, F_MONO, 0, ">_", 2);
        gui_rect(x + 12, y + 28, 24, 3, C_FACE);
    } else {
        gui_rect(x + 14, y + 4, 20, 24, C_WHITE); gui_recto(x + 14, y + 4, 20, 24, C_SHADOW);
        if (kind == K_NOTE) { gui_rect(x + 17, y + 10, 14, 1, 0x4060c0); gui_rect(x + 17, y + 14, 14, 1, 0x4060c0); gui_rect(x + 17, y + 18, 10, 1, 0x4060c0); }
        else d_icon(kind, x + 16, y + 8);
    }
    lw = ui_len(label) * 6;
    // over the picture, a shadow to read it by
    if (wall_shown()) d_text(x + 25 - lw / 2, y + 37, 0x000000, F_SANS, label);
    d_text(x + 24 - lw / 2, y + 36, C_LIGHT, F_SANS, label);
}

// under the windows: the picture (or the colour, when only the damage is
// drawn; a whole redraw has cleared to it), then the icons that reach in
static void desktop(int x, int y, int w, int h) {
    int k;
    if (!wall_draw(x, y, w, h) && wk_part) gui_rect(x, y, w, h, C_DESK);
    k = 0;
    while (k < N_ICONS) {
        // an icon's label is centred under it and can be wider than it
        if (wk_in_damage(0, 14 + k * 72, 80, 52)) icon(18, 14 + k * 72, icon_labels[k], icon_kinds[k]);
        ++k;
    }
}

// ---- the Go menu: a banner down the left, then the tools -----------------------------

enum { GO_RUN = 0 - 1, GO_OFF = 0 - 2, GO_WOLF = 0 - 3, GO_DOOM = 0 - 4, GO_TYRIAN = 0 - 5, GO_ROTT = 0 - 6 };
struct go_item { char *label; int kind; };
static struct go_item start_items[] = {
    { "Terminal", K_TERM }, { "Files", K_FILES }, { "Text Editor", K_NOTE }, { "Editor", K_EDIT },
    { "Calculator", K_CALC }, { "System Monitor", K_TASKS }, { "Settings", K_SETTINGS },
    { "About C4IX", K_ABOUT }, { "-", 0 },
    { "Wolfenstein 3D", GO_WOLF }, { "Doom", GO_DOOM }, { "Tyrian", GO_TYRIAN },
    { "Rise of the Triad", GO_ROTT }, { "Run...", GO_RUN }, { "-", 0 }, { "Power Off...", GO_OFF }
};
static int n_start;

// The games are optional (games/README.md in c4): each one's entry is in
// the menu only where its program is on the disk -- until programs list
// themselves in /etc/desktop.apps (docs/c4-explorer.md, W2)
static char *game_path(int kind) {
    if (kind == GO_WOLF) return "/usr/games/wolf3d/wolf3d.c4r";
    if (kind == GO_DOOM) return "/usr/games/doom/doom.c4r";
    if (kind == GO_TYRIAN) return "/usr/games/tyrian/tyrian.c4r";
    if (kind == GO_ROTT) return "/usr/games/rott/rott.c4r";
    return 0;
}
static void start_games() {
    int k, n, st[8];
    char *p;
    k = 0;
    n = 0;
    while (k < n_start) {
        p = game_path(start_items[k].kind);
        if (!p || ustat(p, st) == 0) {
            start_items[n].label = start_items[k].label;
            start_items[n].kind = start_items[k].kind;
            ++n;
        }
        ++k;
    }
    n_start = n;
}

static int start_height() {
    int k, h;
    h = 6; k = 0;
    while (k < n_start) { h = h + (start_items[k].label[0] == '-' ? 8 : 26); ++k; }
    return h;
}

static void draw_start() {
    int mx, my, mw, mh, k, iy;
    char c[2], *banner;
    mw = START_W; mh = start_height();
    mx = 2; my = wk.sh - TASK_H - mh;
    gui_rect(mx, my, mw, mh, C_FACE);
    d_bevel(mx, my, mw, mh, B_RAISED);
    gui_rect(mx + 3, my + 3, 22, mh - 6, ui_style == S_FLAT ? C_TITLE : C_SHADOW);
    c[1] = 0;
    banner = "C4IX";
    k = 0;
    while (k < 4) { c[0] = banner[k]; gui_text2(mx + 8, my + mh - 30 - (3 - k) * 18, C_WHITE, 16, F_BOLD, 0, c, 1); ++k; }
    iy = my + 3;
    k = 0;
    while (k < n_start) {
        if (start_items[k].label[0] == '-') {
            gui_rect(mx + 28, iy + 3, mw - 32, 1, C_SHADOW); gui_rect(mx + 28, iy + 4, mw - 32, 1, C_LIGHT);
            iy = iy + 8;
        } else {
            if (k == start_hot) gui_rect(mx + 26, iy, mw - 29, 26, C_SEL);
            if (start_items[k].kind > 0) d_icon(start_items[k].kind, mx + 32, iy + 5);
            d_text(mx + 56, iy + 7, k == start_hot ? C_WHITE : C_DARK, F_SANS, start_items[k].label);
            iy = iy + 26;
        }
        ++k;
    }
}

static int start_hit(int x, int y) {
    int mh, my, k, iy;
    mh = start_height();
    my = wk.sh - TASK_H - mh;
    if (x < 28 || x >= START_W + 2 || y < my || y >= my + mh) return 0 - 1;
    iy = my + 3;
    k = 0;
    while (k < n_start) {
        if (start_items[k].label[0] == '-') iy = iy + 8;
        else { if (y >= iy && y < iy + 26) return k; iy = iy + 26; }
        ++k;
    }
    return 0 - 1;
}

static void start_pick(int k) {
    int kind;
    start_open = 0; wk_redraw_all();
    if (k < 0) return;
    kind = start_items[k].kind;
    if (kind > 0) launch(kind);
    else if (kind == GO_RUN) inputbox(0, "Run", "Program to run:", "", A_RUN);
    else if (kind == GO_OFF) running = 0;
    else if (kind == GO_WOLF) run_command("wolf3d");
    else if (kind == GO_DOOM) run_command("doom");
    else if (kind == GO_TYRIAN) run_command("tyrian");
    else if (kind == GO_ROTT) run_command("rott");
}

// ---- the taskbar ----------------------------------------------------------------------

static int n_buttons() {
    struct win *w;
    int n;
    n = 0; w = win_first();
    while (w) { if (w->used && !w->dialog) ++n; w = w->next; }
    return n;
}
static int button_w() { int n, w; n = n_buttons(); if (!n) return 150; w = (wk.sw - 64 - 154) / n - 4; return w > 150 ? 150 : w; }

// The CPU meter beside the clock: System Monitor's number (tasks_cpu_usage),
// taken every two seconds; a click opens System Monitor on Performance.
enum { CPU_W = 66 };
static int cpu_x() { return wk.sw - 146; }

static void draw_taskbar() {
    int k, i, bx, bw, secs, h, m, f, ty;
    char *p;
    struct win *w;
    ty = wk.sh - TASK_H;
    gui_rect(0, ty, wk.sw, TASK_H, C_FACE);
    gui_rect(0, ty + (ui_style == S_FLAT ? 0 : 1), wk.sw, 1, ui_style == S_FLAT ? C_SHADOW : C_LIGHT);
    gui_rect(2, ty + 4, 56, 22, ui_style == S_FLAT && start_open ? C_LIGHT : C_FACE);
    if (ui_style != S_FLAT || start_open) d_bevel(2, ty + 4, 56, 22, start_open ? B_PRESSED : B_RAISED);
    // C4IX's mark: a cake (C4IX, C4KE -- cakes), its candle lit
    k = ty + 4;
    gui_rect(8, k + 13, 15, 6, 0x8b5a2b);
    gui_rect(8, k + 10, 15, 3, 0xf0a0c8); gui_rect(10, k + 13, 2, 2, 0xf0a0c8); gui_rect(17, k + 13, 2, 3, 0xf0a0c8);
    gui_rect(15, k + 4, 2, 6, 0xf4f4f4); gui_rect(15, k + 1, 2, 3, 0xffb020);
    d_text(28, ty + 9, C_DARK, F_BOLD, "Go");
    f = wk_focus();
    bw = button_w();
    k = 0; w = win_first();
    while (w) {
        i = w->id;
        if (w->used && !w->dialog) {
            bx = 64 + k * (bw + 4);
            if (ui_style == S_FLAT) {
                gui_rect(bx, ty + 4, bw, 22, i == f ? C_LIGHT : C_FACE);
                if (i == f) gui_rect(bx, ty + 24, bw, 2, C_TITLE);
            } else {
                gui_rect(bx, ty + 4, bw, 22, i == f ? 0xe0e0e0 : C_FACE);
                d_bevel(bx, ty + 4, bw, 22, i == f ? B_PRESSED : B_RAISED);
            }
            d_icon(w->kind, bx + 4, ty + 7);
            wk_clip_screen(bx + 22, ty + 4, bw - 26, 22);
            d_text(bx + 24, ty + 9, C_DARK, i == f ? F_BOLD : F_SANS, w->title);
            wk_noclip();
            ++k;
        }
        w = w->next;
    }
    gui_rect(cpu_x(), ty + 4, CPU_W, 22, C_FACE);
    if (ui_style != S_FLAT) d_bevel(cpu_x(), ty + 4, CPU_W, 22, B_SUNKEN);
    gui_rect(cpu_x() + 5, ty + 8, 8, 14, 0x000000);
    h = (cpu_shown * 14 + 50) / 100;
    if (h > 0) gui_rect(cpu_x() + 5, ty + 22 - h, 8, h, 0x00e000);
    ui_cpy(tbuf, "CPU ");
    ui_num(tbuf + 4, cpu_shown);
    ui_cat(tbuf, "%");
    d_text(cpu_x() + 17, ty + 9, C_DARK, F_SANS, tbuf);
    gui_rect(wk.sw - 76, ty + 4, 72, 22, C_FACE);
    if (ui_style != S_FLAT) d_bevel(wk.sw - 76, ty + 4, 72, 22, B_SUNKEN);
    secs = gui_clock();
    h = secs / 3600; m = (secs / 60) % 60;
    p = tbuf;
    if (h % 12 == 0) { *p = '1'; ++p; *p = '2'; ++p; }
    else { if (h % 12 >= 10) { *p = '1'; ++p; } *p = '0' + (h % 12) % 10; ++p; }
    *p = ':'; ++p; *p = '0' + m / 10; ++p; *p = '0' + m % 10; ++p;
    *p = ' '; ++p; *p = h < 12 ? 'A' : 'P'; ++p; *p = 'M'; ++p; *p = 0;
    d_text(wk.sw - 64, ty + 9, C_DARK, F_SANS, tbuf);
}

// over the windows: the taskbar, and the Go menu while it is open
static void front() {
    if (wk_in_damage(0, wk.sh - TASK_H, wk.sw, TASK_H)) draw_taskbar();
    if (start_open) draw_start();
}

static int popup() { return start_open; }

// ---- input the shell takes before the windows, and where there are none ----

static int mouse_front(int ev, int x, int y, int b) {
    int k, bw, ty;
    struct win *w;
    ty = wk.sh - TASK_H;
    if (ev == E_MOVE) {
        if (!start_open) return 0;
        k = start_hit(x, y);
        if (k != start_hot) { start_hot = k; wk_redraw_all(); }
        return 1;
    }
    if (ev != E_DOWN) return 0;
    if (start_open) {
        k = start_hit(x, y);
        start_open = 0; wk_redraw_all();
        if (k >= 0) start_pick(k);
        return 1;
    }
    if (y < ty) return 0;
    if (wk_modal()) return 1;
    if (ui_hit(x, y, 2, ty + 4, 56, 22)) { start_open = 1; start_hot = 0 - 1; wk_redraw_all(); return 1; }
    if (ui_hit(x, y, cpu_x(), ty + 4, CPU_W, 22)) { tasks_open_perf(); wk_redraw_all(); return 1; }
    bw = button_w();
    k = 0; w = win_first();
    while (w) {
        if (w->used && !w->dialog) {
            if (ui_hit(x, y, 64 + k * (bw + 4), ty + 4, bw, 22)) {
                if (w->id == wk_focus()) w->min = 1;
                else wk_raise(w->id);
                wk_redraw_all();
                return 1;
            }
            ++k;
        }
        w = w->next;
    }
    return 1;
}

// the desktop icons: a double click opens
static void mouse_desk(int ev, int x, int y, int b) {
    int k, i, t;
    if (ev != E_DOWN) return;
    t = now_ms();
    k = 0 - 1;
    i = 0;
    while (i < N_ICONS) { if (ui_hit(x, y, 14, 12 + i * 72, 56, 64)) k = i; ++i; }
    if (k >= 0 && k == icon_k && t - icon_t < 500) {
        if (icon_kinds[k] == 0) files_open("/"); else launch(icon_kinds[k]);
        icon_k = 0 - 1;
    } else { icon_k = k; icon_t = t; }
}

static int key(int down, int code, int ch, int mods) {
    icon_k = 0 - 1;
    // Ctrl+Shift+Esc: System Monitor, from anywhere
    if (code == KEY_ESC && (mods & M_CTRL) && (mods & M_SHIFT)) { tasks_open(); return 1; }
    if (start_open && code == KEY_ESC) { start_open = 0; wk_redraw_all(); return 1; }
    return 0;
}

static int pointer(int x, int y) { return start_open || y >= wk.sh - TASK_H ? PTR_ARROW : 0 - 1; }

// a dialog no window asked: Run
static void answer(int dialog, int yes) {
    if (dialog == A_RUN && yes && dialog_text()[0]) run_command(dialog_text());
}

// the screen changed size: the taskbar along its new bottom, and the picture stretched again
static void resized() {
    wk_set_area(0, 0, wk.sw, wk.sh - TASK_H);
    wall_resize();
}

static struct wk_shell shell = {
    .desktop = desktop, .front = front, .popup = popup, .mouse_front = mouse_front,
    .mouse_desk = mouse_desk, .key = key, .pointer = pointer, .answer = answer, .resized = resized
};

// ---- starting and stopping ------------------------------------------------------------

// What changed since boot, kept (persist save) before the machine stops:
// the page keeps it, and the next boot lays it back. Where there is no
// drive to keep it on, persist says so on the console and nothing waits.
static void save_files() {
    char *av[3];
    int pid;
    gui_clear(C_DARK);
    gui_text2(wk.sw / 2 - 120, wk.sh / 2 - 20, 0xffffff, 22, F_BOLD, 0, "Saving your files...", 0 - 1);
    gui_show();
    av[0] = "persist"; av[1] = "save"; av[2] = 0;
    if ((pid = spawn("/bin/persist", 2, av)) > 0) uwait(pid);
}

static void shutdown_screen() {
    gui_clear(C_DARK);
    gui_text2(wk.sw / 2 - 200, wk.sh / 2 - 20, 0xff8c1a, 22, F_BOLD, 0, "C4IX has stopped. You can close this page.", 0 - 1);
    gui_show();
}

enum { RING = 65536, EVENTS = GUI_M_MOVE | GUI_M_BUTTONS | GUI_M_KEYS | GUI_M_WHEEL | GUI_M_WINDOW };

int main(int argc, char **argv) {
    int *region, k, busy, secs, clock_min, last_tick, last_draw, input, start_ms, second_draw;
    int last_input, last_frame, aside, ev[4];

    if (!gui_present()) { printf("desktop: not fitted\n"); return 0; }
    // the display, from the kernel, giving way to a game that asks for it
    // (include/display.h, and "aside" below)
    if (!udisplay_take(DISP_YIELDS)) { printf("desktop: a program has the display\n"); return 1; }
    if (!(region = ualloc(RING))) { printf("desktop: no memory for the display's ring\n"); udisplay_give(); return 1; }
    if (!gui_attach(region, RING, 800, 600)) { printf("desktop: the display refused the ring\n"); udisplay_give(); return 1; }
    // GUI_M_WINDOW too: a resize or a focus change from the page means the
    // canvas may have been cleared, and the scene is drawn again
    gui_events(EVENTS);
    n_start = sizeof(start_items) / sizeof(start_items[0]);
    start_games();
    theme_init();
    theme_apply();                     // the default scheme, unless the file names one
    theme_load();
    // the kit's Notepad, as this desktop has always called it
    notepad_name = "Text Editor";
    notepad_about = "Text Editor for C4IX: edits text in the RAM filesystem.";
    notepad_home = "/";
    notepad_untitled = "/tmp/untitled.txt";
    notepad_filebox = 1;
    notepad_filter = "All Files (*)|*|Text Documents (*.txt)|*.txt|C Source (*.c;*.h)|*.c;*.h";
    wk_init(&shell);
    wall_load("/usr/share/splash/c4ix.c4p.gz");
    icon_k = 0 - 1;
    printf("desktop: running on the display. Go > Power Off to come back here.\n");

    term_open(0);
    running = 1; wk_redraw_all();
    clock_min = 0 - 1; last_tick = 0; last_draw = 0;
    start_ms = now_ms(); second_draw = 0;
    last_input = __time(); last_frame = 0;
    aside = 0;
    while (running) {
        busy = 0; input = 0;
        // A game asks for the whole display (wolf3d, include/display.h): give
        // way, between frames, and stand aside until it has gone -- reading
        // the terminals' pipes, so their programs run on, but drawing
        // nothing and reading no events, which go to the game. Then the
        // display again, at the desktop's size, and the whole scene drawn on it.
        if (!aside && udisplay(DISP_STATE, 0) == 1 && udisplay(DISP_YIELD, 0)) aside = 1;
        if (aside) {
            if (udisplay(DISP_STATE, 0) == 2) {
                wk_tick();
                uevwait(100, 0, 1);
                continue;
            }
            aside = 0;
            gui_attach(region, RING, wk.sw, wk.sh);
            gui_events(EVENTS);
            gui_cursor(saver_active() ? GUI_PTR_NONE : GUI_PTR_ARROW);
            wk_repoint();
            wk.mods = 0;
            last_input = __time();
            wk_redraw_all();
        }
        while (gui_poll(ev)) {
            busy = 1; input = 1;
            last_input = __time();
            // a screen saver running: the first key or movement ends it, and
            // goes no further (except while a Preview's click is let go)
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
        // the first frame, once more half a second in, in case the page was
        // not yet showing the display when it came
        if (!second_draw && now_ms() - start_ms >= 500) { second_draw = 1; wk_redraw_all(); }
        // the windows closed meanwhile freed; the terminals' pipes read; the timers due
        wk_tick();
        // the CPU meter, every other MACHINE second, and a redraw only when
        // it moved: machine time is what its numbers count (cpustat's ms)
        // and what the desktop sleeps on. Host time runs at another rate
        // whenever the machine is not paced to it.
        if (__time() - last_tick >= 1000) {
            last_tick = __time();
            cpu_n = cpu_n + 1;
            if (cpu_n >= 2) {
                cpu_n = 0;
                k = tasks_cpu_usage();
                if (k != cpu_shown) { cpu_shown = k; wk_dirty_rect(0, wk.sh - TASK_H, wk.sw, TASK_H); }
            }
        }
        secs = gui_clock() / 60;
        if (secs != clock_min) { clock_min = secs; wk_dirty_rect(0, wk.sh - TASK_H, wk.sw, TASK_H); }
        // The screen saver: after cur_wait minutes with no input, and then
        // a frame every SAVER_MS in place of the desktop's own drawing
        if (!saver_active() && cur_saver && __time() - last_input >= cur_wait * 60000) {
            saver_start(cur_saver);
            gui_cursor(GUI_PTR_NONE);
        }
        if (saver_active()) {
            if (__time() - last_frame >= SAVER_MS) {
                saver_frame();
                gui_show();
                last_frame = __time();
            }
            k = SAVER_MS - (__time() - last_frame);
            if (k < 1) k = 1;
            uevwait(k, gui_events_ring(), 0);
            continue;
        }
        // A redraw: the whole scene, or what is damaged. Input gets one at
        // once; a program pouring out text (raycast, top) gets at most one
        // every 25 ms -- it can print far faster than a person can read.
        if (wk_dirty() && (input || __time() - last_draw >= 25)) {
            wk_redraw(0);
            last_draw = __time();
            wk_drawn();                // the frames just shown have been read: their programs may draw again
            busy = 1;
        }
        // Nothing to do: sleep until there is. That is an event (a key, the
        // mouse), output on a terminal's pipe, or the next thing due -- a
        // redraw held back by the 25 ms above, a window's timer, the meter
        // -- and the kernel watches for the first two itself (SYS_EVWAIT).
        // While a redraw is held back the pipes are not watched: a program
        // pouring out text would wake it at once, with nothing to do until
        // the 25 ms are up.
        if (!busy) {
            if (wk_dirty()) k = 25 - (__time() - last_draw);
            else k = 1000 - (__time() - last_tick);
            secs = wk_next_timer();
            if (secs >= 0 && secs < k) k = secs;
            if (!second_draw && k > 100) k = 100;   // the half-second first-frame redraw
            if (k < 1) k = 1;
            uevwait(k, gui_events_ring(), !wk_dirty());
        }
    }

    wk_close_all();
    save_files();
    shutdown_screen();
    gui_detach();
    udisplay_give();
    printf("desktop: shut down\n");
    return 0;
}
