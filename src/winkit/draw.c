// draw.c -- the window kit's drawing layer, over the display (gui.h).
// docs/grin-plan.md 3.5.
//
// Everything a window draws goes through d_*, relative to an origin --
// its client area's corner while its class draws, the screen's for the
// chrome and the shell -- and clipped to that client area. A redraw is
// either the whole scene, or only the damage: what changed on its own (a
// terminal's output, a clock) marks its rectangle, and the redraw draws
// only what lies in it, clipped to it, with no clear: the display keeps
// the rest of the last frame (docs/c4ix-cpu.md, step 11).

#include "wkwin.h"
#include "libjs/guest/gui.h"

struct wk_env wk;

// ---- colour schemes ----------------------------------------------------------
//
// Nine colours each: desktop, face, light and shadow edges, the active
// and inactive title bars (each a gradient's two ends), and the
// selection, which is the active title's first colour.
enum { N_SCHEMES = 9, S_COLS = 8 };
static char *scheme_names[N_SCHEMES] = {
    "Classic Teal", "Granite", "Drizzle", "Sandstone", "Blush", "Pine", "Lavender", "Thunder",
    "Modern"                          // the flat look's own: cool greys, slate title bars
};
static int schemes[N_SCHEMES * S_COLS] = {
    0x008080, 0xc0c0c0, 0xffffff, 0x808080, 0x000080, 0x1084d0, 0x808080, 0xb4b4b4,
    0x3a4e62, 0xb8c4cc, 0xeef3f6, 0x687884, 0x384c60, 0x7890a8, 0x808c98, 0xb0bcc8,
    0x48586c, 0xb4bcc4, 0xeef2f6, 0x707880, 0x4c5c7c, 0x8898b8, 0x80888c, 0xb0b8c0,
    0xa08060, 0xd5ccbb, 0xf5f0e8, 0xa29275, 0x008080, 0x40b0b0, 0xa29275, 0xc8bca8,
    0x804050, 0xcfafb7, 0xf3e4e8, 0x9f6070, 0x9f6070, 0xd090a0, 0xa08890, 0xc8b0b8,
    0x285040, 0xa2c8a9, 0xe2f2e6, 0x5e9a6a, 0x184830, 0x408048, 0x6a8a70, 0x9ab8a0,
    0x504878, 0xaea8d9, 0xeae6f8, 0x6c64a8, 0x504888, 0x8880c8, 0x8880a0, 0xb4b0d0,
    0x202830, 0xa8a8a8, 0xe8e8e8, 0x606060, 0x303848, 0x607088, 0x707070, 0x989898,
    0x2e3440, 0xeceff4, 0xffffff, 0xa0a8b4, 0x3b4252, 0x3b4252, 0xd8dee9, 0xd8dee9
};

int wk_scheme_count() { return N_SCHEMES; }
char *wk_scheme_name(int i) { return i >= 0 && i < N_SCHEMES ? scheme_names[i] : ""; }
void wk_scheme_use(int i, int desk) {
    int *t;
    if (i < 0 || i >= N_SCHEMES) i = 0;
    t = schemes + i * S_COLS;
    wk.desk = desk >= 0 ? desk : t[0];
    wk.face = t[1]; wk.light = t[2]; wk.shadow = t[3];
    wk.title = t[4]; wk.title2 = t[5]; wk.ititle = t[6]; wk.ititle2 = t[7]; wk.sel = t[4];
    wk_full = 1;
}

int ui_mix(int a, int b, int t, int n) {
    int r, g, bl;
    r = ((a >> 16) & 255) + ((((b >> 16) & 255) - ((a >> 16) & 255)) * t) / n;
    g = ((a >> 8) & 255) + ((((b >> 8) & 255) - ((a >> 8) & 255)) * t) / n;
    bl = (a & 255) + (((b & 255) - (a & 255)) * t) / n;
    return (r << 16) | (g << 8) | bl;
}

// ---- damage --------------------------------------------------------------------

int wk_full, wk_part;
int wk_dmg, wk_dmg_x0, wk_dmg_y0, wk_dmg_x1, wk_dmg_y1;

void wk_redraw_all() { wk_full = 1; }

