// wm.c -- the window manager. docs/c4ix-desktop.md.
//
// The only module that touches the display: it owns the rings, reads
// mouse and keys, draws the desktop, the windows' frames, the taskbar,
// the Go menu, menu bars and dialogs, and asks each window's tool to
// draw its client area (clipped to it) and to handle what happens there.

#include "desktop.h"
#define printf uprintf
#include "libjs/guest/gui.h"

// The windows (desktop.h): a list in the order they opened, which the
// taskbar shows, and through the same nodes a z-order, bottom first. A
// window closed is off the z-order at once and freed at the top of the
// next pass of the main loop (sweep): whoever closed it, or is walking
// the list, may still be holding it.
static struct win *first;
static struct win *zbot, *ztop;
static int next_id;
static int dirty, running;
static int start_open, start_hot;
static int drag, drag_dx, drag_dy;       // window index + 1 while dragging a title bar
static int sizing, size_w0, size_h0, size_x0, size_y0;
static int down_win, down_t, down_x, down_y;   // for double clicks
static int icon_t, icon_k;
static int menu_win, menu_idx, menu_hot; // an open drop-down: window + 1, menu, hot item
static int *ev;
static int clip_on;
static char dtext[128];                  // the input dialog's text
static int dcaret;
static char tbuf[128];

// ---- the drawing layer -------------------------------------------------------

void d_rect(int x, int y, int w, int h, int rgb) { if (w > 0 && h > 0) gui_rect(x, y, w, h, rgb); }
void d_recto(int x, int y, int w, int h, int rgb) { gui_recto(x, y, w, h, rgb); }
void d_line(int x0, int y0, int x1, int y1, int rgb) { gui_line(x0, y0, x1, y1, rgb); }
void d_disc(int x, int y, int r, int rgb) { gui_disc(x, y, r, rgb); }
void d_text(int x, int y, int rgb, int font, char *s) { gui_text2(x, y, rgb, 11, font, 0, s, 0 - 1); }
void d_textn(int x, int y, int rgb, int size, int font, int advance, char *s, int n) {
    gui_text2(x, y, rgb, size, font, advance, s, n);
}

// ---- colour schemes ---------------------------------------------------------
//
// The colours a scheme sets are variables (desktop.h), so Settings can
// change them while everything is open; the rest are constants. A scheme
// is nine of them: desktop, face, light and shadow edges, the active and
// inactive title gradients, and the selection.
int C_DESK = 0x008080;
int C_FACE = 0xc0c0c0;
int C_LIGHT = 0xffffff;
int C_SHADOW = 0x808080;
int C_TITLE = 0x000080;
int C_TITLE2 = 0x1084d0;
int C_ITITLE = 0x808080;
int C_ITITLE2 = 0xb4b4b4;
int C_SEL = 0x000080;

enum { N_THEMES = 9, T_COLS = 9, T_MODERN = 8 };
static char *theme_names[N_THEMES];
static int themes[N_THEMES * T_COLS];
static int swatches[8];
static int cur_scheme, cur_bg;
static int cur_wall;               // the picture (wall.c) under everything, or the colour
static int cur_saver, cur_wait;    // the screen saver (savers.c, 0 none) and after how many minutes
int ui_style;                      // S_FLAT or S_CLASSIC (desktop.h)

static void theme_row(int i, char *name, int desk, int face, int light, int shadow,
                      int t1, int t2, int it1, int it2) {
    int *t;
    theme_names[i] = name;
    t = themes + i * T_COLS;
    t[0] = desk; t[1] = face; t[2] = light; t[3] = shadow;
    t[4] = t1; t[5] = t2; t[6] = it1; t[7] = it2; t[8] = t1;
}
static void theme_init() {
    theme_row(0, "Classic Teal", 0x008080, 0xc0c0c0, 0xffffff, 0x808080, 0x000080, 0x1084d0, 0x808080, 0xb4b4b4);
    theme_row(1, "Granite",            0x3a4e62, 0xb8c4cc, 0xeef3f6, 0x687884, 0x384c60, 0x7890a8, 0x808c98, 0xb0bcc8);
    theme_row(2, "Drizzle",        0x48586c, 0xb4bcc4, 0xeef2f6, 0x707880, 0x4c5c7c, 0x8898b8, 0x80888c, 0xb0b8c0);
    theme_row(3, "Sandstone",           0xa08060, 0xd5ccbb, 0xf5f0e8, 0xa29275, 0x008080, 0x40b0b0, 0xa29275, 0xc8bca8);
    theme_row(4, "Blush",             0x804050, 0xcfafb7, 0xf3e4e8, 0x9f6070, 0x9f6070, 0xd090a0, 0xa08890, 0xc8b0b8);
    theme_row(5, "Pine",           0x285040, 0xa2c8a9, 0xe2f2e6, 0x5e9a6a, 0x184830, 0x408048, 0x6a8a70, 0x9ab8a0);
    theme_row(6, "Lavender",            0x504878, 0xaea8d9, 0xeae6f8, 0x6c64a8, 0x504888, 0x8880c8, 0x8880a0, 0xb4b0d0);
    // the flat look's own: cool greys, slate title bars, no gradient
    theme_row(8, "Modern",           0x2e3440, 0xeceff4, 0xffffff, 0xa0a8b4, 0x3b4252, 0x3b4252, 0xd8dee9, 0xd8dee9);
    theme_row(7, "Thunder",            0x202830, 0xa8a8a8, 0xe8e8e8, 0x606060, 0x303848, 0x607088, 0x707070, 0x989898);
    swatches[0] = 0x008080; swatches[1] = 0x000040; swatches[2] = 0x000000; swatches[3] = 0x3a4a5a;
    swatches[4] = 0x205030; swatches[5] = 0x502850; swatches[6] = 0xa09070; swatches[7] = 0x606060;
    cur_scheme = T_MODERN; cur_bg = 0 - 1; cur_wall = 1; cur_saver = 1; cur_wait = 5; ui_style = S_FLAT;
}
static void theme_apply() {
    int *t;
    t = themes + cur_scheme * T_COLS;
    C_DESK = cur_bg >= 0 ? cur_bg : t[0];
    C_FACE = t[1]; C_LIGHT = t[2]; C_SHADOW = t[3];
    C_TITLE = t[4]; C_TITLE2 = t[5]; C_ITITLE = t[6]; C_ITITLE2 = t[7]; C_SEL = t[8];
}
int theme_count() { return N_THEMES; }
char *theme_name(int i) { return i >= 0 && i < N_THEMES ? theme_names[i] : ""; }
int theme_scheme() { return cur_scheme; }
int theme_bg() { return cur_bg; }
int theme_wall() { return cur_wall; }
int theme_saver() { return cur_saver; }
int theme_wait() { return cur_wait; }
int theme_swatch(int i) { return i >= 0 && i < 8 ? swatches[i] : 0; }

