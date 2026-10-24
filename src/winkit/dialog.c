// dialog.c -- the window kit's dialogs: message, input and file boxes.
// docs/grin-plan.md 3.4.
//
// Each is a window of its own class, modal while it is on top. Its state
// is its own -- the text being typed, the message, the folder listed --
// so two dialogs open at once never share anything (the desktop review's
// B7 and B10). The answer goes to the window that asked, through its
// class's answer hook, or to the shell's when no window did; an input or
// file box copies what was typed into the asker's buffer first, if the
// asker is still open.

#include "wkwin.h"

enum { DLG_LINES = 16, DLG_TEXT = 512, LINE_CHARS = 82 };

// a message or input box; the file box's state begins the same way
struct dlg {
    char *buf;                    // where an answer's text goes (input, file)
    int max;                      // ... and its size
    char out[WK_PATH];            // the text: typed (input), or the path chosen (file)
    int caret;
    int buttons;                  // a message box's: WK_OK or WK_YESNO
    int nlines, ls[DLG_LINES], ll[DLG_LINES];   // a long message, wrapped
    char text[DLG_TEXT];          // the message, or the input box's prompt
};

void wk_dialog_answer(struct win *d, int yes) {
    struct dlg *g;
    int owner, dialog;
    g = (struct dlg *)d->st;
    owner = d->owner; dialog = d->action;
    if (yes && g->buf && g->max > 0 && (owner <= 0 || win_get(owner))) ui_ncpy(g->buf, g->out, g->max);
    wk_close(d->id);
    wk_answer_owner(owner, dialog, yes);
}

static struct win *dialog_open(struct wk_class *c, int size, int owner, int dialog, char *title, int w, int h) {
    int i;
    struct win *d;
    c->size = size;
    i = wk_open(c, title, w, h, WK_DIALOG | WK_FIXED | WK_CENTER, 0);
    if (i < 0 || !(d = win_get(i))) return 0;
    d->owner = owner > 0 ? owner : 0;
    d->action = dialog;
    return d;
}

static void buttons_draw(int cw, int ch, char *a, char *b) {
    ui_button(cw - 176, ch - 34, 76, 24, a, 0, 1);
    ui_button(cw - 88, ch - 34, 76, 24, b, 0, 0);
}
static int buttons_hit(int x, int y, int cw, int ch) {
    if (ui_hit(x, y, cw - 176, ch - 34, 76, 24)) return 1;
    if (ui_hit(x, y, cw - 88, ch - 34, 76, 24)) return 0;
    return 0 - 1;
}

// ---- message boxes ---------------------------------------------------------------
//
// One line as long as it fits the widest box; a longer message is broken
// at spaces into lines, and the box grows to hold them.

static void msg_wrap(struct dlg *g) {
    int n, s, k, brk;
    n = ui_len(g->text);
    g->nlines = 0;
    s = 0;
    while (s < n && g->nlines < DLG_LINES) {
        k = s; brk = 0 - 1;
        while (k < n && k - s < LINE_CHARS) { if (g->text[k] == ' ') brk = k; ++k; }
        if (k < n && brk > s) k = brk;
        g->ls[g->nlines] = s; g->ll[g->nlines] = k - s;
        ++g->nlines;
        s = k;
        while (s < n && g->text[s] == ' ') ++s;
    }
}

static void msg_draw(int w, int cw, int ch, int focused) {
    struct dlg *g;
    int bx, k;
    g = (struct dlg *)wk_state(w);
    d_rect(0, 0, cw, ch, C_FACE);
    d_disc(30, 30, 14, g->buttons ? C_TITLE : 0xd02020);
    d_textn(26, 20, C_WHITE, 18, F_BOLD, 0, g->buttons ? "?" : "i", 1);
    if (g->nlines <= 1) d_text(56, 24, C_DARK, F_SANS, g->text);
    else {
        k = 0;
        while (k < g->nlines) { d_textn(56, 24 + k * 16, C_DARK, 11, F_SANS, 0, g->text + g->ls[k], g->ll[k]); ++k; }
    }
    if (g->buttons) {
        bx = cw / 2 - 84;
        ui_button(bx, ch - 34, 76, 24, g->buttons == WK_OKCANCEL ? "OK" : "Yes", 0, 1);
        ui_button(bx + 92, ch - 34, 76, 24, g->buttons == WK_OKCANCEL ? "Cancel" : "No", 0, 0);
    } else ui_button(cw / 2 - 38, ch - 34, 76, 24, "OK", 0, 1);
}