void wk_dirty_rect(int x, int y, int w, int h) {
    if (wk_full || w <= 0 || h <= 0) return;
    if (x < 0) { w = w + x; x = 0; }
    if (y < 0) { h = h + y; y = 0; }
    if (x + w > wk.sw) w = wk.sw - x;
    if (y + h > wk.sh) h = wk.sh - y;
    if (w <= 0 || h <= 0) return;
    if (!wk_dmg) { wk_dmg = 1; wk_dmg_x0 = x; wk_dmg_y0 = y; wk_dmg_x1 = x + w; wk_dmg_y1 = y + h; return; }
    if (x < wk_dmg_x0) wk_dmg_x0 = x;
    if (y < wk_dmg_y0) wk_dmg_y0 = y;
    if (x + w > wk_dmg_x1) wk_dmg_x1 = x + w;
    if (y + h > wk_dmg_y1) wk_dmg_y1 = y + h;
}

int wk_in_damage(int x, int y, int w, int h) {
    if (!wk_part) return 1;
    return x < wk_dmg_x1 && x + w > wk_dmg_x0 && y < wk_dmg_y1 && y + h > wk_dmg_y0;
}

// Clipping, always inside the damage while only the damage is drawn: the
// clip a window sets for its title or its client area must not reach past
// what is being redrawn, or it would draw over what is not.
void wk_clip_screen(int x, int y, int w, int h) {
    int x1, y1;
    if (wk_part) {
        x1 = x + w; y1 = y + h;
        if (x < wk_dmg_x0) x = wk_dmg_x0;
        if (y < wk_dmg_y0) y = wk_dmg_y0;
        if (x1 > wk_dmg_x1) x1 = wk_dmg_x1;
        if (y1 > wk_dmg_y1) y1 = wk_dmg_y1;
        w = x1 > x ? x1 - x : 0;
        h = y1 > y ? y1 - y : 0;
    }
    gui_clip(x, y, w, h);
}
void wk_noclip() {
    if (wk_part) gui_clip(wk_dmg_x0, wk_dmg_y0, wk_dmg_x1 - wk_dmg_x0, wk_dmg_y1 - wk_dmg_y0);
    else gui_noclip();
}

// ---- the origin, and drawing narrowed to part of a client area ------------------

static int ox, oy;                        // where d_* draw from, on the screen
static int clip_x, clip_y, clip_w, clip_h;

void wk_origin(int x, int y) { ox = x; oy = y; }
void wk_client_clip(int x, int y, int w, int h) {
    clip_x = x; clip_y = y; clip_w = w; clip_h = h;
    wk_clip_screen(x, y, w, h);
}

// An app's pane, and back to all of its client area: never wider than that.
void d_clip(int x, int y, int w, int h) {
    int x1, y1;
    x = x + ox; y = y + oy;
    x1 = x + w; y1 = y + h;
    if (x < clip_x) x = clip_x;
    if (y < clip_y) y = clip_y;
    if (x1 > clip_x + clip_w) x1 = clip_x + clip_w;
    if (y1 > clip_y + clip_h) y1 = clip_y + clip_h;
    wk_clip_screen(x, y, x1 > x ? x1 - x : 0, y1 > y ? y1 - y : 0);
}
void d_clip_end() { wk_clip_screen(clip_x, clip_y, clip_w, clip_h); }

// ---- the drawing -----------------------------------------------------------------

void d_rect(int x, int y, int w, int h, int rgb) { if (w > 0 && h > 0) gui_rect(x + ox, y + oy, w, h, rgb); }
void d_recto(int x, int y, int w, int h, int rgb) { gui_recto(x + ox, y + oy, w, h, rgb); }
void d_line(int x0, int y0, int x1, int y1, int rgb) { gui_line(x0 + ox, y0 + oy, x1 + ox, y1 + oy, rgb); }
void d_disc(int x, int y, int r, int rgb) { gui_disc(x + ox, y + oy, r, rgb); }
void d_text(int x, int y, int rgb, int font, char *s) { gui_text2(x + ox, y + oy, rgb, 11, font, 0, s, 0 - 1); }
void d_textn(int x, int y, int rgb, int size, int font, int advance, char *s, int n) {
    gui_text2(x + ox, y + oy, rgb, size, font, advance, s, n);
}
void d_imgdef(int id, int w, int h, int *px) { gui_imgdef(id, w, h, px); }
void d_image(int id, int x, int y) { gui_image(id, x + ox, y + oy); }
void d_blit8(int x, int y, int w, int h, char *src, int pitch, int *pal, int dw, int dh) {
    gui_blit8(x + ox, y + oy, w, h, src, pitch, pal, dw, dh);
}