// /etc/desktop.cfg: "scheme N bg V wall W saver S wait M", read when the desktop starts, so a
// choice outlives Power Off (not a reload of the page: the disk is RAM)
static char cfg[112];
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
    n = uread(fd, cfg, 111);
    uclose(fd);
    if (n <= 0) return;
    cfg[n] = 0;
    i = 0;
    while (i < n) {
        if (cfg[i] == 's' && cfg[i + 1] == 'c' && cfg[i + 6] == ' ') {
            cfg_num(cfg + i + 7, &v);
            if (v >= 0 && v < N_THEMES) cur_scheme = v;
        }
        if (cfg[i] == 'b' && cfg[i + 1] == 'g' && cfg[i + 2] == ' ') { cfg_num(cfg + i + 3, &v); cur_bg = v; }
        if (cfg[i] == 'w' && cfg[i + 1] == 'a' && cfg[i + 2] == 'l' && cfg[i + 4] == ' ') { cfg_num(cfg + i + 5, &v); cur_wall = v != 0; }
        if (cfg[i] == 's' && cfg[i + 1] == 'a' && cfg[i + 5] == ' ') { cfg_num(cfg + i + 6, &v); if (v >= 0 && v < saver_count()) cur_saver = v; }
        if (cfg[i] == 's' && cfg[i + 1] == 't' && cfg[i + 5] == ' ') { cfg_num(cfg + i + 6, &v); ui_style = v == S_CLASSIC ? S_CLASSIC : S_FLAT; }
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
    ui_cat(cfg, ui_style == S_CLASSIC ? " style 1" : " style 0");
    ui_cat(cfg, "\n");
    write(fd, cfg, ui_len(cfg));
    uclose(fd);
}
void theme_set(int scheme, int bg) {
    if (scheme >= 0 && scheme < N_THEMES) cur_scheme = scheme;
    cur_bg = bg;
    cur_wall = 0;
    theme_apply();
    theme_save();
    dirty = 1;
}
// the picture on or off; the colour stays what it was
void theme_set_wall(int on) {
    cur_wall = on != 0;
    theme_save();
    dirty = 1;
}
// the look: S_FLAT or S_CLASSIC, remembered
void theme_set_style(int style) {
    ui_style = style == S_CLASSIC ? S_CLASSIC : S_FLAT;
    theme_save();
    dirty = 1;
}
// the screen saver (savers.c) and its wait, in minutes
void theme_set_saver(int which, int wait) {
    if (which >= 0 && which < saver_count()) cur_saver = which;
    if (wait >= 1 && wait <= 60) cur_wait = wait;
    theme_save();
    dirty = 1;
}

// Settings' Preview: the saver now, past the click that asked for it
static int saver_grace;
void saver_preview() {
    if (!cur_saver) return;
    saver_start(cur_saver);
    saver_grace = __time() + 150;
}

// ---- damage ----
//
// A redraw is the whole scene when the scene changed -- input, a window
// opened, moved or raised -- but most of what changes on its own is one
// window's contents (a terminal's output, System Monitor's numbers) or the
// taskbar's clock and meter. Those mark only their rectangle, and
// redraw_part draws only what lies in it, front to back as the full
// redraw does, clipped to it and with no clear: the display keeps the
// rest of the last frame (docs/c4ix-cpu.md, step 11).
static int dmg, dmg_x0, dmg_y0, dmg_x1, dmg_y1;
static int part;                  // drawing only the damage (redraw_part)

