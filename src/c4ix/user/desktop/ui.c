// ui.c -- widgets, drawn through wm.c's d_* layer. docs/c4ix-desktop.md.
//
// Stateless: each call draws one thing, or answers whether a point hit
// it. The tools keep their own state (selection, scroll position, which
// field has the caret) and pass it in.

#include "desktop.h"

// ---- strings ----------------------------------------------------------------

int ui_len(char *s) { int n; n = 0; while (s[n]) ++n; return n; }
void ui_cpy(char *d, char *s) { while (*s) { *d = *s; ++d; ++s; } *d = 0; }
void ui_cat(char *d, char *s) { while (*d) ++d; ui_cpy(d, s); }
int ui_eq(char *a, char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
int ui_ends(char *s, char *suffix) {
    int n, m;
    n = ui_len(s); m = ui_len(suffix);
    if (m > n) return 0;
    return ui_eq(s + n - m, suffix);
}

char *ui_num(char *buf, int v) {
    char t[16];
    int n, neg, k;
    neg = v < 0;
    if (neg) v = 0 - v;
    n = 0;
    while (1) { t[n] = '0' + v % 10; ++n; v = v / 10; if (!v) break; }
    k = 0;
    if (neg) { buf[k] = '-'; ++k; }
    while (n) { --n; buf[k] = t[n]; ++k; }
    buf[k] = 0;
    return buf;
}

char *ui_num_commas(char *buf, int v) {
    char t[20];
    int n, k, i;
    ui_num(t, v);
    n = ui_len(t);
    k = 0;
    i = 0;
    while (i < n) {
        buf[k] = t[i]; ++k;
        if (t[i] != '-' && (n - i - 1) % 3 == 0 && i < n - 1) { buf[k] = ','; ++k; }
        ++i;
    }
    buf[k] = 0;
    return buf;
}

int ui_hit(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

// ---- controls ------------------------------------------------------------------

void ui_button(int x, int y, int w, int h, char *label, int pressed, int deflt) {
    int lw;
    d_rect(x, y, w, h, C_FACE);
    if (deflt) { d_recto(x - 1, y - 1, w + 1, h + 1, C_DARK); }
    d_bevel(x, y, w, h, pressed ? B_PRESSED : B_RAISED);
    lw = ui_len(label) * 6;
    d_text(x + (w - lw) / 2 + (pressed ? 1 : 0), y + (h - 12) / 2 + (pressed ? 1 : 0), C_DARK, F_SANS, label);
}

// A vertical scroll bar: arrows at both ends, a thumb sized to what is
// visible. Nothing to scroll draws it disabled.
void ui_scrollbar(int x, int y, int h, int total, int visible, int top) {
    int track, th, ty;
    d_rect(x, y, 16, h, 0xe0e0e0);
    d_rect(x, y, 16, 16, C_FACE); d_bevel(x, y, 16, 16, B_RAISED);
    d_rect(x, y + h - 16, 16, 16, C_FACE); d_bevel(x, y + h - 16, 16, 16, B_RAISED);
    d_line(x + 8, y + 5, x + 4, y + 9, C_DARK); d_line(x + 8, y + 5, x + 12, y + 9, C_DARK);
    d_rect(x + 4, y + 9, 9, 1, C_DARK);
    d_line(x + 4, y + h - 10, x + 8, y + h - 6, C_DARK); d_line(x + 12, y + h - 10, x + 8, y + h - 6, C_DARK);
    d_rect(x + 4, y + h - 11, 9, 1, C_DARK);
    track = h - 32;
    if (total <= visible || track < 8) return;
    th = (track * visible) / total;
    if (th < 10) th = 10;
    ty = y + 16 + ((track - th) * top) / (total - visible);
    d_rect(x, ty, 16, th, C_FACE);
    d_bevel(x, ty, 16, th, B_RAISED);
}

// The new top line for a click on the scroll bar, or -1 if it missed.
int ui_scroll_click(int px, int py, int x, int y, int h, int total, int visible, int top) {
    int track, th, ty, max;
    if (!ui_hit(px, py, x, y, 16, h)) return 0 - 1;
    max = total - visible;
    if (max < 0) max = 0;
    if (py < y + 16) top = top - 1;
    else if (py >= y + h - 16) top = top + 1;
    else {
        track = h - 32;
        if (total <= visible) return top;
        th = (track * visible) / total;
        if (th < 10) th = 10;
        ty = y + 16 + ((track - th) * top) / (total - visible);
        if (py < ty) top = top - visible;
        else if (py >= ty + th) top = top + visible;
    }
    if (top > max) top = max;
    if (top < 0) top = 0;
    return top;
}

// A single-line text field: sunken white, monospace so the caret can sit
// exactly between two characters (7 pixels a character).
void ui_field(int x, int y, int w, char *text, int caret, int focused) {
    int n, first, show;
    d_rect(x, y, w, 22, C_WHITE);
    d_bevel(x, y, w, 22, B_SUNKEN);
    n = ui_len(text);
    show = (w - 10) / 7;
    first = 0;
    if (caret > show) first = caret - show;
    d_textn(x + 4, y + 5, C_DARK, 12, F_MONO, 7, text + first, n - first > show ? show : n - first);
    if (focused) d_rect(x + 4 + (caret - first) * 7, y + 4, 1, 14, C_DARK);
}

// Editing keys for a field's text; returns 1 if the text changed.
int ui_field_key(char *text, int max, int *caret, int code, int ch, int mods) {
    int n, i;
    n = ui_len(text);
    if (code == KEY_LEFT) { if (*caret > 0) *caret = *caret - 1; return 0; }
    if (code == KEY_RIGHT) { if (*caret < n) *caret = *caret + 1; return 0; }
    if (code == KEY_HOME) { *caret = 0; return 0; }
    if (code == KEY_END) { *caret = n; return 0; }
    if (code == KEY_BACK) {
        if (*caret == 0) return 0;
        i = *caret - 1;
        while (i < n) { text[i] = text[i + 1]; ++i; }
        *caret = *caret - 1;
        return 1;
    }
    if (code == KEY_DEL) {
        if (*caret >= n) return 0;
        i = *caret;
        while (i < n) { text[i] = text[i + 1]; ++i; }
        return 1;
    }
    if ((mods & M_CTRL) || ch < 32 || ch > 126 || n >= max) return 0;
    i = n + 1;
    while (i > *caret) { text[i] = text[i - 1]; --i; }
    text[*caret] = ch;
    *caret = *caret + 1;
    return 1;
}

// Tabs across the top of a property sheet, the selected one raised into
// the page below it.
void ui_tabs(int x, int y, char **labels, int n, int sel) {
    int k, tx, tw;
    tx = x;
    k = 0;
    while (k < n) {
        tw = ui_len(labels[k]) * 7 + 16;
        if (k == sel) {
            d_rect(tx - 2, y, tw + 4, 22, C_FACE);
            d_rect(tx - 2, y, tw + 4, 1, C_LIGHT); d_rect(tx - 2, y, 1, 22, C_LIGHT);
            d_rect(tx + tw + 1, y + 1, 1, 21, C_DARK);
            d_text(tx + 8, y + 5, C_DARK, F_SANS, labels[k]);
        } else {
            d_rect(tx, y + 3, tw, 1, C_LIGHT); d_rect(tx, y + 3, 1, 19, C_LIGHT);
            d_rect(tx + tw - 1, y + 4, 1, 18, C_DARK);
            d_text(tx + 8, y + 7, C_DARK, F_SANS, labels[k]);
        }
        tx = tx + tw;
        ++k;
    }
}

int ui_tab_hit(int px, int py, int x, int y, char **labels, int n) {
    int k, tx, tw;
    if (py < y || py >= y + 22) return 0 - 1;
    tx = x;
    k = 0;
    while (k < n) {
        tw = ui_len(labels[k]) * 7 + 16;
        if (px >= tx && px < tx + tw) return k;
        tx = tx + tw;
        ++k;
    }
    return 0 - 1;
}

// A group box: an etched frame with its label set into the top edge.
void ui_group(int x, int y, int w, int h, char *label) {
    d_bevel(x, y + 6, w, h - 6, B_ETCHED);
    d_rect(x + 8, y, ui_len(label) * 6 + 6, 12, C_FACE);
    d_text(x + 10, y, C_DARK, F_SANS, label);
}

void ui_radio(int x, int y, char *label, int on) {
    d_disc(x + 6, y + 6, 6, C_SHADOW);
    d_disc(x + 6, y + 6, 5, C_WHITE);
    if (on) d_disc(x + 6, y + 6, 2, C_DARK);
    d_text(x + 16, y, C_DARK, F_SANS, label);
}

// A list view's column headers: raised buttons side by side.
void ui_header(int x, int y, int *widths, char **labels, int n) {
    int k;
    k = 0;
    while (k < n) {
        d_rect(x, y, widths[k], 18, C_FACE);
        d_bevel(x, y, widths[k], 18, B_RAISED);
        d_text(x + 5, y + 3, C_DARK, F_SANS, labels[k]);
        x = x + widths[k];
        ++k;
    }
}
