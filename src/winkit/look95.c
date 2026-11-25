// look95.c -- window chrome: the Flat and Classic looks. docs/grin-plan.md 3.5.
//
// Classic is Windows 95's: a raised frame, a title bar shading from one
// colour to another, the window's icon and title on the left, and raised
// minimise, maximise and close buttons on the right. Flat keeps the
// layout and draws it with outlines and solid colours. Both draw the
// kit's file box as Windows 95's Open and Save As (below).

#include "wkwin.h"
#include "wkgui.h"

void wk_gradient(int x, int y, int w, int h, int a, int b) {
    int i, n, bw;
    if (wk.style != S_CLASSIC) { gui_rect(x, y, w, h, a); return; }
    n = 16; bw = (w + n - 1) / n;
    i = 0;
    while (i < n) {
        if (i * bw < w) gui_rect(x + i * bw, y, (i + 1) * bw > w ? w - i * bw : bw, h, ui_mix(a, b, i, n - 1));
        ++i;
    }
}

// white on a dark bar, the scheme's dark on a light one
int wk_ink(int bg) {
    return (((bg >> 16) & 255) + ((bg >> 8) & 255) + (bg & 255)) < 384 ? C_WHITE : 0x404850;
}

enum { CAP_CLOSE = 0, CAP_MAX = 1, CAP_RESTORE = 2, CAP_MIN = 3 };
static int cap_ink, cap_bg;           // flat: the glyph's colour and the bar's under it

static void cap_button(int x, int y, int what) {
    int k;
    k = C_DARK;
    if (wk.style == S_FLAT) k = cap_ink;
    else { gui_rect(x, y, 16, 14, C_FACE); d_bevel(x, y, 16, 14, B_RAISED); }
    if (what == CAP_CLOSE) {                       // an X
        gui_line(x + 4, y + 3, x + 10, y + 9, k); gui_line(x + 5, y + 3, x + 11, y + 9, k);
        gui_line(x + 10, y + 3, x + 4, y + 9, k); gui_line(x + 11, y + 3, x + 5, y + 9, k);
    } else if (what == CAP_MAX) {
        gui_recto(x + 3, y + 2, 9, 9, k); gui_rect(x + 3, y + 3, 9, 1, k);
    } else if (what == CAP_RESTORE) {
        gui_recto(x + 5, y + 2, 7, 6, k); gui_rect(x + 5, y + 3, 7, 1, k);
        gui_rect(x + 3, y + 5, 7, 6, wk.style == S_FLAT ? cap_bg : C_FACE);
        gui_recto(x + 3, y + 5, 7, 6, k); gui_rect(x + 3, y + 6, 7, 1, k);
    } else gui_rect(x + 4, y + 9, 6, 2, k);        // minimise
}

static void frame95(struct win *w, int focused) {
    int tx, bx, by, ink;
    gui_rect(w->x, w->y, w->w, w->h, C_FACE);
    if (wk.style == S_FLAT) {
        cap_bg = focused ? C_TITLE : C_ITITLE;
        gui_rect(w->x, w->y, w->w, BORDER + TITLE_H, cap_bg);
        gui_recto(w->x, w->y, w->w - 1, w->h - 1, focused ? C_TITLE : C_SHADOW);
        ink = wk_ink(cap_bg);
        cap_ink = ink;
    } else {
        d_bevel(w->x, w->y, w->w, w->h, B_RAISED);
        if (focused) wk_gradient(w->x + BORDER, w->y + BORDER, w->w - 2 * BORDER, TITLE_H, C_TITLE, C_TITLE2);
        else wk_gradient(w->x + BORDER, w->y + BORDER, w->w - 2 * BORDER, TITLE_H, C_ITITLE, C_ITITLE2);
        ink = focused ? C_LIGHT : C_FACE;
    }
    tx = w->x + BORDER + 3;
    if (!w->dialog) { d_icon(w->kind, tx, w->y + BORDER + 1); tx = tx + 20; }
    wk_clip_screen(w->x + BORDER, w->y + BORDER, w->w - 2 * BORDER - 56, TITLE_H);
    d_text(tx, w->y + BORDER + 3, ink, F_BOLD, w->title);
    wk_noclip();
    by = w->y + BORDER + 2;
    bx = w->x + w->w - BORDER - 2 - 16;
    cap_button(bx, by, CAP_CLOSE);
    if (!w->dialog) {
        cap_button(bx - 18, by, w->max ? CAP_RESTORE : CAP_MAX);
        cap_button(bx - 34, by, CAP_MIN);
    }
}