void mark_rect_dirty(int x, int y, int w, int h) {
    if (dirty || w <= 0 || h <= 0) return;
    if (x < 0) { w = w + x; x = 0; }
    if (y < 0) { h = h + y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    if (w <= 0 || h <= 0) return;
    if (!dmg) { dmg = 1; dmg_x0 = x; dmg_y0 = y; dmg_x1 = x + w; dmg_y1 = y + h; return; }
    if (x < dmg_x0) dmg_x0 = x;
    if (y < dmg_y0) dmg_y0 = y;
    if (x + w > dmg_x1) dmg_x1 = x + w;
    if (y + h > dmg_y1) dmg_y1 = y + h;
}

// A window's own contents changed; nothing else did.
void mark_win_dirty(struct win *w) {
    if (!w->min) mark_rect_dirty(w->x, w->y, w->w, w->h);
}

static int in_damage(int x, int y, int w, int h) {
    return x < dmg_x1 && x + w > dmg_x0 && y < dmg_y1 && y + h > dmg_y0;
}

// Clipping, always inside the damage while redraw_part draws: the clip
// a window sets for its title or its client area must not reach past
// what is being redrawn, or it would draw over what is not.
static void wclip(int x, int y, int w, int h) {
    int x1, y1;
    if (part) {
        x1 = x + w; y1 = y + h;
        if (x < dmg_x0) x = dmg_x0;
        if (y < dmg_y0) y = dmg_y0;
        if (x1 > dmg_x1) x1 = dmg_x1;
        if (y1 > dmg_y1) y1 = dmg_y1;
        w = x1 > x ? x1 - x : 0;
        h = y1 > y ? y1 - y : 0;
    }
    gui_clip(x, y, w, h);
}
static void wnoclip() {
    if (part) gui_clip(dmg_x0, dmg_y0, dmg_x1 - dmg_x0, dmg_y1 - dmg_y0);
    else gui_noclip();
}

// Drawing narrowed to part of the window being drawn -- an app's pane --
// and back to all of it. Never wider than the window's client area.
static int clip_x, clip_y, clip_w, clip_h;
void d_clip(int x, int y, int w, int h) {
    int x1, y1;
    x1 = x + w; y1 = y + h;
    if (x < clip_x) x = clip_x;
    if (y < clip_y) y = clip_y;
    if (x1 > clip_x + clip_w) x1 = clip_x + clip_w;
    if (y1 > clip_y + clip_h) y1 = clip_y + clip_h;
    wclip(x, y, x1 > x ? x1 - x : 0, y1 > y ? y1 - y : 0);
}
void d_clip_end() { wclip(clip_x, clip_y, clip_w, clip_h); }

// The bevels: two pixels, light top-left, dark bottom-right (or
// the other way round for a sunken one).
void d_bevel(int x, int y, int w, int h, int style) {
    int a, b, c, d;
    // Flat: an outline, the accent's when pressed
    if (ui_style == S_FLAT) {
        if (w > 1 && h > 1) gui_recto(x, y, w - 1, h - 1, style == B_PRESSED ? C_TITLE : C_SHADOW);
        return;
    }
    if (style == B_ETCHED) {
        gui_recto(x, y, w - 1, h - 1, C_SHADOW);
        gui_recto(x + 1, y + 1, w - 1, h - 1, C_LIGHT);
        return;
    }
    if (style == B_RAISED) { a = C_LIGHT; b = C_DARK; c = C_FACE; d = C_SHADOW; }
    else if (style == B_PRESSED) { a = C_DARK; b = C_LIGHT; c = C_SHADOW; d = C_FACE; }
    else { a = C_SHADOW; b = C_LIGHT; c = C_DARK; d = C_FACE; }
    gui_rect(x, y, w, 1, a);             gui_rect(x, y, 1, h, a);
    gui_rect(x, y + h - 1, w, 1, b);     gui_rect(x + w - 1, y, 1, h, b);
    gui_rect(x + 1, y + 1, w - 2, 1, c); gui_rect(x + 1, y + 1, 1, h - 2, c);
    gui_rect(x + 1, y + h - 2, w - 2, 1, d); gui_rect(x + w - 2, y + 1, 1, h - 2, d);
}

// 16x16 pictures for each kind of window: title bars, the taskbar, icons.
void d_icon(int kind, int x, int y) {
    if (kind == K_TERM) {
        gui_rect(x, y + 1, 16, 13, C_DARK); gui_recto(x, y + 1, 16, 13, C_SHADOW);
        gui_text2(x + 2, y + 3, 0xc0c0c0, 8, F_MONO, 0, ">_", 2);
    } else if (kind == K_FILES) {
        gui_rect(x, y + 3, 7, 3, 0xc8a000); gui_rect(x, y + 5, 16, 10, 0xffd84a);
        gui_recto(x, y + 5, 16, 10, 0x806000);
    } else if (kind == K_TASKS) {
        gui_rect(x, y + 1, 16, 12, C_DARK); gui_recto(x, y + 1, 16, 12, C_SHADOW);
        gui_line(x + 2, y + 10, x + 6, y + 5, C_GREEN); gui_line(x + 6, y + 5, x + 9, y + 8, C_GREEN);
        gui_line(x + 9, y + 8, x + 14, y + 3, C_GREEN); gui_rect(x + 5, y + 13, 6, 2, C_SHADOW);
    } else if (kind == K_EDIT) {
        gui_rect(x + 1, y, 14, 16, C_WHITE); gui_recto(x + 1, y, 14, 16, C_SHADOW);
        gui_text2(x + 2, y + 3, 0x008000, 8, F_MONO, 0, "{}", 2);
        gui_rect(x + 3, y + 12, 10, 1, 0x4060c0);
    } else if (kind == K_NOTE) {
        gui_rect(x + 2, y, 12, 16, C_WHITE); gui_recto(x + 2, y, 12, 16, C_SHADOW);
        gui_rect(x + 4, y + 4, 8, 1, 0x4060c0); gui_rect(x + 4, y + 7, 8, 1, 0x4060c0);
        gui_rect(x + 4, y + 10, 6, 1, 0x4060c0);
    } else if (kind == K_SETTINGS) {
        // a gear: a disc with teeth, and a hole
        gui_rect(x + 7, y, 3, 16, 0x707070); gui_rect(x, y + 7, 16, 3, 0x707070);
        gui_line(x + 2, y + 2, x + 13, y + 13, 0x707070); gui_line(x + 13, y + 2, x + 2, y + 13, 0x707070);
        gui_line(x + 3, y + 2, x + 14, y + 13, 0x707070); gui_line(x + 12, y + 2, x + 1, y + 13, 0x707070);
        d_disc(x + 8, y + 8, 6, 0x909090);
        d_disc(x + 8, y + 8, 2, C_FACE);
    } else if (kind == K_CALC) {
        gui_rect(x + 2, y, 12, 16, 0xa0a0a0); gui_recto(x + 2, y, 12, 16, C_DARK);
        gui_rect(x + 4, y + 2, 8, 4, 0xd0ffd0);
        gui_rect(x + 4, y + 8, 3, 2, C_DARK); gui_rect(x + 9, y + 8, 3, 2, C_DARK);
        gui_rect(x + 4, y + 12, 3, 2, C_DARK); gui_rect(x + 9, y + 12, 3, 2, 0xc02020);
    } else {
        gui_rect(x + 1, y + 1, 14, 14, C_WHITE); gui_recto(x + 1, y + 1, 14, 14, C_TITLE);
        gui_text2(x + 6, y + 2, C_TITLE, 12, F_BOLD, 0, "i", 1);
    }
}

// ---- windows ---------------------------------------------------------------

struct win *win_get(int i) {
    struct win *w;
    w = first;
    while (w) { if (w->id == i && w->used) return w; w = w->next; }
    return 0;
}
int win_index(struct win *w) { return w ? w->id : 0 - 1; }
struct win *win_first() { return first; }
void mark_dirty() { dirty = 1; }
int now_ms() { return gui_ticks(); }
char *dialog_text() { return dtext; }

static int client_x(struct win *w) { return w->x + BORDER; }
static int menu_h(struct win *w) { return w->nmenus ? MENU_H : 0; }
static int client_y(struct win *w) { return w->y + BORDER + TITLE_H + 1 + menu_h(w); }
static int client_w(struct win *w) { return w->w - 2 * BORDER; }
static int client_h(struct win *w) { return w->h - (client_y(w) - w->y) - BORDER; }

static void z_unlink(struct win *w) {
    if (w->below) w->below->above = w->above; else if (zbot == w) zbot = w->above;
    if (w->above) w->above->below = w->below; else if (ztop == w) ztop = w->below;
    w->above = 0; w->below = 0;
}

void win_raise(int i) {
    struct win *w;
    if (!(w = win_get(i))) return;
    if (ztop != w) {
        z_unlink(w);
        w->below = ztop;
        if (ztop) ztop->above = w; else zbot = w;
        ztop = w;
    }
    w->min = 0;
    dirty = 1;
}

// The topmost window that is showing, or 0 -- and its number, or -1.
static struct win *focus() {
    struct win *w;
    w = ztop;
    while (w && w->min) w = w->below;
    return w;
}
int win_focused() { return win_index(focus()); }

// A dialog on top keeps the input to itself.
static int modal() {
    struct win *f;
    f = focus();
    if (f && (f->kind == K_MSG || f->kind == K_INPUT)) return f->id;
    return 0 - 1;
}

// The windows closed since the last pass, freed.
static void sweep() {
    struct win *w, *p, *n;
    p = 0; w = first;
    while (w) {
        n = w->next;
        if (!w->used) {
            if (p) p->next = n; else first = n;
            free(w->st); free(w);
        } else p = w;
        w = n;
    }
}

// Opens a window, or answers -1 when there is no memory for one.
int win_open(int kind, char *title, int w, int h) {
    int slot;
    struct win *p, *q;
    if (!(p = (struct win *)ualloc(sizeof(struct win)))) return 0 - 1;
    memset(p, 0, sizeof(struct win));
    if (!(p->st = (char *)ualloc(ST_BYTES))) { free(p); return 0 - 1; }
    memset(p->st, 0, ST_BYTES);
    // it cascades from the first place no open window holds
    slot = 0; q = first;
    while (q) { if (q->used && q->slot == slot) { ++slot; q = first; } else q = q->next; }
    p->slot = slot;
    p->id = next_id; ++next_id;
    if (!first) first = p;
    else { q = first; while (q->next) q = q->next; q->next = p; }
    p->used = 1; p->kind = kind; p->min = 0; p->max = 0; p->resizable = 1;
    p->nmenus = 0; p->owner = 0 - 1; p->action = 0;
    p->x = 100 + (slot % 10) * 24 + (slot / 10) * 12; p->y = 20 + (slot % 10) * 22;
    if (p->x + w > SW) p->x = SW - w; if (p->x < 0) p->x = 0;
    if (p->y + h > SH - TASK_H) p->y = SH - TASK_H - h; if (p->y < 0) p->y = 0;
    p->w = w; p->h = h;
    ui_cpy(p->title, title);
    win_raise(p->id);
    return p->id;
}

void win_title(struct win *w, char *s) { ui_cpy(w->title, s); dirty = 1; }

void win_menus(struct win *w, int n, char **titles, char ***items) {
    w->nmenus = n; w->menu_titles = titles; w->menu_items = items;
}

void win_close(int i) {
    struct win *w;
    if (!(w = win_get(i))) return;
    if (w->kind == K_TERM) term_close(w);
    if (w->kind == K_EDIT) edit_close(w);
    w->used = 0;
    z_unlink(w);
    if (drag == i + 1) drag = 0;
    if (sizing == i + 1) sizing = 0;
    if (menu_win == i + 1) menu_win = 0;
    dirty = 1;
}

// ---- dialogs ---------------------------------------------------------------------

void msgbox(struct win *owner, char *title, char *text, int yesno, int action) {
    int i, tw;
    struct win *w;
    tw = ui_len(text) * 6 + 40;
    if (tw < 260) tw = 260;
    if (tw > 560) tw = 560;
    i = win_open(K_MSG, title, tw, 120);
    if (i < 0) return;
    w = win_get(i);
    w->resizable = 0;
    w->x = (SW - tw) / 2; w->y = (SH - 120) / 2 - 40;
    w->owner = owner ? win_index(owner) : 0 - 1;
    w->action = action;
    ui_cpy(w->st, text);
    w->st[200] = yesno;
}

void inputbox(struct win *owner, char *title, char *prompt, char *initial, int action) {
    int i;
    struct win *w;
    i = win_open(K_INPUT, title, 380, 130);
    if (i < 0) return;
    w = win_get(i);
    w->resizable = 0;
    w->x = (SW - 380) / 2; w->y = (SH - 130) / 2 - 40;
    w->owner = owner ? win_index(owner) : 0 - 1;
    w->action = action;
    ui_cpy(w->st, prompt);
    ui_cpy(dtext, initial);
    dcaret = ui_len(dtext);
}

static void answer(struct win *d, int yes) {
    int owner, action;
    struct win *o;
    owner = d->owner; action = d->action;
    win_close(win_index(d));
    if (action == A_RUN) { if (yes && dtext[0]) run_command(dtext); return; }
    if (!(o = win_get(owner))) return;
    win_raise(owner);
    if (o->kind == K_FILES) files_answer(o, action, yes);
    else if (o->kind == K_NOTE) note_answer(o, action, yes);
    else if (o->kind == K_EDIT) edit_answer(o, action, yes);
    else if (o->kind == K_TASKS) tasks_answer(o, action, yes);
}

static void dialog_draw(struct win *w, int ox, int oy, int cw, int ch) {
    int bx;
    d_rect(ox, oy, cw, ch, C_FACE);
    if (w->kind == K_MSG) {
        gui_disc(ox + 30, oy + 30, 14, w->st[200] ? C_TITLE : 0xd02020);
        gui_text2(ox + 26, oy + 20, C_WHITE, 18, F_BOLD, 0, w->st[200] ? "?" : "i", 1);
        d_text(ox + 56, oy + 24, C_DARK, F_SANS, w->st);
        if (w->st[200]) {
            bx = cw / 2 - 84;
            ui_button(ox + bx, oy + ch - 34, 76, 24, "Yes", 0, 1);
            ui_button(ox + bx + 92, oy + ch - 34, 76, 24, "No", 0, 0);
        } else ui_button(ox + cw / 2 - 38, oy + ch - 34, 76, 24, "OK", 0, 1);
    } else {
        d_text(ox + 12, oy + 12, C_DARK, F_SANS, w->st);
        ui_field(ox + 12, oy + 30, cw - 24, dtext, dcaret, 1);
        ui_button(ox + cw - 176, oy + ch - 34, 76, 24, "OK", 0, 1);
        ui_button(ox + cw - 88, oy + ch - 34, 76, 24, "Cancel", 0, 0);
    }
}

static void dialog_mouse(struct win *w, int x, int y, int cw, int ch) {
    int bx;
    if (w->kind == K_MSG) {
        if (w->st[200]) {
            bx = cw / 2 - 84;
            if (ui_hit(x, y, bx, ch - 34, 76, 24)) answer(w, 1);
            else if (ui_hit(x, y, bx + 92, ch - 34, 76, 24)) answer(w, 0);
        } else if (ui_hit(x, y, cw / 2 - 38, ch - 34, 76, 24)) answer(w, 1);
    } else {
        if (ui_hit(x, y, cw - 176, ch - 34, 76, 24)) answer(w, 1);
        else if (ui_hit(x, y, cw - 88, ch - 34, 76, 24)) answer(w, 0);
        else if (ui_hit(x, y, 12, 30, cw - 24, 22)) { dcaret = (x - 16) / 7; if (dcaret > ui_len(dtext)) dcaret = ui_len(dtext); if (dcaret < 0) dcaret = 0; }
    }
    dirty = 1;
}

static void dialog_key(struct win *w, int code, int ch, int mods) {
    if (code == KEY_ESC) { answer(w, 0); return; }
    if (code == KEY_ENTER) { answer(w, 1); return; }
    if (w->kind == K_MSG) {
        if (w->st[200] && (ch == 'y' || ch == 'Y')) answer(w, 1);
        else if (w->st[200] && (ch == 'n' || ch == 'N')) answer(w, 0);
        return;
    }
    ui_field_key(dtext, 120, &dcaret, code, ch, mods);
    dirty = 1;
}

void run_command(char *cmd) { term_open(cmd); }

// ---- per-kind dispatch -------------------------------------------------------------

static void app_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    int k;
    k = w->kind;
    if (k == K_TERM) term_draw(w, ox, oy, cw, ch, focused);
    else if (k == K_FILES) files_draw(w, ox, oy, cw, ch, focused);
    else if (k == K_TASKS) tasks_draw(w, ox, oy, cw, ch, focused);
    else if (k == K_NOTE) note_draw(w, ox, oy, cw, ch, focused);
    else if (k == K_EDIT) edit_draw(w, ox, oy, cw, ch, focused);
    else if (k == K_CALC) calc_draw(w, ox, oy, cw, ch, focused);
    else if (k == K_SETTINGS) settings_draw(w, ox, oy, cw, ch, focused);
    else if (k == K_MSG || k == K_INPUT) dialog_draw(w, ox, oy, cw, ch);
    else about_draw(w, ox, oy, cw, ch, focused);
}

