// ui.c -- the window kit's widgets, drawn through d_* (draw.c). docs/c4ix-desktop.md.
//
// Stateless: each call draws one thing, or answers whether a point hit
// it. The tools keep their own state (selection, scroll position, which
// field has the caret) and pass it in.

#include "winkit.h"

// ---- strings ----------------------------------------------------------------

int ui_len(char *s) { int n; n = 0; while (s[n]) ++n; return n; }
void ui_cpy(char *d, char *s) { while (*s) { *d = *s; ++d; ++s; } *d = 0; }
void ui_cat(char *d, char *s) { while (*d) ++d; ui_cpy(d, s); }
int ui_eq(char *a, char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
// Bounded copies, for every path and name kept in a fixed buffer: at
// most n - 1 bytes and a 0, and 1 if all of it fit. A caller refuses
// what did not fit, rather than keep it cut short. (d and s may be the
// same buffer.)
int ui_ncpy(char *d, char *s, int n) {
    int k;
    if (n <= 0) return 0;
    k = 0;
    while (s[k] && k < n - 1) { d[k] = s[k]; ++k; }
    d[k] = 0;
    return !s[k];
}
int ui_ncat(char *d, char *s, int n) {
    int k;
    k = ui_len(d);
    return ui_ncpy(d + k, s, n - k);
}
// dir and name joined by one '/', into out (which may be dir)
int ui_join(char *out, char *dir, char *name, int n) {
    int k;
    if (out != dir && !ui_ncpy(out, dir, n)) return 0;
    k = ui_len(out);
    if (k > 0 && out[k - 1] != '/') {
        if (k + 1 >= n) return 0;
        out[k] = '/'; out[k + 1] = 0;
    }
    return ui_ncat(out, name, n);
}
char *ui_base(char *p) {
    int k;
    k = ui_len(p);
    while (k > 0 && p[k - 1] != '/') --k;
    return p + k;
}

int ui_ends(char *s, char *suffix) {
    int n, m;
    n = ui_len(s); m = ui_len(suffix);
    if (m > n) return 0;
    return ui_eq(s + n - m, suffix);
}

char *ui_num(char *buf, int v) {
    char t[24];
    int n, neg, k;
    // digits from the remainders, kept positive: INT_MIN has no positive
    // twin to negate into
    neg = v < 0;
    n = 0;
    while (1) { k = v % 10; if (k < 0) k = 0 - k; t[n] = '0' + k; ++n; v = v / 10; if (!v) break; }
    k = 0;
    if (neg) { buf[k] = '-'; ++k; }
    while (n) { --n; buf[k] = t[n]; ++k; }
    buf[k] = 0;
    return buf;
}

char *ui_num_commas(char *buf, int v) {
    char t[32];
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
    if (ui_style == S_FLAT) {
        // a light face and an outline; the default's in the accent, two deep
        d_rect(x, y, w, h, pressed ? ui_mix(C_FACE, C_SHADOW, 1, 3) : C_LIGHT);
        d_bevel(x, y, w, h, B_RAISED);
        if (deflt) { d_recto(x, y, w - 1, h - 1, C_TITLE); d_recto(x + 1, y + 1, w - 3, h - 3, C_TITLE); }
        lw = ui_len(label) * 6;
        d_text(x + (w - lw) / 2, y + (h - 12) / 2, C_DARK, F_SANS, label);
        return;
    }
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
    if (ui_style != S_FLAT) {
        d_rect(x, y, 16, 16, C_FACE); d_bevel(x, y, 16, 16, B_RAISED);
        d_rect(x, y + h - 16, 16, 16, C_FACE); d_bevel(x, y + h - 16, 16, 16, B_RAISED);
    }
    d_line(x + 8, y + 5, x + 4, y + 9, C_DARK); d_line(x + 8, y + 5, x + 12, y + 9, C_DARK);
    d_rect(x + 4, y + 9, 9, 1, C_DARK);
    d_line(x + 4, y + h - 10, x + 8, y + h - 6, C_DARK); d_line(x + 12, y + h - 10, x + 8, y + h - 6, C_DARK);
    d_rect(x + 4, y + h - 11, 9, 1, C_DARK);
    track = h - 32;
    if (total <= visible || track < 8) return;
    th = (track * visible) / total;
    if (th < 10) th = 10;
    ty = y + 16 + ((track - th) * top) / (total - visible);
    if (ui_style == S_FLAT) { d_rect(x + 4, ty + 1, 8, th - 2, C_SHADOW); return; }
    d_rect(x, ty, 16, th, C_FACE);
    d_bevel(x, ty, 16, th, B_RAISED);
}

// A drag of a scroll bar's thumb. ui_scroll_click starts one when it is
// the thumb that is pressed; each move then asks ui_scroll_drag for the
// top line, naming the bar by where it is (a window may have two); the
// desktop ends it when the button comes up. The thumb keeps its place
// under the pointer: a pixel of travel is (total - visible) / (track -
// thumb) lines.
//
// A press on an arrow or on the track either side of the thumb is said
// to repeat (ui_scroll_repeat): the desktop presses it again while the
// button is held (wmcore.c), so a held arrow keeps scrolling, as a held
// track pages until the thumb reaches the pointer.
static int sd_on, sd_x, sd_y, sd_y0, sd_top0, sd_range, sd_span, sr_on;
static int sp_on, sp_x0, sp_pos0;     // a splitter's drag (below)

int ui_scroll_drag(int x, int y, int py) {
    int top;
    if (!sd_on || x != sd_x || y != sd_y) return 0 - 1;
    top = sd_top0;
    if (sd_span > 0) top = sd_top0 + ((py - sd_y0) * sd_range) / sd_span;
    if (top > sd_range) top = sd_range;
    if (top < 0) top = 0;
    return top;
}
int ui_scroll_active() { return sd_on || sp_on; }
int ui_scroll_repeat() { return sr_on; }
void ui_scroll_end() { sd_on = 0; sr_on = 0; sp_on = 0; }

// A bar across, for what is wider than its pane: the same parts turned
// on their side. LEFT is the first column shown of TOTAL, VISIBLE at once.
void ui_hscrollbar(int x, int y, int w, int total, int visible, int left) {
    int track, tw, tx;
    d_rect(x, y, w, 16, 0xe0e0e0);
    if (ui_style != S_FLAT) {
        d_rect(x, y, 16, 16, C_FACE); d_bevel(x, y, 16, 16, B_RAISED);
        d_rect(x + w - 16, y, 16, 16, C_FACE); d_bevel(x + w - 16, y, 16, 16, B_RAISED);
    }
    d_line(x + 5, y + 8, x + 9, y + 4, C_DARK); d_line(x + 5, y + 8, x + 9, y + 12, C_DARK);
    d_rect(x + 9, y + 4, 1, 9, C_DARK);
    d_line(x + w - 6, y + 8, x + w - 10, y + 4, C_DARK); d_line(x + w - 6, y + 8, x + w - 10, y + 12, C_DARK);
    d_rect(x + w - 11, y + 4, 1, 9, C_DARK);
    track = w - 32;
    if (total <= visible || track < 8) return;
    tw = (track * visible) / total;
    if (tw < 10) tw = 10;
    tx = x + 16 + ((track - tw) * left) / (total - visible);
    if (ui_style == S_FLAT) { d_rect(tx + 1, y + 4, tw - 2, 8, C_SHADOW); return; }
    d_rect(tx, y, tw, 16, C_FACE);
    d_bevel(tx, y, tw, 16, B_RAISED);
}

// The new first column for a click on a bar across, or -1 if it missed. A
// press on the thumb starts a drag, which ui_scroll_drag then follows with
// the pointer's x.
int ui_hscroll_click(int px, int py, int x, int y, int w, int total, int visible, int left) {
    int track, tw, tx, max;
    if (!ui_hit(px, py, x, y, w, 16)) return 0 - 1;
    max = total - visible;
    if (max < 0) max = 0;
    if (px < x + 16) { left = left - 1; sr_on = 1; }
    else if (px >= x + w - 16) { left = left + 1; sr_on = 1; }
    else {
        track = w - 32;
        if (total <= visible) return left;
        tw = (track * visible) / total;
        if (tw < 10) tw = 10;
        tx = x + 16 + ((track - tw) * left) / (total - visible);
        if (px < tx) { left = left - visible; sr_on = 1; }
        else if (px >= tx + tw) { left = left + visible; sr_on = 1; }
        else { sd_on = 1; sd_x = x; sd_y = y; sd_y0 = px; sd_top0 = left; sd_range = total - visible; sd_span = track - tw; }
    }
    if (left > max) left = max;
    if (left < 0) left = 0;
    return left;
}

// The new top line for a click on the scroll bar, or -1 if it missed.
int ui_scroll_click(int px, int py, int x, int y, int h, int total, int visible, int top) {
    int track, th, ty, max;
    if (!ui_hit(px, py, x, y, 16, h)) return 0 - 1;
    max = total - visible;
    if (max < 0) max = 0;
    if (py < y + 16) { top = top - 1; sr_on = 1; }
    else if (py >= y + h - 16) { top = top + 1; sr_on = 1; }
    else {
        track = h - 32;
        if (total <= visible) return top;
        th = (track * visible) / total;
        if (th < 10) th = 10;
        ty = y + 16 + ((track - th) * top) / (total - visible);
        if (py < ty) { top = top - visible; sr_on = 1; }
        else if (py >= ty + th) { top = top + visible; sr_on = 1; }
        else { sd_on = 1; sd_x = x; sd_y = y; sd_y0 = py; sd_top0 = top; sd_range = total - visible; sd_span = track - th; }
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

// Where a click at px puts the caret in a field drawn as above.
int ui_field_hit(int px, int x, int w, char *text, int caret) {
    int n, first, show, c;
    n = ui_len(text);
    show = (w - 10) / 7;
    first = 0;
    if (caret > show) first = caret - show;
    c = first + (px - x - 4) / 7;
    if (c > n) c = n;
    if (c < 0) c = 0;
    return c;
}

// Editing keys for a field's text; returns 1 if the text changed.
static int ctrl_letter(int code, int ch, int mods, int c) {
    return (mods & M_CTRL) && !(mods & M_ALT) && (ch == c || ch == c + 32 || code == c);
}
int ui_key_copy(int code, int ch, int mods) { return ctrl_letter(code, ch, mods, 'C') || ((mods & M_CTRL) && code == KEY_INS); }
int ui_key_cut(int code, int ch, int mods) { return ctrl_letter(code, ch, mods, 'X') || ((mods & M_SHIFT) && code == KEY_DEL); }
int ui_key_paste(int code, int ch, int mods) { return ctrl_letter(code, ch, mods, 'V') || ((mods & M_SHIFT) && code == KEY_INS); }

// A field has no selection: copy and cut take the whole of it, and a
// paste puts the clipboard's first line in at the caret, as much as fits.
int ui_field_key(char *text, int max, int *caret, int code, int ch, int mods) {
    int n, i, k, cn;
    char *c;
    n = ui_len(text);
    if (ui_key_copy(code, ch, mods) || ui_key_cut(code, ch, mods)) {
        wk_clip_set(text, n);
        if (!ui_key_cut(code, ch, mods) || !n) return 0;
        text[0] = 0; *caret = 0;
        return 1;
    }
    if (ui_key_paste(code, ch, mods)) {
        c = wk_clip_get(&cn);
        k = 0;
        while (k < cn && c[k] != 10 && c[k] != 13 && n < max) {
            if (c[k] >= 32 && c[k] <= 126) {
                i = n + 1;
                while (i > *caret) { text[i] = text[i - 1]; --i; }
                text[*caret] = c[k];
                *caret = *caret + 1;
                ++n;
            }
            ++k;
        }
        return k > 0;
    }
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
        if (ui_style == S_FLAT) {
            d_rect(tx, y, tw, 22, k == sel ? C_LIGHT : C_FACE);
            if (k == sel) d_rect(tx, y + 19, tw, 3, C_TITLE);
            d_text(tx + 8, y + 5, C_DARK, k == sel ? F_BOLD : F_SANS, labels[k]);
        } else if (k == sel) {
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
    d_disc(x + 6, y + 6, 6, ui_style == S_FLAT && on ? C_TITLE : C_SHADOW);
    d_disc(x + 6, y + 6, 5, C_WHITE);
    if (on) d_disc(x + 6, y + 6, ui_style == S_FLAT ? 3 : 2, ui_style == S_FLAT ? C_TITLE : C_DARK);
    d_text(x + 16, y, C_DARK, F_SANS, label);
}

// A list view's column headers: raised buttons side by side.
void ui_header(int x, int y, int *widths, char **labels, int n) {
    int k;
    k = 0;
    while (k < n) {
        d_rect(x, y, widths[k], 18, C_FACE);
        if (ui_style == S_FLAT) { d_rect(x, y + 17, widths[k], 1, C_SHADOW); d_rect(x + widths[k] - 1, y + 3, 1, 12, C_SHADOW); }
        else d_bevel(x, y, widths[k], 18, B_RAISED);
        d_text(x + 5, y + 3, C_DARK, F_SANS, labels[k]);
        x = x + widths[k];
        ++k;
    }
}

// A check box: a sunken white square, ticked when ON, its label beside it.
void ui_check(int x, int y, char *label, int on) {
    int k;
    d_rect(x, y, 13, 13, C_WHITE);
    if (ui_style == S_FLAT) d_recto(x, y, 13, 13, on ? C_TITLE : C_SHADOW);
    else d_bevel(x, y, 13, 13, B_SUNKEN);
    if (on) {
        k = 0;
        while (k < 7) { d_rect(x + 3 + k, y + (k < 2 ? 5 + k : 9 - k), 2, 2, ui_style == S_FLAT ? C_TITLE : C_DARK); ++k; }
    }
    d_text(x + 18, y, C_DARK, F_SANS, label);
}

// ---- a dropdown ------------------------------------------------------------------

void ui_drop(int x, int y, int w, struct ui_drop *d) {
    int k;
    d_rect(x, y, w, 22, C_WHITE);
    if (ui_style == S_FLAT) d_recto(x, y, w, 22, d->open ? C_TITLE : C_SHADOW);
    else d_bevel(x, y, w, 22, B_SUNKEN);
    if (d->sel >= 0 && d->sel < d->n) d_text(x + 5, y + 5, C_DARK, F_SANS, d->items[d->sel]);
    d_rect(x + w - 20, y + 2, 18, 18, C_FACE);
    d_bevel(x + w - 20, y + 2, 18, 18, d->open ? B_PRESSED : B_RAISED);
    k = 0;
    while (k < 4) { d_rect(x + w - 15 + k, y + 9 + k, 8 - 2 * k, 1, C_DARK); ++k; }
}
// the list's top: under the box, or over it
static int drop_top(int y, struct ui_drop *d) { return d->up ? y - d->n * 18 - 4 : y + 22; }
void ui_drop_list(int x, int y, int w, struct ui_drop *d) {
    int k, t;
    if (!d->open) return;
    t = drop_top(y, d);
    d_rect(x, t, w, d->n * 18 + 4, C_WHITE);
    d_recto(x, t, w, d->n * 18 + 4, C_DARK);
    k = 0;
    while (k < d->n) {
        if (k == d->hover) d_rect(x + 2, t + 2 + k * 18, w - 4, 18, C_SEL);
        d_text(x + 6, t + 5 + k * 18, k == d->hover ? C_WHITE : C_DARK, F_SANS, d->items[k]);
        ++k;
    }
}
int ui_drop_mouse(int ev, int px, int py, int x, int y, int w, struct ui_drop *d) {
    int k, t;
    if (d->open) {
        t = drop_top(y, d);
        if (ui_hit(px, py, x, t + 2, w, d->n * 18)) {
            k = (py - t - 2) / 18;
            if (k >= d->n) k = d->n - 1;
            d->hover = k;
            if (ev != E_DOWN && ev != E_UP) return 1;
            if (ev == E_UP && k == d->sel) return 1;    // the press that opened it, let go over the chosen
            d->open = 0;
            if (k != d->sel) { d->sel = k; return 2; }
            return 1;
        }
        if (ev == E_DOWN) { d->open = 0; return ui_hit(px, py, x, y, w, 22); }
        return ev == E_MOVE ? 0 : 1;
    }
    if (ev == E_DOWN && ui_hit(px, py, x, y, w, 22)) { d->open = 1; d->hover = d->sel; return 1; }
    return 0;
}
// the open list's mark, or the choice of a shut one, moved by D
static void k_drop_move(struct ui_drop *d, int dir) {
    if (d->open) {
        d->hover = d->hover + dir;
        if (d->hover < 0) d->hover = 0;
        if (d->hover >= d->n) d->hover = d->n - 1;
    } else {
        d->sel = d->sel + dir;
        if (d->sel < 0) d->sel = 0;
        if (d->sel >= d->n) d->sel = d->n - 1;
    }
}
int ui_drop_key(struct ui_drop *d, int code) {
    if (code == KEY_ESC && d->open) { d->open = 0; return 1; }
    if (code == KEY_ENTER) {
        if (!d->open) { d->open = 1; d->hover = d->sel; return 1; }
        d->open = 0;
        if (d->hover != d->sel && d->hover >= 0) { d->sel = d->hover; return 2; }
        return 1;
    }
    if (code == KEY_UP || code == KEY_DOWN) {
        k_drop_move(d, code == KEY_UP ? 0 - 1 : 1);
        if (!d->open) return 2;
        return 1;
    }
    return 0;
}

// ---- a field that checks what is typed ----------------------------------------------

void ui_field_bad(int x, int y, int w) {
    d_recto(x, y, w, 22, 0xd03030);
    d_recto(x + 1, y + 1, w - 2, 20, 0xd03030);
}
int ui_numfield_key(char *text, int max, int *caret, int code, int ch, int mods) {
    if (ch >= 32 && (ch < '0' || ch > '9') && !(mods & M_CTRL)) return 0;
    return ui_field_key(text, max, caret, code, ch, mods);
}
int ui_num_ok(char *text, int lo, int hi) {
    int v, k;
    if (!text[0]) return 0;
    v = 0; k = 0;
    while (text[k]) { if (text[k] < '0' || text[k] > '9' || k > 8) return 0; v = v * 10 + text[k] - '0'; ++k; }
    return v >= lo && v <= hi;
}

// ---- a splitter ------------------------------------------------------------------
//
// The bar between two panes side by side, dragged across to size them.
// ui_split_click starts a drag when the bar is pressed, from POS (what the
// window sizes by it, the left pane's width); each move then asks
// ui_split_drag what the pointer has made that, which the window keeps
// within its limits. The drag ends when the button comes up, as a thumb's
// does (ui_scroll_end), and follows the pointer out of the window
// meanwhile (ui_scroll_active).

void ui_splitter(int x, int y, int w, int h) {
    int k;
    d_rect(x, y, w, h, C_FACE);
    d_rect(x, y, 1, h, C_LIGHT);
    d_rect(x + w - 1, y, 1, h, C_SHADOW);
    // a grip in the middle, three dots down
    k = 0;
    while (k < 3 && h > 30) { d_rect(x + w / 2 - 1, y + h / 2 - 7 + k * 6, 2, 2, C_SHADOW); ++k; }
}
int ui_split_click(int px, int py, int x, int y, int w, int h, int pos) {
    if (!ui_hit(px, py, x, y, w, h)) return 0;
    sp_on = 1; sp_x0 = px; sp_pos0 = pos;
    return 1;
}
int ui_split_drag(int px) { return sp_on ? sp_pos0 + px - sp_x0 : 0 - 1; }
int ui_split_active() { return sp_on; }

// ---- a find bar --------------------------------------------------------------------
//
// Under an editor's text: what to find, Next and Previous, Match case, a
// note of what the last look found, and a box to close it. The editor
// keeps one (struct ui_find) and does the looking; the bar takes the
// keys while it has them (f->keys) and the clicks on it, and says what
// they asked for: UF_NEXT, UF_PREV, UF_TYPED (the text changed, to look
// again from where the match is), UF_CLOSE. Enter is Next, Shift+Enter
// Previous, F3 and Shift+F3 the same, Esc closes, Alt+C turns Match case
// over.

// where its parts are, for a bar W wide at (x, y)
static int fb_fx, fb_fw, fb_nx, fb_px, fb_cx, fb_tx, fb_xx;
static void fb_layout(int x, int w) {
    fb_fw = w < 620 ? w - 470 : 150;          // narrower, so the note keeps some room
    if (fb_fw < 80) fb_fw = 80;
    fb_fx = x + 40;
    fb_nx = fb_fx + fb_fw + 6;                 // Next
    fb_px = fb_nx + 50;                        // Previous
    fb_cx = fb_px + 70;                        // Match case
    fb_tx = fb_cx + 92;                        // the note
    fb_xx = x + w - 22;                        // the close box
}

void ui_find_open(struct ui_find *f, char *seed, int n) {
    int k;
    f->on = 1; f->keys = 1;
    if (seed && n > 0 && n < UI_FIND_MAX) {
        k = 0;
        while (k < n && seed[k] != 10 && seed[k] != 13) ++k;
        if (k == n) { memcpy(f->text, seed, n); f->text[n] = 0; }
    }
    f->caret = ui_len(f->text);
    f->note[0] = 0; f->bad = 0;
}

void ui_find_say(struct ui_find *f, char *note, int bad) {
    ui_ncpy(f->note, note, 64);
    f->bad = bad;
}

void ui_find_draw(struct ui_find *f, int x, int y, int w, int focused) {
    fb_layout(x, w);
    d_rect(x, y, w, UI_FIND_H, C_FACE);
    d_rect(x, y, w, 1, C_SHADOW);
    d_rect(x, y + 1, w, 1, C_LIGHT);
    d_text(x + 6, y + 8, C_DARK, F_SANS, "Find:");
    ui_field(fb_fx, y + 4, fb_fw, f->text, f->caret, focused && f->keys);
    ui_button(fb_nx, y + 4, 46, 22, "Next", 0, 0);
    ui_button(fb_px, y + 4, 66, 22, "Previous", 0, 0);
    ui_check(fb_cx, y + 9, "Match case", f->mcase);
    if (f->note[0] && fb_tx < fb_xx - 12) {
        d_clip(fb_tx, y + 2, fb_xx - 4 - fb_tx, UI_FIND_H - 2);
        d_text(fb_tx, y + 8, f->bad ? 0xc00000 : C_DARK, F_SANS, f->note);
        d_clip_end();
    }
    ui_button(fb_xx, y + 5, 18, 18, "x", 0, 0);
}

int ui_find_mouse(struct ui_find *f, int ev, int px, int py, int x, int y, int w) {
    if (!f->on || !ui_hit(px, py, x, y, w, UI_FIND_H)) return 0 - 1;
    if (ev != E_DOWN) return UF_NONE;
    fb_layout(x, w);
    f->keys = 1;
    if (ui_hit(px, py, fb_fx, y + 4, fb_fw, 22)) { f->caret = ui_field_hit(px, fb_fx, fb_fw, f->text, f->caret); return UF_NONE; }
    if (ui_hit(px, py, fb_nx, y + 4, 46, 22)) return UF_NEXT;
    if (ui_hit(px, py, fb_px, y + 4, 66, 22)) return UF_PREV;
    if (ui_hit(px, py, fb_cx, y + 6, 88, 18)) { f->mcase = !f->mcase; return UF_NONE; }
    if (ui_hit(px, py, fb_xx, y + 5, 18, 18)) { f->on = 0; f->keys = 0; return UF_CLOSE; }
    return UF_NONE;
}

int ui_find_key(struct ui_find *f, int code, int ch, int mods) {
    if (code == KEY_ESC) { f->on = 0; f->keys = 0; return UF_CLOSE; }
    if (code == KEY_ENTER || code == KEY_F3) return (mods & M_SHIFT) ? UF_PREV : UF_NEXT;
    if ((mods & M_ALT) && (ch == 'c' || ch == 'C' || code == 'C')) { f->mcase = !f->mcase; return UF_NONE; }
    if (ui_field_key(f->text, UI_FIND_MAX - 1, &f->caret, code, ch, mods)) { f->note[0] = 0; f->bad = 0; return UF_TYPED; }
    return UF_NONE;
}

// Does what F looks for start at S, which has N characters to look at?
int ui_find_at(struct ui_find *f, char *s, int n) {
    int m, k, a, b;
    m = ui_len(f->text);
    if (!m || m > n) return 0;
    k = 0;
    while (k < m) {
        a = s[k] & 255; b = f->text[k] & 255;
        if (a != b) {
            if (f->mcase) return 0;
            if (a >= 'A' && a <= 'Z') a = a + 32;
            if (b >= 'A' && b <= 'Z') b = b + 32;
            if (a != b) return 0;
        }
        ++k;
    }
    return 1;
}