static void msg_mouse(int w, int ev, int x, int y, int b) {
    struct dlg *g;
    struct win *d;
    int bx, cw, ch;
    if (ev != E_DOWN || !(d = win_get(w))) return;
    g = (struct dlg *)d->st;
    wk_size(w, &cw, &ch);
    if (g->buttons) {
        bx = cw / 2 - 84;
        if (ui_hit(x, y, bx, ch - 34, 76, 24)) wk_dialog_answer(d, 1);
        else if (ui_hit(x, y, bx + 92, ch - 34, 76, 24)) wk_dialog_answer(d, 0);
    } else if (ui_hit(x, y, cw / 2 - 38, ch - 34, 76, 24)) wk_dialog_answer(d, 1);
}

static void msg_key(int w, int down, int code, int ch, int mods) {
    struct dlg *g;
    struct win *d;
    if (!down || !(d = win_get(w))) return;
    g = (struct dlg *)d->st;
    if (code == KEY_ESC) { wk_dialog_answer(d, 0); return; }
    if (code == KEY_ENTER) { wk_dialog_answer(d, 1); return; }
    if (g->buttons == WK_YESNO && (ch == 'y' || ch == 'Y')) wk_dialog_answer(d, 1);
    else if (g->buttons == WK_YESNO && (ch == 'n' || ch == 'N')) wk_dialog_answer(d, 0);
}

struct wk_class wk_msg_class = { .kind = ICON_MSG, .draw = msg_draw, .mouse = msg_mouse, .key = msg_key };

void wk_msgbox(int owner, int dialog, char *title, char *text, int buttons) {
    struct win *d;
    struct dlg *g;
    int tw;
    tw = ui_len(text) * 6 + 40;
    if (tw < 260) tw = 260;
    if (tw > 560) tw = 560;
    if (!(d = dialog_open(&wk_msg_class, sizeof(struct dlg), owner, dialog, title, tw, 120))) return;
    g = (struct dlg *)d->st;
    g->buttons = buttons;
    ui_ncpy(g->text, text, DLG_TEXT);
    msg_wrap(g);
    if (g->nlines > 1) { d->h = 120 + 16 * (g->nlines - 1); d->y = (wk.sh - d->h) / 2 - 40; }
}

// ---- input boxes -------------------------------------------------------------------

static void input_draw(int w, int cw, int ch, int focused) {
    struct dlg *g;
    g = (struct dlg *)wk_state(w);
    d_rect(0, 0, cw, ch, C_FACE);
    d_text(12, 12, C_DARK, F_SANS, g->text);
    ui_field(12, 30, cw - 24, g->out, g->caret, 1);
    buttons_draw(cw, ch, "OK", "Cancel");
}

static void input_mouse(int w, int ev, int x, int y, int b) {
    struct dlg *g;
    struct win *d;
    int cw, ch, k;
    if (ev != E_DOWN || !(d = win_get(w))) return;
    g = (struct dlg *)d->st;
    wk_size(w, &cw, &ch);
    if ((k = buttons_hit(x, y, cw, ch)) >= 0) { wk_dialog_answer(d, k); return; }
    if (ui_hit(x, y, 12, 30, cw - 24, 22)) g->caret = ui_field_hit(x, 12, cw - 24, g->out, g->caret);
}

static void input_key(int w, int down, int code, int ch, int mods) {
    struct dlg *g;
    struct win *d;
    int max;
    if (!down || !(d = win_get(w))) return;
    g = (struct dlg *)d->st;
    if (code == KEY_ESC) { wk_dialog_answer(d, 0); return; }
    if (code == KEY_ENTER) { wk_dialog_answer(d, 1); return; }
    max = g->max > 0 && g->max <= WK_PATH ? g->max - 1 : WK_PATH - 1;
    ui_field_key(g->out, max, &g->caret, code, ch, mods);
}

struct wk_class wk_input_class = { .kind = ICON_INPUT, .draw = input_draw, .mouse = input_mouse, .key = input_key };

void wk_inputbox(int owner, int dialog, char *title, char *prompt, char *buf, int max) {
    struct win *d;
    struct dlg *g;
    if (!(d = dialog_open(&wk_input_class, sizeof(struct dlg), owner, dialog, title, 380, 130))) return;
    g = (struct dlg *)d->st;
    g->buf = buf; g->max = max;
    ui_ncpy(g->text, prompt, DLG_TEXT);
    if (buf) ui_ncpy(g->out, buf, max > 0 && max <= WK_PATH ? max : WK_PATH);
    g->caret = ui_len(g->out);
}