static void app_mouse(struct win *w, int e, int x, int y) {
    int k, cw, ch;
    k = w->kind; cw = client_w(w); ch = client_h(w);
    if (k == K_TERM) term_mouse(w, e, x, y, cw, ch);
    else if (k == K_FILES) files_mouse(w, e, x, y, cw, ch);
    else if (k == K_TASKS) tasks_mouse(w, e, x, y, cw, ch);
    else if (k == K_NOTE) note_mouse(w, e, x, y, cw, ch);
    else if (k == K_EDIT) edit_mouse(w, e, x, y, cw, ch);
    else if (k == K_CALC) calc_mouse(w, e, x, y, cw, ch);
    else if (k == K_SETTINGS) settings_mouse(w, e, x, y, cw, ch);
    else if ((k == K_MSG || k == K_INPUT) && e == E_DOWN) dialog_mouse(w, x, y, cw, ch);
}

static void app_key(struct win *w, int code, int ch, int mods) {
    int k;
    k = w->kind;
    if (k == K_TERM) term_key(w, code, ch, mods);
    else if (k == K_FILES) files_key(w, code, ch, mods);
    else if (k == K_TASKS) tasks_key(w, code, ch, mods);
    else if (k == K_NOTE) note_key(w, code, ch, mods);
    else if (k == K_EDIT) edit_key(w, code, ch, mods);
    else if (k == K_CALC) calc_key(w, code, ch, mods);
    else if (k == K_SETTINGS) settings_key(w, code, ch, mods);
    else if (k == K_MSG || k == K_INPUT) dialog_key(w, code, ch, mods);
    else if (code == KEY_ENTER || code == KEY_ESC) win_close(win_index(w));
}

static void app_command(struct win *w, int menu, int item) {
    if (w->kind == K_FILES) files_command(w, menu, item);
    else if (w->kind == K_NOTE) note_command(w, menu, item);
    else if (w->kind == K_EDIT) edit_command(w, menu, item);
}

// ---- drawing the scene -----------------------------------------------------------

int ui_mix(int a, int b, int t, int n) {
    int r, g, bl;
    r = ((a >> 16) & 255) + ((((b >> 16) & 255) - ((a >> 16) & 255)) * t) / n;
    g = ((a >> 8) & 255) + ((((b >> 8) & 255) - ((a >> 8) & 255)) * t) / n;
    bl = (a & 255) + (((b & 255) - (a & 255)) * t) / n;
    return (r << 16) | (g << 8) | bl;
}

static void gradient(int x, int y, int w, int h, int a, int b) {
    int i, n, bw;
    if (ui_style == S_FLAT) { gui_rect(x, y, w, h, a); return; }
    n = 16; bw = (w + n - 1) / n;
    i = 0;
    while (i < n) {
        if (i * bw < w) gui_rect(x + i * bw, y, (i + 1) * bw > w ? w - i * bw : bw, h, ui_mix(a, b, i, n - 1));
        ++i;
    }
}

static int cap_ink, cap_bg;           // flat: the glyph's colour and the bar's under it
static void cap_button(int x, int y, int what) {
    int k;
    k = C_DARK;
    if (ui_style == S_FLAT) k = cap_ink;
    else { gui_rect(x, y, 16, 14, C_FACE); d_bevel(x, y, 16, 14, B_RAISED); }
    if (what == 0) {                               // close: an X
        gui_line(x + 4, y + 3, x + 10, y + 9, k); gui_line(x + 5, y + 3, x + 11, y + 9, k);
        gui_line(x + 10, y + 3, x + 4, y + 9, k); gui_line(x + 11, y + 3, x + 5, y + 9, k);
    } else if (what == 1) {                        // maximise
        gui_recto(x + 3, y + 2, 9, 9, k); gui_rect(x + 3, y + 3, 9, 1, k);
    } else if (what == 2) {                        // restore
        gui_recto(x + 5, y + 2, 7, 6, k); gui_rect(x + 5, y + 3, 7, 1, k);
        gui_rect(x + 3, y + 5, 7, 6, ui_style == S_FLAT ? cap_bg : C_FACE);
        gui_recto(x + 3, y + 5, 7, 6, k); gui_rect(x + 3, y + 6, 7, 1, k);
    } else gui_rect(x + 4, y + 9, 6, 2, k);        // minimise
}