static void menubar95(struct win *w, int focused, int open, int hot) {
    int k, x, y, lw;
    y = w->y + BORDER + TITLE_H + 1;
    gui_rect(wk_client_x(w), y, wk_client_w(w), MENU_H, C_FACE);
    x = wk_client_x(w) + 4;
    k = 0;
    while (k < w->nmenus) {
        lw = wk_menu_w(w, k);
        if (open == k) gui_rect(x, y + 1, lw, MENU_H - 2, C_SEL);
        d_text(x + 6, y + 4, open == k ? C_WHITE : C_DARK, F_SANS, w->menus[k].title);
        x = x + lw;
        ++k;
    }
}

// the size grip, over the client area's corner
static void grip95(struct win *w) {
    int bx, by;
    if (!w->resizable || w->max) return;
    bx = w->x + w->w - BORDER - 12; by = w->y + w->h - BORDER - 12;
    gui_line(bx + 3, by + 11, bx + 11, by + 3, C_LIGHT); gui_line(bx + 4, by + 11, bx + 11, by + 4, C_SHADOW);
    gui_line(bx + 7, by + 11, bx + 11, by + 7, C_LIGHT); gui_line(bx + 8, by + 11, bx + 11, by + 8, C_SHADOW);
}

static int hit95(struct win *w, int x, int y) {
    int bx, by;
    by = w->y + BORDER + 2;
    bx = w->x + w->w - BORDER - 2 - 16;
    if (ui_hit(x, y, bx, by, 16, 14)) return HT_CLOSE;
    if (!w->dialog && ui_hit(x, y, bx - 18, by, 16, 14)) return HT_MAX;
    if (!w->dialog && ui_hit(x, y, bx - 34, by, 16, 14)) return HT_MIN;
    if (y < w->y + BORDER + TITLE_H) return HT_CAPTION;
    if (w->resizable && !w->max && x >= w->x + w->w - 16 && y >= w->y + w->h - 16) return HT_SIZE + SZ_R + SZ_B;
    if (w->nmenus && y < wk_client_y(w)) return HT_MENUBAR;
    if (ui_hit(x, y, wk_client_x(w), wk_client_y(w), wk_client_w(w), wk_client_h(w))) return HT_CLIENT;
    return HT_NONE;
}

struct wk_look look_flat = { .frame = frame95, .menubar = menubar95, .hit = hit95, .grip = grip95 };
struct wk_look look_classic = { .frame = frame95, .menubar = menubar95, .hit = hit95, .grip = grip95 };

// ---- the file box, as Windows 95 drew it ---------------------------------------------
//
// dialog.c's file box (struct fdlg, wk_fb_*) laid out as Windows 95's
// Open and Save As: "Look in" with the folder shown, a list down from it
// of the folders above it to go back to, and a button up one; the
// folder's contents, folders first, each with its icon; the name, and
// "Files of type" with the types the box offers; Open or Save, and Cancel.
// A click picks an entry, a double click opens it; Up and Down walk the
// list, Backspace on an empty name goes up a folder.

enum { FB_LX = 10, FB_LY = 40, FB_FX = 100, FB_DROP_ROW = 16 };

static int fb_lw(int cw) { return cw - 2 * FB_LX; }
static int fb_lh(int ch) { return ch - FB_LY - 76; }
static int fb_rows(int ch) { return (fb_lh(ch) - 4) / FB_ROW; }
static int fb_cw(int cw) { return cw - FB_FX - 96; }        // the name field's and the type list's width
static int fb_look_w(int cw) { return cw - 78 - 40; }