// The bevels: two pixels, light top-left, dark bottom-right (or the other
// way round for a sunken one). Flat draws an outline, the accent's when
// pressed; Classic and Windows 3.1 bevel.
void d_bevel(int x, int y, int w, int h, int style) {
    int a, b, c, d;
    x = x + ox; y = y + oy;
    if (wk.style == S_FLAT) {
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
    x = x + ox; y = y + oy;
    if (kind == ICON_TERM) {
        gui_rect(x, y + 1, 16, 13, C_DARK); gui_recto(x, y + 1, 16, 13, C_SHADOW);
        gui_text2(x + 2, y + 3, 0xc0c0c0, 8, F_MONO, 0, ">_", 2);
    } else if (kind == ICON_FILES) {
        gui_rect(x, y + 3, 7, 3, 0xc8a000); gui_rect(x, y + 5, 16, 10, 0xffd84a);
        gui_recto(x, y + 5, 16, 10, 0x806000);
    } else if (kind == ICON_TASKS) {
        gui_rect(x, y + 1, 16, 12, C_DARK); gui_recto(x, y + 1, 16, 12, C_SHADOW);
        gui_line(x + 2, y + 10, x + 6, y + 5, C_GREEN); gui_line(x + 6, y + 5, x + 9, y + 8, C_GREEN);
        gui_line(x + 9, y + 8, x + 14, y + 3, C_GREEN); gui_rect(x + 5, y + 13, 6, 2, C_SHADOW);
    } else if (kind == ICON_EDIT) {
        gui_rect(x + 1, y, 14, 16, C_WHITE); gui_recto(x + 1, y, 14, 16, C_SHADOW);
        gui_text2(x + 2, y + 3, 0x008000, 8, F_MONO, 0, "{}", 2);
        gui_rect(x + 3, y + 12, 10, 1, 0x4060c0);
    } else if (kind == ICON_NOTE) {
        gui_rect(x + 2, y, 12, 16, C_WHITE); gui_recto(x + 2, y, 12, 16, C_SHADOW);
        gui_rect(x + 4, y + 4, 8, 1, 0x4060c0); gui_rect(x + 4, y + 7, 8, 1, 0x4060c0);
        gui_rect(x + 4, y + 10, 6, 1, 0x4060c0);
    } else if (kind == ICON_SETTINGS) {
        // a gear: a disc with teeth, and a hole
        gui_rect(x + 7, y, 3, 16, 0x707070); gui_rect(x, y + 7, 16, 3, 0x707070);
        gui_line(x + 2, y + 2, x + 13, y + 13, 0x707070); gui_line(x + 13, y + 2, x + 2, y + 13, 0x707070);
        gui_line(x + 3, y + 2, x + 14, y + 13, 0x707070); gui_line(x + 12, y + 2, x + 1, y + 13, 0x707070);
        gui_disc(x + 8, y + 8, 6, 0x909090);
        gui_disc(x + 8, y + 8, 2, C_FACE);
    } else if (kind == ICON_CALC) {
        gui_rect(x + 2, y, 12, 16, 0xa0a0a0); gui_recto(x + 2, y, 12, 16, C_DARK);
        gui_rect(x + 4, y + 2, 8, 4, 0xd0ffd0);
        gui_rect(x + 4, y + 8, 3, 2, C_DARK); gui_rect(x + 9, y + 8, 3, 2, C_DARK);
        gui_rect(x + 4, y + 12, 3, 2, C_DARK); gui_rect(x + 9, y + 12, 3, 2, 0xc02020);
    } else if (kind == ICON_CLOCK) {
        gui_disc(x + 8, y + 8, 7, C_DARK); gui_disc(x + 8, y + 8, 6, C_WHITE);
        gui_line(x + 8, y + 8, x + 8, y + 4, C_DARK); gui_line(x + 8, y + 8, x + 11, y + 8, C_DARK);
    } else {
        gui_rect(x + 1, y + 1, 14, 14, C_WHITE); gui_recto(x + 1, y + 1, 14, 14, C_TITLE);
        gui_text2(x + 6, y + 2, C_TITLE, 12, F_BOLD, 0, "i", 1);
    }
}

int wk_clock() { return gui_clock(); }