static int is_dialog(struct win *w) { return w->kind == K_MSG || w->kind == K_INPUT; }

static void draw_menubar(struct win *w, int focused) {
    int k, x, y, lw;
    y = w->y + BORDER + TITLE_H + 1;
    gui_rect(client_x(w), y, client_w(w), MENU_H, C_FACE);
    x = client_x(w) + 4;
    k = 0;
    while (k < w->nmenus) {
        lw = ui_len(w->menu_titles[k]) * 7 + 12;
        if (menu_win == win_index(w) + 1 && menu_idx == k) gui_rect(x, y + 1, lw, MENU_H - 2, C_SEL);
        d_text(x + 6, y + 4, menu_win == win_index(w) + 1 && menu_idx == k ? C_WHITE : C_DARK, F_SANS, w->menu_titles[k]);
        x = x + lw;
        ++k;
    }
}

// Where menu k of window w sits on the bar.
static int menu_x(struct win *w, int k) {
    int x, j;
    x = client_x(w) + 4;
    j = 0;
    while (j < k) { x = x + ui_len(w->menu_titles[j]) * 7 + 12; ++j; }
    return x;
}

static int count_items(char **items) { int n; n = 0; while (items[n]) ++n; return n; }

static void draw_dropdown() {
    struct win *w;
    char **items;
    int n, x, y, k, iy, mw, mh;
    if (!(w = win_get(menu_win - 1))) return;
    items = w->menu_items[menu_idx];
    n = count_items(items);
    x = menu_x(w, menu_idx);
    y = w->y + BORDER + TITLE_H + 1 + MENU_H;
    mw = 160; mh = 6;
    k = 0; while (k < n) { if (items[k][0] == '-') mh = mh + 8; else mh = mh + 20; ++k; }
    gui_rect(x, y, mw, mh, C_FACE);
    d_bevel(x, y, mw, mh, B_RAISED);
    iy = y + 3;
    k = 0;
    while (k < n) {
        if (items[k][0] == '-') { gui_rect(x + 3, iy + 3, mw - 6, 1, C_SHADOW); gui_rect(x + 3, iy + 4, mw - 6, 1, C_LIGHT); iy = iy + 8; }
        else {
            if (k == menu_hot) gui_rect(x + 3, iy, mw - 6, 20, C_SEL);
            d_text(x + 20, iy + 4, k == menu_hot ? C_WHITE : C_DARK, F_SANS, items[k]);
            iy = iy + 20;
        }
        ++k;
    }
}

// Which item of the open drop-down is at (x, y), or -1.
static int dropdown_hit(int px, int py) {
    struct win *w;
    char **items;
    int n, x, y, k, iy;
    if (!(w = win_get(menu_win - 1))) return 0 - 1;
    items = w->menu_items[menu_idx];
    n = count_items(items);
    x = menu_x(w, menu_idx);
    y = w->y + BORDER + TITLE_H + 1 + MENU_H;
    if (px < x || px >= x + 160) return 0 - 1;
    iy = y + 3;
    k = 0;
    while (k < n) {
        if (items[k][0] == '-') iy = iy + 8;
        else { if (py >= iy && py < iy + 20) return k; iy = iy + 20; }
        ++k;
    }
    return 0 - 1;
}

static void draw_window(struct win *w, int focused) {
    int tx, bx, by, ink;
    gui_rect(w->x, w->y, w->w, w->h, C_FACE);
    if (ui_style == S_FLAT) {
        cap_bg = focused ? C_TITLE : C_ITITLE;
        gui_rect(w->x, w->y, w->w, BORDER + TITLE_H, cap_bg);
        gui_recto(w->x, w->y, w->w - 1, w->h - 1, focused ? C_TITLE : C_SHADOW);
        // white on the dark bar, the scheme's dark on a light one
        ink = (((cap_bg >> 16) & 255) + ((cap_bg >> 8) & 255) + (cap_bg & 255)) < 384 ? C_WHITE : 0x404850;
        cap_ink = ink;
    } else {
        d_bevel(w->x, w->y, w->w, w->h, B_RAISED);
        if (focused) gradient(w->x + BORDER, w->y + BORDER, w->w - 2 * BORDER, TITLE_H, C_TITLE, C_TITLE2);
        else gradient(w->x + BORDER, w->y + BORDER, w->w - 2 * BORDER, TITLE_H, C_ITITLE, C_ITITLE2);
        ink = focused ? C_LIGHT : C_FACE;
    }
    tx = w->x + BORDER + 3;
    if (!is_dialog(w)) { d_icon(w->kind, tx, w->y + BORDER + 1); tx = tx + 20; }
    wclip(w->x + BORDER, w->y + BORDER, w->w - 2 * BORDER - 56, TITLE_H);
    d_text(tx, w->y + BORDER + 3, ink, F_BOLD, w->title);
    wnoclip();
    by = w->y + BORDER + 2;
    bx = w->x + w->w - BORDER - 2 - 16;
    cap_button(bx, by, 0);
    if (!is_dialog(w)) {
        cap_button(bx - 18, by, w->max ? 2 : 1);
        cap_button(bx - 34, by, 3);
    }
    if (w->nmenus) draw_menubar(w, focused);
    clip_x = client_x(w); clip_y = client_y(w); clip_w = client_w(w); clip_h = client_h(w);
    wclip(clip_x, clip_y, clip_w, clip_h);
    app_draw(w, client_x(w), client_y(w), client_w(w), client_h(w), focused);
    wnoclip();
    if (w->resizable && !w->max) {                 // the size grip
        bx = w->x + w->w - BORDER - 12; by = w->y + w->h - BORDER - 12;
        gui_line(bx + 3, by + 11, bx + 11, by + 3, C_LIGHT); gui_line(bx + 4, by + 11, bx + 11, by + 4, C_SHADOW);
        gui_line(bx + 7, by + 11, bx + 11, by + 7, C_LIGHT); gui_line(bx + 8, by + 11, bx + 11, by + 8, C_SHADOW);
    }
}

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

static char *icon_labels[6];
static int icon_kinds[6];
enum { N_ICONS = 6 };

// The Go menu: a banner down the left, then the tools.
static char *start_items[14];
static int start_kinds[14];
static int n_start;

static int start_height() {
    int k, h;
    h = 6; k = 0;
    while (k < n_start) { h = h + (start_items[k][0] == '-' ? 8 : 26); ++k; }
    return h;
}

static void draw_start() {
    int mx, my, mw, mh, k, iy;
    char c[2], *banner;
    mw = 200; mh = start_height();
    mx = 2; my = SH - TASK_H - mh;
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
        if (start_items[k][0] == '-') {
            gui_rect(mx + 28, iy + 3, mw - 32, 1, C_SHADOW); gui_rect(mx + 28, iy + 4, mw - 32, 1, C_LIGHT);
            iy = iy + 8;
        } else {
            if (k == start_hot) gui_rect(mx + 26, iy, mw - 29, 26, C_SEL);
            if (start_kinds[k] > 0) d_icon(start_kinds[k], mx + 32, iy + 5);
            d_text(mx + 56, iy + 7, k == start_hot ? C_WHITE : C_DARK, F_SANS, start_items[k]);
            iy = iy + 26;
        }
        ++k;
    }
}

static int start_hit(int x, int y) {
    int mh, my, k, iy;
    mh = start_height();
    my = SH - TASK_H - mh;
    if (x < 28 || x >= 202 || y < my || y >= my + mh) return 0 - 1;
    iy = my + 3;
    k = 0;
    while (k < n_start) {
        if (start_items[k][0] == '-') iy = iy + 8;
        else { if (y >= iy && y < iy + 26) return k; iy = iy + 26; }
        ++k;
    }
    return 0 - 1;
}

static int n_buttons() {
    struct win *w;
    int n;
    n = 0; w = first;
    while (w) { if (w->used && !is_dialog(w)) ++n; w = w->next; }
    return n;
}
static int button_w() { int n, w; n = n_buttons(); if (!n) return 150; w = (SW - 64 - 154) / n - 4; return w > 150 ? 150 : w; }

