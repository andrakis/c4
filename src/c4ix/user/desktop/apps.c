// apps.c -- About and Settings. docs/c4ix-desktop.md. (The Calculator and the
// Text Editor are the window kit's: calc.c and notepad.c in src/winkit.)

#include "desktop.h"

// ---- About ---------------------------------------------------------------------------------

void about_open() {
    int i, k;
    struct win *w;
    w = win_first();
    while (w) { if (w->used && w->kind == K_ABOUT) { win_raise(w->id); return; } w = w->next; }
    i = win_open(K_ABOUT, "About C4IX", 440, 180, 0);
    if (i < 0) return;
    w = win_get(i);
    w->resizable = 0;
    w->x = (wk.sw - 440) / 2; w->y = (wk.sh - 180) / 2 - 60;
}

void about_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    d_rect(ox, oy, cw, ch, C_FACE);
    d_rect(ox + 16, oy + 16, 36, 36, C_TITLE);
    d_rect(ox + 20, oy + 20, 13, 13, 0xff2020); d_rect(ox + 35, oy + 20, 13, 13, 0x20c020);
    d_rect(ox + 20, oy + 35, 13, 13, 0x2040ff); d_rect(ox + 35, oy + 35, 13, 13, 0xffd020);
    d_text(ox + 68, oy + 16, C_DARK, F_BOLD, "C4IX Desktop");
    d_text(ox + 68, oy + 36, C_DARK, F_SANS, "Running on c4m.js: the c4m virtual machine in");
    d_text(ox + 68, oy + 52, C_DARK, F_SANS, "JavaScript, in a Web Worker.");
    d_text(ox + 68, oy + 76, C_DARK, F_SANS, "Every Terminal is a real c4ix-sh task; its");
    d_text(ox + 68, oy + 92, C_DARK, F_SANS, "stdin and stdout are pipes the desktop holds.");
    d_text(ox + 68, oy + 116, C_SHADOW, F_SANS, "Enter or Esc closes this window.");
}

// ---- Settings: the scheme, the desktop, the screen's size, the saver and the look ----
//
// Display Properties, cut down: a list of schemes and a row of desktop
// colours, each applied at once (theme_set, which also remembers them in
// /etc/desktop.cfg), beside a preview drawn in the colours in use. The
// screen's size too, which the display takes at once (display_set).

static char *dflt_label;
static char sbuf[16];

int settings_open() {
    int k;
    struct win *w;
    w = win_first();
    while (w) { if (w->used && w->kind == K_SETTINGS) { win_raise(w->id); return w->id; } w = w->next; }
    dflt_label = "Defaults";
    return win_open(K_SETTINGS, "Settings", 440, 450, 0);
}

void settings_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    int i, x, y, sel, n;
    d_rect(ox, oy, cw, ch, C_FACE);
    // the schemes
    n = theme_count();
    ui_group(ox + 8, oy + 8, 180, 20 + n * 20, "Color scheme");
    sel = theme_scheme();
    i = 0;
    while (i < n) { ui_radio(ox + 18, oy + 26 + i * 20, theme_name(i), i == sel); ++i; }
    // the desktop: the picture, the scheme's colour, or one of eight
    y = oy + 36 + n * 20;
    ui_group(ox + 8, y, 180, 94, "Desktop");
    ui_radio(ox + 18, y + 16, "Picture: C4IX", theme_wall());
    ui_radio(ox + 18, y + 36, "The scheme's colour", !theme_wall() && theme_bg() < 0);
    i = 0;
    while (i < 8) {
        x = ox + 18 + i * 20;
        sel = !theme_wall() && theme_bg() == theme_swatch(i);
        d_rect(x, y + 60, 16, 16, theme_swatch(i));
        d_recto(x - 1, y + 59, 18, 18, sel ? C_DARK : C_SHADOW);
        if (sel) d_recto(x - 2, y + 58, 20, 20, C_DARK);
        ++i;
    }
    // the screen's size: two to a row
    y = y + 102;
    ui_group(ox + 8, y, 180, 60, "Screen size");
    i = 0;
    while (i < display_count()) { ui_radio(ox + 18 + (i % 2) * 84, y + 16 + (i / 2) * 20, display_name(i), i == display_cur()); ++i; }
    // the preview: the desktop, an inactive window, an active one
    x = ox + 200; y = oy + 14;
    d_rect(x, y, cw - 210, 170, C_DESK);
    d_bevel(x, y, cw - 210, 170, B_SUNKEN);
    d_rect(x + 12, y + 14, 150, 70, C_FACE); d_bevel(x + 12, y + 14, 150, 70, B_RAISED);
    d_rect(x + 15, y + 17, 144, 16, C_ITITLE);
    // its title in the scheme's dark when the bar is light, as the windows' (wm.c)
    i = ((C_ITITLE >> 16) & 255) + ((C_ITITLE >> 8) & 255) + (C_ITITLE & 255);
    d_text(x + 19, y + 19, ui_style == S_FLAT && i >= 384 ? 0x404850 : C_FACE, F_BOLD, "Inactive Window");
    d_rect(x + 28, y + 50, 170, 100, C_FACE); d_bevel(x + 28, y + 50, 170, 100, B_RAISED);
    d_rect(x + 31, y + 53, 164, 16, C_TITLE);
    d_text(x + 35, y + 55, C_LIGHT, F_BOLD, "Active Window");
    d_rect(x + 36, y + 76, 150, 40, C_WHITE); d_bevel(x + 36, y + 76, 150, 40, B_SUNKEN);
    d_text(x + 42, y + 80, C_DARK, F_SANS, "Normal");
    d_rect(x + 40, y + 96, 60, 15, C_SEL);
    d_text(x + 42, y + 97, C_WHITE, F_SANS, "Selected");
    ui_button(x + 110, y + 122, 76, 22, "OK", 0, 1);
    // the screen saver: which, after how long, and a look at it now
    x = ox + 200; y = oy + 192;
    n = saver_count();
    ui_group(x, y, cw - 210, 26 + n * 18, "Screen saver");
    sel = theme_saver();
    i = 0;
    while (i < n) { ui_radio(x + 10, y + 16 + i * 18, saver_name(i), i == sel); ++i; }
    ui_button(x + 130, y + 14, 70, 22, "Preview", 0, 0);
    ui_num(sbuf, theme_wait());
    ui_cat(sbuf, " min");
    d_text(x + 130, y + 46, C_DARK, F_SANS, "Wait:");
    ui_button(x + 130, y + 62, 22, 20, "-", 0, 0);
    d_text(x + 156, y + 66, C_DARK, F_SANS, sbuf);
    ui_button(x + 192, y + 62, 22, 20, "+", 0, 0);
    // the look
    y = oy + 192 + 26 + n * 18 + 8;
    ui_group(x, y, cw - 210, 44, "Look");
    ui_radio(x + 10, y + 18, "Flat", ui_style == S_FLAT);
    ui_radio(x + 70, y + 18, "Classic", ui_style == S_CLASSIC);
    ui_radio(x + 140, y + 18, "Windows 3.1", ui_style == S_WIN31);
    ui_button(ox + 8, oy + ch - 34, 86, 24, dflt_label, 0, 0);
}