// the folders above the one shown, and it: how many, and the k'th's path and depth
static int fb_depth(struct fdlg *f) {
    int k, n;
    n = 1; k = 1;
    while (f->dir[k]) { if (f->dir[k] == '/' || !f->dir[k + 1]) ++n; ++k; }
    return f->dir[1] ? n : 1;
}
static void fb_ancestor(struct fdlg *f, int k, char *out) {
    int i, seen;
    if (k == 0) { out[0] = '/'; out[1] = 0; return; }
    if (k >= fb_depth(f) - 1) { ui_ncpy(out, f->dir, WK_PATH); return; }
    i = 1; seen = 0;
    while (f->dir[i]) {
        if (f->dir[i] == '/') { ++seen; if (seen == k) break; }
        ++i;
    }
    ui_ncpy(out, f->dir, i + 1 < WK_PATH ? i + 1 : WK_PATH);
}

static char fbtmp[WK_PATH];

static void fb_folder_icon(int x, int y) {
    d_rect(x + 1, y + 3, 6, 2, 0xc8a000);
    d_rect(x + 1, y + 5, 14, 9, 0xffd84a);
    d_recto(x + 1, y + 5, 13, 8, 0xc8a000);
}
static void fb_file_icon(int x, int y) {
    d_rect(x + 3, y + 1, 10, 14, C_WHITE);
    d_recto(x + 3, y + 1, 9, 13, C_SHADOW);
    d_rect(x + 5, y + 5, 6, 1, C_SHADOW); d_rect(x + 5, y + 8, 6, 1, C_SHADOW); d_rect(x + 5, y + 11, 4, 1, C_SHADOW);
}

// a drop-down list's box: what is chosen, and the arrow that opens the list
static void fb_combo(int x, int y, int w, char *text, int folder) {
    int bx;
    d_rect(x, y, w, 22, C_WHITE);
    d_bevel(x, y, w, 22, B_SUNKEN);
    if (folder) fb_folder_icon(x + 4, y + 3);
    d_clip(x + 2, y + 2, w - 22, 18);
    d_text(x + (folder ? 24 : 6), y + 5, C_DARK, F_SANS, text);
    d_clip_end();
    bx = x + w - 19;
    d_rect(bx, y + 2, 17, 18, C_FACE);
    d_bevel(bx, y + 2, 17, 18, B_RAISED);
    d_line(bx + 4, y + 9, bx + 12, y + 9, C_DARK); d_line(bx + 5, y + 10, bx + 11, y + 10, C_DARK);
    d_line(bx + 6, y + 11, bx + 10, y + 11, C_DARK); d_line(bx + 7, y + 12, bx + 9, y + 12, C_DARK);
    d_rect(bx + 8, y + 13, 1, 1, C_DARK);
}

// the name a folder shows: its last part (a / after it or not), C4IX (/) for the top
static char fbname[WK_NAME];
static char *fb_folder_name(char *path) {
    int k, e;
    e = ui_len(path);
    while (e > 1 && path[e - 1] == '/') --e;
    if (e <= 1) return "C4IX (/)";
    k = e;
    while (k > 0 && path[k - 1] != '/') --k;
    ui_ncpy(fbname, path + k, e - k + 1 < WK_NAME ? e - k + 1 : WK_NAME);
    return fbname;
}