// The CPU meter beside the clock: System Monitor's number (tasks_cpu_usage),
// taken every two seconds; a click opens System Monitor on Performance.
enum { CPU_X = SW - 146, CPU_W = 66 };
static int cpu_pct, cpu_n;

static void draw_taskbar() {
    int k, i, bx, bw, secs, h, m, f;
    char *p;
    struct win *w;
    gui_rect(0, SH - TASK_H, SW, TASK_H, C_FACE);
    gui_rect(0, SH - TASK_H + (ui_style == S_FLAT ? 0 : 1), SW, 1, ui_style == S_FLAT ? C_SHADOW : C_LIGHT);
    gui_rect(2, SH - TASK_H + 4, 56, 22, ui_style == S_FLAT && start_open ? C_LIGHT : C_FACE);
    if (ui_style == S_CLASSIC || start_open) d_bevel(2, SH - TASK_H + 4, 56, 22, start_open ? B_PRESSED : B_RAISED);
    // C4IX's mark: a cake (C4IX, C4KE -- cakes), its candle lit
    k = SH - TASK_H + 4;
    gui_rect(8, k + 13, 15, 6, 0x8b5a2b);
    gui_rect(8, k + 10, 15, 3, 0xf0a0c8); gui_rect(10, k + 13, 2, 2, 0xf0a0c8); gui_rect(17, k + 13, 2, 3, 0xf0a0c8);
    gui_rect(15, k + 4, 2, 6, 0xf4f4f4); gui_rect(15, k + 1, 2, 3, 0xffb020);
    d_text(28, SH - TASK_H + 9, C_DARK, F_BOLD, "Go");
    f = win_focused();
    bw = button_w();
    k = 0; w = first;
    while (w) {
        i = w->id;
        if (w->used && !is_dialog(w)) {
            bx = 64 + k * (bw + 4);
            if (ui_style == S_FLAT) {
                gui_rect(bx, SH - TASK_H + 4, bw, 22, i == f ? C_LIGHT : C_FACE);
                if (i == f) gui_rect(bx, SH - TASK_H + 24, bw, 2, C_TITLE);
            } else {
                gui_rect(bx, SH - TASK_H + 4, bw, 22, i == f ? 0xe0e0e0 : C_FACE);
                d_bevel(bx, SH - TASK_H + 4, bw, 22, i == f ? B_PRESSED : B_RAISED);
            }
            d_icon(w->kind, bx + 4, SH - TASK_H + 7);
            wclip(bx + 22, SH - TASK_H + 4, bw - 26, 22);
            d_text(bx + 24, SH - TASK_H + 9, C_DARK, i == f ? F_BOLD : F_SANS, w->title);
            wnoclip();
            ++k;
        }
        w = w->next;
    }
    gui_rect(CPU_X, SH - TASK_H + 4, CPU_W, 22, C_FACE);
    if (ui_style == S_CLASSIC) d_bevel(CPU_X, SH - TASK_H + 4, CPU_W, 22, B_SUNKEN);
    gui_rect(CPU_X + 5, SH - TASK_H + 8, 8, 14, 0x000000);
    h = (cpu_pct * 14 + 50) / 100;
    if (h > 0) gui_rect(CPU_X + 5, SH - TASK_H + 22 - h, 8, h, 0x00e000);
    ui_cpy(tbuf, "CPU ");
    ui_num(tbuf + 4, cpu_pct);
    ui_cat(tbuf, "%");
    d_text(CPU_X + 17, SH - TASK_H + 9, C_DARK, F_SANS, tbuf);
    gui_rect(SW - 76, SH - TASK_H + 4, 72, 22, C_FACE);
    if (ui_style == S_CLASSIC) d_bevel(SW - 76, SH - TASK_H + 4, 72, 22, B_SUNKEN);
    secs = gui_clock();
    h = secs / 3600; m = (secs / 60) % 60;
    p = tbuf;
    if (h % 12 == 0) { *p = '1'; ++p; *p = '2'; ++p; }
    else { if (h % 12 >= 10) { *p = '1'; ++p; } *p = '0' + (h % 12) % 10; ++p; }
    *p = ':'; ++p; *p = '0' + m / 10; ++p; *p = '0' + m % 10; ++p;
    *p = ' '; ++p; *p = h < 12 ? 'A' : 'P'; ++p; *p = 'M'; ++p; *p = 0;
    d_text(SW - 64, SH - TASK_H + 9, C_DARK, F_SANS, tbuf);
}

// the picture (wall.c), on the display as tiles it keeps
void d_imgdef(int id, int w, int h, int *px) { gui_imgdef(id, w, h, px); }
void d_image(int id, int x, int y) { gui_image(id, x, y); }

static void redraw() {
    int k;
    struct win *w, *f;
    gui_clear(C_DESK);
    wall_draw(0, 0, SW, SH);
    k = 0;
    while (k < N_ICONS) { icon(18, 14 + k * 72, icon_labels[k], icon_kinds[k]); ++k; }
    f = focus();
    w = zbot;
    while (w) {
        if (!w->min) draw_window(w, w == f);
        w = w->above;
    }
    if (menu_win) draw_dropdown();
    draw_taskbar();
    if (start_open) draw_start();
    gui_show();
    dirty = 0;
    dmg = 0;
}

// Only the damage (above): the desktop under it, the icons and windows
// that reach into it, back to front, and the taskbar if it does. A menu
// open is drawn over everything, so that is still the whole scene.
static void redraw_part() {
    int k;
    struct win *w, *f;
    if (menu_win || start_open) { redraw(); return; }
    part = 1;
    wnoclip();
    if (!wall_draw(dmg_x0, dmg_y0, dmg_x1 - dmg_x0, dmg_y1 - dmg_y0))
        gui_rect(dmg_x0, dmg_y0, dmg_x1 - dmg_x0, dmg_y1 - dmg_y0, C_DESK);
    k = 0;
    while (k < N_ICONS) {
        // an icon's label is centred under it and can be wider than it
        if (in_damage(0, 14 + k * 72, 80, 52)) icon(18, 14 + k * 72, icon_labels[k], icon_kinds[k]);
        ++k;
    }
    f = focus();
    w = zbot;
    while (w) {
        if (!w->min && in_damage(w->x, w->y, w->w, w->h)) draw_window(w, w == f);
        w = w->above;
    }
    if (in_damage(0, SH - TASK_H, SW, TASK_H)) draw_taskbar();
    gui_noclip();
    gui_show();
    part = 0;
    dmg = 0;
}

// ---- actions --------------------------------------------------------------------------

static void toggle_max(struct win *w) {
    if (!w->max) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->x = 0; w->y = 0; w->w = SW; w->h = SH - TASK_H; w->max = 1;
    } else {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh; w->max = 0;
    }
    dirty = 1;
}

static void launch(int kind) {
    if (kind == K_TERM) term_open(0);
    else if (kind == K_FILES) files_open("/");
    else if (kind == K_TASKS) tasks_open();
    else if (kind == K_NOTE) note_open(0);
    else if (kind == K_EDIT) edit_open(0);
    else if (kind == K_CALC) calc_open();
    else if (kind == K_SETTINGS) settings_open();
    else if (kind == K_ABOUT) about_open();
}

static void start_pick(int k) {
    start_open = 0; dirty = 1;
    if (k < 0) return;
    if (start_kinds[k] > 0) launch(start_kinds[k]);
    else if (start_kinds[k] == 0 - 1) inputbox(0, "Run", "Program to run:", "", A_RUN);
    else if (start_kinds[k] == 0 - 2) running = 0;
}