// ---- file boxes ----------------------------------------------------------------------
//
// The folder's contents, folders first, then a name. A double click on a
// folder (or "..") goes into it; on a file, or OK with a name, answers
// with the folder and the name joined.

enum { FB_MAX = 256, FB_ROWS = 12, FB_ROW = 16 };

struct fdlg {
    char *buf;                    // as struct dlg's
    int max;
    char out[WK_PATH];
    int caret;
    int save;
    int n, top, sel;
    char dir[WK_PATH];
    char pattern[WK_NAME];
    int isdir[FB_MAX];
    int order[FB_MAX];
    char names[FB_MAX * WK_NAME];
};

static char *fname(struct fdlg *f, int k) { return f->names + f->order[k] * WK_NAME; }

static int fb_wanted(struct fdlg *f, char *name, int dir) {
    if (dir || !f->pattern[0] || ui_eq(f->pattern, "*")) return 1;
    if (f->pattern[0] == '*') return ui_ends(name, f->pattern + 1);
    return ui_eq(name, f->pattern);
}

static int fb_before(struct fdlg *f, int a, int b) {
    char *p, *q;
    if (f->isdir[a] != f->isdir[b]) return f->isdir[a];
    p = f->names + a * WK_NAME; q = f->names + b * WK_NAME;
    while (*p && *p == *q) { ++p; ++q; }
    return (*p & 255) < (*q & 255);
}

static void fb_list(struct fdlg *f) {
    int i, k, j, r;
    char name[WK_NAME];
    f->n = 0; f->top = 0; f->sel = 0 - 1;
    if (f->dir[0] && !ui_eq(f->dir, "/")) {
        ui_cpy(f->names, ".."); f->isdir[0] = 1; f->order[0] = 0; f->n = 1;
    }
    i = 0;
    while (f->n < FB_MAX && (r = wk_readdir(f->dir, i, name)) >= 0) {
        ++i;
        name[WK_NAME - 1] = 0;
        if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) continue;
        if (!fb_wanted(f, name, r)) continue;
        ui_ncpy(f->names + f->n * WK_NAME, name, WK_NAME);
        f->isdir[f->n] = r;
        // into order, folders first and by name (".." stays first)
        k = f->n;
        j = f->n && ui_eq(f->names, "..") ? 1 : 0;
        while (k > j && fb_before(f, f->n, f->order[k - 1])) { f->order[k] = f->order[k - 1]; --k; }
        f->order[k] = f->n;
        ++f->n;
    }
}

static void fb_enter(struct fdlg *f, char *name) {
    int k;
    if (ui_eq(name, "..")) {
        k = ui_len(f->dir);
        while (k > 1 && f->dir[k - 1] == '/') --k;
        while (k > 0 && f->dir[k - 1] != '/') --k;
        if (k > 1) --k;
        if (k < 1) k = 1;
        f->dir[k] = 0;
    } else if (!ui_join(f->dir, f->dir, name, WK_PATH)) return;
    fb_list(f);
    wk_redraw_all();
}

// OK, or a double click on a file: the folder and the name joined
static void fb_ok(struct win *d, struct fdlg *f) {
    int k;
    char name[WK_NAME];
    k = 0;
    while (k < f->n) { if (f->isdir[f->order[k]] && ui_eq(fname(f, k), f->out)) { fb_enter(f, f->out); f->out[0] = 0; f->caret = 0; return; } ++k; }
    if (!f->out[0]) return;
    if (f->out[0] != '/') {                     // a name: in the folder shown
        ui_ncpy(name, f->out, WK_NAME);
        if (!ui_join(f->out, f->dir, name, WK_PATH)) { ui_ncpy(f->out, name, WK_NAME); return; }
    }
    wk_dialog_answer(d, 1);
}

static void file_draw(int w, int cw, int ch, int focused) {
    struct fdlg *f;
    int k, y, lw;
    f = (struct fdlg *)wk_state(w);
    d_rect(0, 0, cw, ch, C_FACE);
    d_text(12, 10, C_DARK, F_SANS, f->dir);
    lw = cw - 24;
    d_rect(12, 28, lw, FB_ROWS * FB_ROW + 4, C_WHITE);
    d_bevel(12, 28, lw, FB_ROWS * FB_ROW + 4, B_SUNKEN);
    k = f->top; y = 30;
    while (k < f->n && k < f->top + FB_ROWS) {
        if (k == f->sel) d_rect(14, y, lw - 22, FB_ROW, C_SEL);
        if (f->isdir[f->order[k]]) d_icon(ICON_FILES, 16, y);
        d_text(36, y + 2, k == f->sel ? C_WHITE : C_DARK, F_SANS, fname(f, k));
        y = y + FB_ROW;
        ++k;
    }
    ui_scrollbar(12 + lw - 18, 30, FB_ROWS * FB_ROW, f->n, FB_ROWS, f->top);
    d_text(12, ch - 64, C_DARK, F_SANS, f->save ? "Save as:" : "File name:");
    ui_field(80, ch - 68, cw - 92, f->out, f->caret, 1);
    buttons_draw(cw, ch, f->save ? "Save" : "Open", "Cancel");
}