static void file95_draw(int w, int cw, int ch, int focused) {
    struct fdlg *f;
    int k, y, lw, lh, rows, x, n, ty, dy;
    f = (struct fdlg *)wk_state(w);
    d_rect(0, 0, cw, ch, C_FACE);
    // Look in: the folder, and Up
    d_text(FB_LX, 14, C_DARK, F_SANS, f->save ? "Save in:" : "Look in:");
    fb_combo(78, 10, fb_look_w(cw), fb_folder_name(f->dir), 1);
    ui_button(cw - 34, 10, 24, 22, "", 0, 0);
    x = cw - 34;
    fb_folder_icon(x + 3, 13);
    d_line(x + 16, 14, x + 16, 22, C_DARK); d_line(x + 13, 17, x + 16, 14, C_DARK); d_line(x + 19, 17, x + 16, 14, C_DARK);
    d_line(x + 16, 22, x + 20, 22, C_DARK);
    // the folder's contents
    lw = fb_lw(cw); lh = fb_lh(ch); rows = fb_rows(ch);
    d_rect(FB_LX, FB_LY, lw, lh, C_WHITE);
    d_bevel(FB_LX, FB_LY, lw, lh, B_SUNKEN);
    d_clip(FB_LX + 2, FB_LY + 2, lw - 20, lh - 4);
    k = f->top; y = FB_LY + 2;
    while (k < f->n && k < f->top + rows) {
        if (f->isdir[f->order[k]]) fb_folder_icon(FB_LX + 4, y);
        else fb_file_icon(FB_LX + 4, y);
        n = ui_len(wk_fb_name(f, k));
        if (k == f->sel) d_rect(FB_LX + 22, y, n * 6 + 6, FB_ROW, C_SEL);
        d_text(FB_LX + 25, y + 2, k == f->sel ? C_WHITE : C_DARK, F_SANS, wk_fb_name(f, k));
        y = y + FB_ROW;
        ++k;
    }
    d_clip_end();
    ui_scrollbar(FB_LX + lw - 18, FB_LY + 2, lh - 4, f->n, rows, f->top);
    // the name, the type, and the buttons
    y = ch - 66;
    d_text(FB_LX, y + 5, C_DARK, F_SANS, "File name:");
    ui_field(FB_FX, y, fb_cw(cw), f->out, f->caret, !f->drop);
    ui_button(cw - 88, y, 78, 24, f->save ? "Save" : "Open", 0, 1);
    ty = ch - 34;
    d_text(FB_LX, ty + 5, C_DARK, F_SANS, f->save ? "Save as type:" : "Files of type:");
    wk_fb_label(f, f->ftype, fbtmp, WK_PATH);
    fb_combo(FB_FX, ty, fb_cw(cw), fbtmp, 0);
    ui_button(cw - 88, ty, 78, 24, "Cancel", 0, 0);
    // a list dropped down, over the rest
    if (f->drop == 1) {
        n = fb_depth(f);
        lw = fb_look_w(cw);
        d_rect(78, 32, lw, n * FB_DROP_ROW + 4, C_WHITE);
        d_recto(78, 32, lw - 1, n * FB_DROP_ROW + 3, C_DARK);
        k = 0;
        while (k < n) {
            dy = 34 + k * FB_DROP_ROW;
            fb_ancestor(f, k, fbtmp);
            if (k == n - 1) d_rect(80 + k * 10 + 20, dy, ui_len(fb_folder_name(fbtmp)) * 6 + 6, FB_DROP_ROW, C_SEL);
            fb_folder_icon(82 + k * 10, dy);
            d_text(80 + k * 10 + 23, dy + 2, k == n - 1 ? C_WHITE : C_DARK, F_SANS, fb_folder_name(fbtmp));
            ++k;
        }
    } else if (f->drop == 2) {
        n = wk_fb_types(f);
        lw = fb_cw(cw);
        dy = ty - n * FB_DROP_ROW - 4;               // up from the box: there is no room under it
        d_rect(FB_FX, dy, lw, n * FB_DROP_ROW + 4, C_WHITE);
        d_recto(FB_FX, dy, lw - 1, n * FB_DROP_ROW + 3, C_DARK);
        k = 0;
        while (k < n) {
            if (k == f->ftype) d_rect(FB_FX + 2, dy + 2 + k * FB_DROP_ROW, lw - 4, FB_DROP_ROW, C_SEL);
            wk_fb_label(f, k, fbtmp, WK_PATH);
            d_text(FB_FX + 6, dy + 4 + k * FB_DROP_ROW, k == f->ftype ? C_WHITE : C_DARK, F_SANS, fbtmp);
            ++k;
        }
    }
}

// the entry picked: a file's name goes into the name
static void fb_pick(struct fdlg *f, int k, int rows) {
    if (k < 0 || k >= f->n) return;
    f->sel = k;
    if (k < f->top) f->top = k;
    if (k >= f->top + rows) f->top = k - rows + 1;
    if (!f->isdir[f->order[k]]) { ui_ncpy(f->out, wk_fb_name(f, k), WK_NAME); f->caret = ui_len(f->out); }
}