static void mouse_down(int x, int y, int button) {
    int k, i, bx, by, bw, t, cx, cy, m;
    struct win *w;
    if (start_open) {
        k = start_hit(x, y);
        start_open = 0; dirty = 1;
        if (k >= 0) start_pick(k);
        if (!ui_hit(x, y, 2, SH - TASK_H + 4, 56, 22)) return;
        return;
    }
    if (menu_win) {
        w = win_get(menu_win - 1);
        k = dropdown_hit(x, y);
        i = menu_idx;
        menu_win = 0; dirty = 1;
        if (!w) return;
        if (k >= 0) { app_command(w, i, k); return; }
        // a click on another title of the same bar opens that one instead
        if (y >= w->y + BORDER + TITLE_H + 1 && y < w->y + BORDER + TITLE_H + 1 + MENU_H) {
            m = 0;
            while (m < w->nmenus) {
                if (m != i && x >= menu_x(w, m) && x < menu_x(w, m) + ui_len(w->menu_titles[m]) * 7 + 12) {
                    menu_win = win_index(w) + 1; menu_idx = m; menu_hot = 0 - 1; return;
                }
                ++m;
            }
        }
        return;
    }
    if (y >= SH - TASK_H) {
        if (modal() >= 0) return;
        if (ui_hit(x, y, 2, SH - TASK_H + 4, 56, 22)) { start_open = 1; start_hot = 0 - 1; dirty = 1; return; }
        if (ui_hit(x, y, CPU_X, SH - TASK_H + 4, CPU_W, 22)) { tasks_open_perf(); dirty = 1; return; }
        bw = button_w();
        k = 0; w = first;
        while (w) {
            if (w->used && !is_dialog(w)) {
                if (ui_hit(x, y, 64 + k * (bw + 4), SH - TASK_H + 4, bw, 22)) {
                    if (w == focus()) w->min = 1;
                    else win_raise(w->id);
                    dirty = 1;
                    return;
                }
                ++k;
            }
            w = w->next;
        }
        return;
    }
    // windows, topmost first; a dialog keeps the input to itself
    w = ztop;
    while (w) {
        i = w->id;
        if (!w->min && ui_hit(x, y, w->x, w->y, w->w, w->h)) {
            if (modal() >= 0 && modal() != i) return;
            win_raise(i);
            by = w->y + BORDER + 2;
            bx = w->x + w->w - BORDER - 2 - 16;
            if (ui_hit(x, y, bx, by, 16, 14)) {
                if (is_dialog(w)) answer(w, 0); else win_close(i);
                return;
            }
            if (!is_dialog(w) && ui_hit(x, y, bx - 18, by, 16, 14)) { toggle_max(w); return; }
            if (!is_dialog(w) && ui_hit(x, y, bx - 34, by, 16, 14)) { w->min = 1; dirty = 1; return; }
            if (y < w->y + BORDER + TITLE_H) {
                t = now_ms();
                if (!is_dialog(w) && down_win == i + 1 && t - down_t < 500 && y < w->y + BORDER + TITLE_H) { toggle_max(w); down_win = 0; return; }
                down_win = i + 1; down_t = t;
                if (!w->max) { drag = i + 1; drag_dx = x - w->x; drag_dy = y - w->y; }
                return;
            }
            if (w->resizable && !w->max && x >= w->x + w->w - 16 && y >= w->y + w->h - 16) {
                sizing = i + 1; size_w0 = w->w; size_h0 = w->h; size_x0 = x; size_y0 = y;
                return;
            }
            if (w->nmenus && y < client_y(w)) {
                m = 0;
                while (m < w->nmenus) {
                    if (x >= menu_x(w, m) && x < menu_x(w, m) + ui_len(w->menu_titles[m]) * 7 + 12) {
                        menu_win = i + 1; menu_idx = m; menu_hot = 0 - 1; dirty = 1; return;
                    }
                    ++m;
                }
                return;
            }
            cx = x - client_x(w); cy = y - client_y(w);
            if (cx >= 0 && cy >= 0 && cx < client_w(w) && cy < client_h(w)) {
                t = now_ms();
                app_mouse(w, E_DOWN, cx, cy);
                if (w->used && down_win == i + 1 && t - down_t < 500 && x - down_x < 5 && down_x - x < 5 && y - down_y < 5 && down_y - y < 5) {
                    app_mouse(w, E_DBL, cx, cy);
                    down_win = 0;
                } else { down_win = i + 1; down_t = t; down_x = x; down_y = y; }
                dirty = 1;
            }
            return;
        }
        w = w->below;
    }
    if (modal() >= 0) return;
    // the desktop icons: a double click opens
    t = now_ms();
    k = 0 - 1;
    i = 0;
    while (i < N_ICONS) { if (ui_hit(x, y, 14, 12 + i * 72, 56, 64)) k = i; ++i; }
    if (k >= 0 && k == icon_k && t - icon_t < 500) {
        if (icon_kinds[k] == 0) files_open("/"); else launch(icon_kinds[k]);
        icon_k = 0 - 1;
    } else { icon_k = k; icon_t = t; }
}

// ---- the pointer: the host's own, its shape chosen by what is under it ----
static int ptr_shape;
static int shape_at(int x, int y) {
    struct win *w;
    int cx, cy;
    if (sizing) return GUI_PTR_RESIZE;
    if (drag || start_open || menu_win || y >= SH - TASK_H) return GUI_PTR_ARROW;
    w = ztop;
    while (w) {
        if (!w->min && ui_hit(x, y, w->x, w->y, w->w, w->h)) {
            if (w->resizable && !w->max && x >= w->x + w->w - 16 && y >= w->y + w->h - 16) return GUI_PTR_RESIZE;
            cx = x - client_x(w); cy = y - client_y(w);
            if (cx < 0 || cy < 0 || cx >= client_w(w) || cy >= client_h(w)) return GUI_PTR_ARROW;
            if (w->kind == K_TERM) return GUI_PTR_TEXT;
            if (w->kind == K_NOTE) return cx < client_w(w) - 18 ? GUI_PTR_TEXT : GUI_PTR_ARROW;
            if (w->kind == K_EDIT) return edit_pointer(w, cx, cy, client_w(w), client_h(w));
            return GUI_PTR_ARROW;
        }
        w = w->below;
    }
    return GUI_PTR_ARROW;
}
static void pointer(int x, int y) {
    int s;
    s = shape_at(x, y);
    if (s != ptr_shape) { ptr_shape = s; gui_cursor(s); }
}

static void mouse_move(int x, int y) {
    struct win *w;
    int k;
    pointer(x, y);
    if (drag && (w = win_get(drag - 1))) {
        w->x = x - drag_dx; w->y = y - drag_dy;
        if (w->y < 0) w->y = 0;
        if (w->y > SH - TASK_H - TITLE_H) w->y = SH - TASK_H - TITLE_H;
        if (w->x < 60 - w->w) w->x = 60 - w->w;
        if (w->x > SW - 60) w->x = SW - 60;
        dirty = 1;
        return;
    }
    if (sizing && (w = win_get(sizing - 1))) {
        w->w = size_w0 + x - size_x0; w->h = size_h0 + y - size_y0;
        if (w->w < 220) w->w = 220;
        if (w->h < 140) w->h = 140;
        dirty = 1;
        return;
    }
    if (start_open) { k = start_hit(x, y); if (k != start_hot) { start_hot = k; dirty = 1; } return; }
    if (menu_win) { k = dropdown_hit(x, y); if (k != menu_hot) { menu_hot = k; dirty = 1; } return; }
    if ((w = focus())) {
        // a thumb being dragged follows the pointer out of the window too
        if (ui_scroll_active() || ui_hit(x, y, client_x(w), client_y(w), client_w(w), client_h(w))) {
            app_mouse(w, E_MOVE, x - client_x(w), y - client_y(w));
            if (ui_scroll_active()) dirty = 1;          // the thumb moved: draw it there
        }
    }
}

static void mouse_up(int x, int y) {
    struct win *w;
    int k;
    drag = 0; sizing = 0;
    ui_scroll_end();
    pointer(x, y);
    if ((w = focus())) app_mouse(w, E_UP, x - client_x(w), y - client_y(w));
}

static void wheel(int x, int y, int dy) {
    struct win *w;
    w = ztop;
    while (w) {
        if (!w->min && ui_hit(x, y, client_x(w), client_y(w), client_w(w), client_h(w))) {
            app_mouse(w, E_WHEEL, x - client_x(w), dy);
            dirty = 1;
            return;
        }
        w = w->below;
    }
}