static void file_mouse(int w, int ev, int x, int y, int b) {
    struct fdlg *f;
    struct win *d;
    int cw, ch, k, t, lw;
    if (!(d = win_get(w))) return;
    f = (struct fdlg *)d->st;
    wk_size(w, &cw, &ch);
    lw = cw - 24;
    if (ev == E_WHEEL) {
        f->top = f->top + b * 3;
        if (f->top > f->n - FB_ROWS) f->top = f->n - FB_ROWS;
        if (f->top < 0) f->top = 0;
        return;
    }
    if (ev == E_MOVE) { t = ui_scroll_drag(12 + lw - 18, 30, y); if (t >= 0) f->top = t; return; }
    if (ev != E_DOWN && ev != E_DBL) return;
    if (ev == E_DOWN && (k = buttons_hit(x, y, cw, ch)) >= 0) { if (k) fb_ok(d, f); else wk_dialog_answer(d, 0); return; }
    t = ui_scroll_click(x, y, 12 + lw - 18, 30, FB_ROWS * FB_ROW, f->n, FB_ROWS, f->top);
    if (t >= 0) { f->top = t; return; }
    if (ui_hit(x, y, 14, 30, lw - 22, FB_ROWS * FB_ROW)) {
        k = f->top + (y - 30) / FB_ROW;
        if (k >= f->n) return;
        f->sel = k;
        if (ev == E_DBL) {
            if (f->isdir[f->order[k]]) { fb_enter(f, fname(f, k)); return; }
            ui_ncpy(f->out, fname(f, k), WK_PATH);
            fb_ok(d, f);
            return;
        }
        if (!f->isdir[f->order[k]]) { ui_ncpy(f->out, fname(f, k), WK_PATH); f->caret = ui_len(f->out); }
        return;
    }
    if (ui_hit(x, y, 80, ch - 68, cw - 92, 22)) f->caret = ui_field_hit(x, 80, cw - 92, f->out, f->caret);
}

static void file_key(int w, int down, int code, int ch, int mods) {
    struct fdlg *f;
    struct win *d;
    if (!down || !(d = win_get(w))) return;
    f = (struct fdlg *)d->st;
    if (code == KEY_ESC) { wk_dialog_answer(d, 0); return; }
    if (code == KEY_ENTER) { fb_ok(d, f); return; }
    if (code == KEY_BACK && !f->out[0]) { fb_enter(f, ".."); return; }
    ui_field_key(f->out, WK_NAME - 1, &f->caret, code, ch, mods);
}

struct wk_class wk_file_class = { .kind = ICON_FILEBOX, .draw = file_draw, .mouse = file_mouse, .key = file_key };

void wk_filebox(int owner, int dialog, int save, char *pattern, char *buf, int max) {
    struct win *d;
    struct fdlg *f;
    char *s;
    int k;
    if (!(d = dialog_open(&wk_file_class, sizeof(struct fdlg), owner, dialog, save ? "Save As" : "Open", 420, 330))) return;
    f = (struct fdlg *)d->st;
    f->buf = buf; f->max = max; f->save = save;
    ui_ncpy(f->pattern, pattern ? pattern : "*", WK_NAME);
    // buf: a folder to start in, or a path whose folder it is and whose name is offered
    f->dir[0] = '/'; f->dir[1] = 0;
    if (buf && buf[0] == '/') {
        ui_ncpy(f->dir, buf, WK_PATH);
        s = ui_base(f->dir);
        if (s[0] && !ui_ends(f->dir, "/")) { ui_ncpy(f->out, s, WK_NAME); k = s - f->dir; f->dir[k > 1 ? k - 1 : 1] = 0; }
    } else if (buf && buf[0]) ui_ncpy(f->out, buf, WK_NAME);
    f->caret = ui_len(f->out);
    fb_list(f);
}