static void file95_mouse(int w, int ev, int x, int y, int b) {
    struct fdlg *f;
    struct win *d;
    int cw, ch, k, t, lw, rows, n, dy;
    if (!(d = win_get(w))) return;
    f = (struct fdlg *)d->st;
    wk_size(w, &cw, &ch);
    lw = fb_lw(cw); rows = fb_rows(ch);
    if (ev == E_WHEEL) {
        f->top = f->top + b * 3;
        if (f->top > f->n - rows) f->top = f->n - rows;
        if (f->top < 0) f->top = 0;
        return;
    }
    if (ev == E_MOVE) { t = ui_scroll_drag(FB_LX + lw - 18, FB_LY + 2, y); if (t >= 0) f->top = t; return; }
    if (ev != E_DOWN && ev != E_DBL) return;
    // a list dropped down takes the click, and closes
    if (f->drop) {
        if (ev != E_DOWN) return;
        if (f->drop == 1) {
            n = fb_depth(f);
            if (ui_hit(x, y, 78, 34, fb_look_w(cw), n * FB_DROP_ROW)) {
                k = (y - 34) / FB_DROP_ROW;
                fb_ancestor(f, k, fbtmp);
                ui_ncpy(f->dir, fbtmp, WK_PATH);
                wk_fb_list(f);
            }
        } else {
            n = wk_fb_types(f);
            dy = ch - 34 - n * FB_DROP_ROW - 4;
            if (ui_hit(x, y, FB_FX, dy + 2, fb_cw(cw), n * FB_DROP_ROW)) wk_fb_type(f, (y - dy - 2) / FB_DROP_ROW);
        }
        f->drop = 0;
        wk_redraw_all();
        return;
    }
    if (ev == E_DOWN && ui_hit(x, y, cw - 88, ch - 66, 78, 24)) { wk_fb_ok(d, f); return; }
    if (ev == E_DOWN && ui_hit(x, y, cw - 88, ch - 34, 78, 24)) { wk_dialog_answer(d, 0); return; }
    if (ev == E_DOWN && ui_hit(x, y, cw - 34, 10, 24, 22)) { if (f->dir[1]) wk_fb_enter(f, ".."); return; }
    if (ev == E_DOWN && ui_hit(x, y, 78, 10, fb_look_w(cw), 22)) { f->drop = 1; wk_redraw_all(); return; }
    if (ev == E_DOWN && ui_hit(x, y, FB_FX, ch - 34, fb_cw(cw), 22)) { f->drop = 2; wk_redraw_all(); return; }
    t = ui_scroll_click(x, y, FB_LX + lw - 18, FB_LY + 2, fb_lh(ch) - 4, f->n, rows, f->top);
    if (t >= 0) { f->top = t; return; }
    if (ui_hit(x, y, FB_LX + 2, FB_LY + 2, lw - 20, fb_lh(ch) - 4)) {
        k = f->top + (y - FB_LY - 2) / FB_ROW;
        if (k >= f->n) { f->sel = 0 - 1; return; }
        fb_pick(f, k, rows);
        if (ev == E_DBL) {
            if (f->isdir[f->order[k]]) { ui_ncpy(fbtmp, wk_fb_name(f, k), WK_PATH); wk_fb_enter(f, fbtmp); return; }
            wk_fb_ok(d, f);
        }
        return;
    }
    if (ui_hit(x, y, FB_FX, ch - 66, fb_cw(cw), 22)) f->caret = ui_field_hit(x, FB_FX, fb_cw(cw), f->out, f->caret);
}

static void file95_key(int w, int down, int code, int ch, int mods) {
    struct fdlg *f;
    struct win *d;
    int cw, chh;
    if (!down || !(d = win_get(w))) return;
    f = (struct fdlg *)d->st;
    wk_size(w, &cw, &chh);
    if (code == KEY_ESC) { if (f->drop) { f->drop = 0; return; } wk_dialog_answer(d, 0); return; }
    f->drop = 0;
    if (code == KEY_ENTER) { wk_fb_ok(d, f); return; }
    if (code == KEY_BACK && !f->out[0]) { if (f->dir[1]) wk_fb_enter(f, ".."); return; }
    if (code == KEY_DOWN) { fb_pick(f, f->sel < 0 ? 0 : f->sel + 1, fb_rows(chh)); return; }
    if (code == KEY_UP) { fb_pick(f, f->sel > 0 ? f->sel - 1 : 0, fb_rows(chh)); return; }
    ui_field_key(f->out, WK_NAME - 1, &f->caret, code, ch, mods);
}

struct wk_class wk_file95_class = { .kind = ICON_FILEBOX, .draw = file95_draw, .mouse = file95_mouse, .key = file95_key };