static void key_down(int code, int ch, int mods) {
    struct win *f;
    // typing between two clicks makes them two clicks, not a double click
    down_win = 0; icon_k = 0 - 1;
    // Ctrl+Shift+Esc: System Monitor, from anywhere
    if (code == KEY_ESC && (mods & M_CTRL) && (mods & M_SHIFT)) { tasks_open(); return; }
    if (start_open && code == KEY_ESC) { start_open = 0; dirty = 1; return; }
    if (menu_win && code == KEY_ESC) { menu_win = 0; dirty = 1; return; }
    if (!(f = focus())) return;
    app_key(f, code, ch, mods);
    dirty = 1;
}

static void shutdown_screen() {
    gui_clear(C_DARK);
    gui_text2(SW / 2 - 200, SH / 2 - 20, 0xff8c1a, 22, F_BOLD, 0, "C4IX has stopped. You can close this page.", 0 - 1);
    gui_show();
}

int main(int argc, char **argv) {
    int *region, k, busy, secs, clock_min, last_tick, last_draw, input, start_ms, second_draw;
    int last_input, last_frame;
    struct win *w;

    if (!gui_present()) { printf("desktop: not fitted\n"); return 0; }
    region = ualloc(65536);
    if (!gui_attach(region, 65536, SW, SH)) { printf("desktop: the display refused the ring\n"); return 1; }
    // GUI_M_WINDOW too: a resize or a focus change from the page means the
    // canvas may have been cleared, and the scene is drawn again
    gui_events(GUI_M_MOVE | GUI_M_BUTTONS | GUI_M_KEYS | GUI_M_WHEEL | GUI_M_WINDOW);
    theme_init();
    theme_apply();                     // the default scheme, unless the file names one
    theme_load();
    wall_load("/usr/share/splash/c4ix.c4p.gz");
    ev = ualloc(4 * sizeof(int));

    icon_labels[0] = "Filesystem"; icon_kinds[0] = 0;
    icon_labels[1] = "Terminal"; icon_kinds[1] = K_TERM;
    icon_labels[2] = "Text Editor"; icon_kinds[2] = K_NOTE;
    icon_labels[3] = "IDE"; icon_kinds[3] = K_EDIT;
    icon_labels[4] = "Calculator"; icon_kinds[4] = K_CALC;
    icon_labels[5] = "System Monitor"; icon_kinds[5] = K_TASKS;
    k = 0;
    start_items[k] = "Terminal"; start_kinds[k] = K_TERM; ++k;
    start_items[k] = "Files"; start_kinds[k] = K_FILES; ++k;
    start_items[k] = "Text Editor"; start_kinds[k] = K_NOTE; ++k;
    start_items[k] = "Editor"; start_kinds[k] = K_EDIT; ++k;
    start_items[k] = "Calculator"; start_kinds[k] = K_CALC; ++k;
    start_items[k] = "System Monitor"; start_kinds[k] = K_TASKS; ++k;
    start_items[k] = "Settings"; start_kinds[k] = K_SETTINGS; ++k;
    start_items[k] = "About C4IX"; start_kinds[k] = K_ABOUT; ++k;
    start_items[k] = "-"; start_kinds[k] = 0; ++k;
    start_items[k] = "Run..."; start_kinds[k] = 0 - 1; ++k;
    start_items[k] = "-"; start_kinds[k] = 0; ++k;
    start_items[k] = "Power Off..."; start_kinds[k] = 0 - 2; ++k;
    n_start = k;
    icon_k = 0 - 1;
    printf("desktop: running on the display. Go > Power Off to come back here.\n");

    term_open(0);
    running = 1; dirty = 1;
    clock_min = 0 - 1; last_tick = 0; last_draw = 0;
    start_ms = now_ms(); second_draw = 0;
    last_input = __time(); last_frame = 0;
    while (running) {
        busy = 0; input = 0;
        sweep();
        while (gui_poll(ev)) {
            busy = 1; input = 1;
            last_input = __time();
            // a screen saver running: the first key or movement ends it, and
            // goes no further (except while a Preview's click is let go)
            if (saver_active()) {
                if (__time() - saver_grace < 0) continue;
                if (ev[0] == GUI_EV_RESIZE || ev[0] == GUI_EV_FOCUS) continue;
                saver_stop();
                gui_cursor(GUI_PTR_ARROW);
                dirty = 1;
                continue;
            }
            if (ev[0] == GUI_EV_DOWN) mouse_down(ev[1], ev[2], ev[3]);
            else if (ev[0] == GUI_EV_UP) mouse_up(ev[1], ev[2]);
            else if (ev[0] == GUI_EV_MOVE) mouse_move(ev[1], ev[2]);
            else if (ev[0] == GUI_EV_KEYDOWN) key_down(ev[1], ev[2], ev[3]);
            else if (ev[0] == GUI_EV_WHEEL) wheel(ev[1], ev[2], ev[3]);
            else if (ev[0] == GUI_EV_RESIZE || ev[0] == GUI_EV_FOCUS) dirty = 1;
        }
        // the first frame, once more half a second in, in case the page was
        // not yet showing the display when it came
        if (!second_draw && now_ms() - start_ms >= 500) { second_draw = 1; dirty = 1; }
        w = first;
        while (w) {
            if (w->used && w->kind == K_TERM) term_tick(w);
            if (w->used && w->kind == K_EDIT) edit_tick(w);
            w = w->next;
        }
        // a tick a second for the windows that show live numbers: a
        // MACHINE second, which is what their numbers count (cpustat's ms)
        // and what the desktop sleeps on. Host time runs at another rate
        // whenever the machine is not paced to it -- headless, an idle
        // machine covers many seconds of its own in one of the host's.
        if (__time() - last_tick >= 1000) {
            last_tick = __time();
            w = first;
            while (w) { if (w->used && w->kind == K_TASKS) tasks_tick(w); w = w->next; }
            // the meter, every other tick, and a redraw only when it moved
            cpu_n = cpu_n + 1;
            if (cpu_n >= 2) {
                cpu_n = 0;
                k = tasks_cpu_usage();
                if (k != cpu_pct) { cpu_pct = k; mark_rect_dirty(0, SH - TASK_H, SW, TASK_H); }
            }
        }
        secs = gui_clock() / 60;
        if (secs != clock_min) { clock_min = secs; mark_rect_dirty(0, SH - TASK_H, SW, TASK_H); }
        // A redraw: the whole scene, or what is damaged (above). Input
        // gets one at once; a program pouring out text (raycast, top) gets
        // at most one every 25 ms -- it can print far faster than a person
        // can read. Machine time (__time), the clock the desktop sleeps on:
        // host time runs at a different rate whenever the machine is not
        // paced to it.
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
            uevwait(k, gui_in, 0);
            continue;
        }
        if ((dirty || dmg) && (input || __time() - last_draw >= 25)) {
            if (dirty) redraw(); else redraw_part();
            last_draw = __time();
            busy = 1;
        }
        // Nothing to do: sleep until there is. That is an event (a key, the
        // mouse), output on a terminal's pipe, or the next thing due -- a
        // redraw held back by the 25 ms above, System Monitor's tick -- and
        // the kernel watches for the first two itself (SYS_EVWAIT), where
        // this used to wake every 15 ms to look. While a redraw is held
        // back the pipes are not watched: a program pouring out text would
        // wake it at once, with nothing to do until the 25 ms are up.
        if (!busy) {
            if (dirty || dmg) k = 25 - (__time() - last_draw);
            else k = 1000 - (__time() - last_tick);
            if (!second_draw && k > 100) k = 100;   // the half-second first-frame redraw
            if (k < 1) k = 1;
            uevwait(k, gui_in, !(dirty || dmg));
        }
    }

    w = first;
    while (w) { if (w->used) win_close(w->id); w = w->next; }
    sweep();
    shutdown_screen();
    gui_detach();
    printf("desktop: shut down\n");
    return 0;
}