void settings_mouse(struct win *w, int ev, int x, int y, int cw, int ch) {
    int n, i, sy, k;
    if (ev != E_DOWN) return;
    n = theme_count();
    i = 0;
    while (i < n) {
        if (ui_hit(x, y, 14, 24 + i * 20, 170, 18)) { k = theme_wall(); theme_set(i, theme_bg()); theme_set_wall(k); return; }
        ++i;
    }
    sy = 36 + n * 20;
    if (ui_hit(x, y, 14, sy + 14, 170, 18)) { theme_set_wall(1); return; }
    if (ui_hit(x, y, 14, sy + 34, 170, 18)) { theme_set(theme_scheme(), 0 - 1); return; }
    i = 0;
    while (i < 8) {
        if (ui_hit(x, y, 16 + i * 20, sy + 58, 20, 20)) { theme_set(theme_scheme(), theme_swatch(i)); return; }
        ++i;
    }
    // the screen's size (settings_draw)
    i = 0;
    while (i < display_count()) {
        if (ui_hit(x, y, 14 + (i % 2) * 84, sy + 102 + 14 + (i / 2) * 20, 80, 18)) { display_set(i); return; }
        ++i;
    }
    // the screen saver group (settings_draw)
    i = 0;
    while (i < saver_count()) {
        if (ui_hit(x, y, 206, 206 + i * 18, 120, 16)) { theme_set_saver(i, theme_wait()); return; }
        ++i;
    }
    if (ui_hit(x, y, 330, 206, 70, 22)) { saver_preview(); return; }
    if (ui_hit(x, y, 330, 254, 22, 20)) { theme_set_saver(theme_saver(), theme_wait() - 1); return; }
    if (ui_hit(x, y, 392, 254, 22, 20)) { theme_set_saver(theme_saver(), theme_wait() + 1); return; }
    // the look group (settings_draw): under the savers
    sy = 192 + 26 + saver_count() * 18 + 8 + 18;
    if (ui_hit(x, y, 206, sy - 2, 60, 16)) { theme_set_style(S_FLAT); return; }
    if (ui_hit(x, y, 266, sy - 2, 66, 16)) { theme_set_style(S_CLASSIC); return; }
    if (ui_hit(x, y, 336, sy - 2, 90, 16)) { theme_set_style(S_WIN31); return; }
    if (ui_hit(x, y, 8, ch - 34, 86, 24)) { theme_set(theme_count() - 1, 0 - 1); theme_set_wall(1); theme_set_style(S_FLAT); }
}

// Up and Down walk the schemes; Esc or Enter closes
void settings_key(struct win *w, int code, int ch, int mods) {
    int s, w2;
    s = theme_scheme();
    w2 = theme_wall();
    if (code == KEY_UP && s > 0) { theme_set(s - 1, theme_bg()); theme_set_wall(w2); }
    else if (code == KEY_DOWN && s < theme_count() - 1) { theme_set(s + 1, theme_bg()); theme_set_wall(w2); }
    else if (code == KEY_ESC || code == KEY_ENTER) win_close(win_index(w));
}
