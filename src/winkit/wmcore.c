// wmcore.c -- the window kit's window manager. docs/grin-plan.md 3.5.
//
// The windows: a list in the order they opened, and through the same
// nodes a z-order, bottom first. Focus is the topmost window showing; a
// dialog on top keeps the input to itself. The mouse drags title bars and
// sizes windows, menu bars open drop-downs, and everything else is handed
// to the window's class, in its client area's coordinates. The shell
// draws what lies under and over the windows (struct wk_shell).

#include "wkwin.h"
#include "libjs/guest/gui.h"

static struct wk_shell *shell;
static struct win *first;
static struct win *zbot, *ztop;
static int next_id;
static int drag, drag_dx, drag_dy;        // a window's number while its title bar is dragged
static int sizing, size_edges, size_w0, size_h0, size_x0, size_y0, size_wx, size_wy;
static int down_win, down_t, down_x, down_y;   // for double clicks
static int menu_win, menu_idx, menu_hot;  // an open drop-down: its window, menu (-1 the
                                          // system menu), hot item
static int ptr_shape;
static char *clip_text;
static int clip_n;

enum { MIN_W = 220, MIN_H = 140, DROP_W = 160 };

int wk_now() { return __time(); }

// ---- the list -------------------------------------------------------------------

struct win *win_get(int i) {
    struct win *w;
    if (i <= 0) return 0;
    w = first;
    while (w) { if (w->id == i && w->used) return w; w = w->next; }
    return 0;
}
int win_index(struct win *w) { return w ? w->id : 0 - 1; }
struct win *win_first() { return first; }

int wk_alive(int i) { return win_get(i) != 0; }
void *wk_state(int i) { struct win *w; w = win_get(i); return w ? w->st : 0; }
struct wk_class *wk_class_of(int i) { struct win *w; w = win_get(i); return w ? w->cls : 0; }
char *wk_get_title(int i) { struct win *w; w = win_get(i); return w ? w->title : ""; }

int wk_find(struct wk_class *c) {
    struct win *w;
    w = first;
    while (w) { if (w->used && w->cls == c) return w->id; w = w->next; }
    return 0;
}

int wk_next(int i) {
    struct win *w;
    w = first;
    if (i > 0) {
        while (w && w->id != i) w = w->next;
        if (w) w = w->next;
    }
    while (w && !w->used) w = w->next;
    return w ? w->id : 0;
}

int wk_client_x(struct win *w) { return w->x + BORDER; }
static int menu_h(struct win *w) { return w->nmenus ? MENU_H : 0; }
int wk_client_y(struct win *w) { return w->y + BORDER + TITLE_H + 1 + menu_h(w); }
int wk_client_w(struct win *w) { return w->w - 2 * BORDER; }
int wk_client_h(struct win *w) { return w->h - (wk_client_y(w) - w->y) - BORDER; }

void wk_size(int i, int *cw, int *ch) {
    struct win *w;
    if (!(w = win_get(i))) { *cw = 0; *ch = 0; return; }
    *cw = wk_client_w(w); *ch = wk_client_h(w);
}

static void z_unlink(struct win *w) {
    if (w->below) w->below->above = w->above; else if (zbot == w) zbot = w->above;
    if (w->above) w->above->below = w->below; else if (ztop == w) ztop = w->below;
    w->above = 0; w->below = 0;
}

void wk_raise(int i) {
    struct win *w;
    if (!(w = win_get(i))) return;
    if (ztop != w) {
        z_unlink(w);
        w->below = ztop;
        if (ztop) ztop->above = w; else zbot = w;
        ztop = w;
    }
    w->min = 0;
    wk_full = 1;
}

// The topmost window that is showing, or 0.
static struct win *focus() {
    struct win *w;
    w = ztop;
    while (w && w->min) w = w->below;
    return w;
}
int wk_focus() { struct win *f; f = focus(); return f ? f->id : 0; }

int wk_modal() {
    struct win *f;
    f = focus();
    return f && f->dialog ? f->id : 0;
}

// The windows closed since the last pass, freed.
static void sweep() {
    struct win *w, *p, *n;
    p = 0; w = first;
    while (w) {
        n = w->next;
        if (!w->used) {
            if (p) p->next = n; else first = n;
            wk_free(w->st); wk_free((char *)w);
        } else p = w;
        w = n;
    }
}

// Opens a window, or answers -1 when there is no memory for one.
int wk_open(struct wk_class *c, char *title, int w, int h, int flags, char *arg) {
    int slot, size;
    struct win *p, *q;
    if (!next_id) next_id = 1;
    if (!(p = (struct win *)wk_alloc(sizeof(struct win)))) return 0 - 1;
    size = c->size > 0 ? c->size : 16;
    if (!(p->st = wk_alloc(size))) { wk_free((char *)p); return 0 - 1; }
    // it cascades from the first place no open window holds
    slot = 0; q = first;
    while (q) { if (q->used && q->slot == slot) { ++slot; q = first; } else q = q->next; }
    p->slot = slot;
    p->id = next_id; ++next_id;
    if (!first) first = p;
    else { q = first; while (q->next) q = q->next; q->next = p; }
    p->cls = c; p->kind = c->kind;
    p->used = 1; p->resizable = !(flags & WK_FIXED); p->dialog = (flags & WK_DIALOG) != 0;
    p->owner = 0; p->action = 0;
    p->w = w; p->h = h;
    if (flags & WK_CENTER) { p->x = (wk.sw - w) / 2; p->y = (wk.sh - h) / 2 - 40; }
    else {
        p->x = wk.ax + 100 + (slot % 10) * 24 + (slot / 10) * 12; p->y = wk.ay + 20 + (slot % 10) * 22;
        if (p->x + w > wk.sw) p->x = wk.sw - w;
        if (p->x < 0) p->x = 0;
        if (p->y + h > wk.ay + wk.ah) p->y = wk.ay + wk.ah - h;
        if (p->y < 0) p->y = 0;
    }
    ui_ncpy(p->title, title, 48);
    wk_raise(p->id);
    if (c->open) c->open(p->id, arg);
    return p->id;
}

void wk_title(int i, char *s) {
    struct win *w;
    if (!(w = win_get(i))) return;
    ui_ncpy(w->title, s, 48);
    wk_full = 1;
}

void wk_menu(int i, struct wk_menu *m, int n) {
    struct win *w;
    if (!(w = win_get(i))) return;
    w->menus = m; w->nmenus = n;
}

void wk_place(int i, int x, int y, int ww, int hh) {
    struct win *w;
    if (!(w = win_get(i))) return;
    w->x = x; w->y = y; w->w = ww; w->h = hh;
    wk_full = 1;
}

void wk_close(int i) {
    struct win *w;
    if (!(w = win_get(i))) return;
    if (w->cls->destroy) w->cls->destroy(i);
    w->used = 0;
    z_unlink(w);
    if (drag == i) drag = 0;
    if (sizing == i) sizing = 0;
    if (menu_win == i) menu_win = 0;
    wk_full = 1;
}

void wk_ask_close(int i) {
    struct win *w;
    if (!(w = win_get(i))) return;
    if (w->dialog) { wk_dialog_answer(w, 0); return; }
    if (w->cls->close && !w->cls->close(i)) return;
    wk_close(i);
}

void wk_close_all() {
    struct win *w;
    w = first;
    while (w) { if (w->used) wk_close(w->id); w = w->next; }
    sweep();
}

// A window's own contents changed (or a part of them); nothing else did.
void wk_invalidate(int i, int x, int y, int ww, int hh) {
    struct win *w;
    if (!(w = win_get(i)) || w->min) return;
    if (ww <= 0) wk_dirty_rect(w->x, w->y, w->w, w->h);
    else wk_dirty_rect(wk_client_x(w) + x, wk_client_y(w) + y, ww, hh);
}

// A dialog's answer: to the window that asked, raised first, or to the
// shell when no window did. An owner closed meanwhile hears nothing.
void wk_answer_owner(int owner, int dialog, int result) {
    struct win *o;
    if (owner <= 0) { if (shell && shell->answer) shell->answer(dialog, result); return; }
    if (!(o = win_get(owner))) return;
    wk_raise(owner);
    if (o->cls->answer) o->cls->answer(owner, dialog, result);
}

// ---- timers ---------------------------------------------------------------------

int wk_timer(int i, int id, int ms) {
    struct win *w;
    int k, f;
    if (!(w = win_get(i))) return 0;
    f = 0 - 1;
    k = 0;
    while (k < WK_TIMERS) {
        if (w->tms[k] && w->tid[k] == id) break;
        if (!w->tms[k] && f < 0) f = k;
        ++k;
    }
    if (k == WK_TIMERS) { if (!ms) return 1; if (f < 0) return 0; k = f; }
    w->tid[k] = id; w->tms[k] = ms > 0 ? ms : 0;
    w->tnext[k] = wk_now() + ms;
    return 1;
}

static void timers() {
    struct win *w;
    int k, now;
    now = wk_now();
    w = first;
    while (w) {
        k = 0;
        while (w->used && k < WK_TIMERS) {
            if (w->tms[k] && now - w->tnext[k] >= 0) {
                w->tnext[k] = w->tnext[k] + w->tms[k];
                if (now - w->tnext[k] >= 0) w->tnext[k] = now + w->tms[k];   // fallen behind
                if (w->cls->timer) w->cls->timer(w->id, w->tid[k]);
            }
            ++k;
        }
        w = w->next;
    }
}

int wk_next_timer() {
    struct win *w;
    int k, d, best, now;
    now = wk_now();
    best = 0 - 1;
    w = first;
    while (w) {
        k = 0;
        while (w->used && k < WK_TIMERS) {
            if (w->tms[k]) {
                d = w->tnext[k] - now;
                if (d < 0) d = 0;
                if (best < 0 || d < best) best = d;
            }
            ++k;
        }
        w = w->next;
    }
    return best;
}

// ---- the clipboard ----------------------------------------------------------------

int wk_clip_set(char *s, int n) {
    char *t;
    if (n < 0) n = 0;
    if (!(t = wk_alloc(n + 1))) return 0;
    memcpy(t, s, n);
    t[n] = 0;
    if (clip_text) wk_free(clip_text);
    clip_text = t; clip_n = n;
    return 1;
}
char *wk_clip_get(int *n) { *n = clip_text ? clip_n : 0; return clip_text ? clip_text : ""; }

// ---- menus ------------------------------------------------------------------------

static char *sys_items[6] = { "Restore", "Minimize", "Maximize", "-", "Close", 0 };
static char *sys_dlg_items[2] = { "Close", 0 };
enum { SYS_RESTORE = 0, SYS_MIN = 1, SYS_MAX = 2, SYS_CLOSE = 4 };

int wk_menu_w(struct win *w, int k) { return ui_len(w->menus[k].title) * 7 + 12; }
int wk_menu_x(struct win *w, int k) {
    int x, j;
    x = wk_client_x(w) + 4;
    j = 0;
    while (j < k) { x = x + wk_menu_w(w, j); ++j; }
    return x;
}

static int count_items(char **items) { int n; n = 0; while (items[n]) ++n; return n; }

static char **drop_items(struct win *w) {
    if (menu_idx < 0) return w->dialog ? sys_dlg_items : sys_items;
    return w->menus[menu_idx].items;
}
static int drop_x(struct win *w) { return menu_idx < 0 ? w->x + BORDER : wk_menu_x(w, menu_idx); }
static int drop_y(struct win *w) {
    return menu_idx < 0 ? w->y + BORDER + TITLE_H + 1 : w->y + BORDER + TITLE_H + 1 + MENU_H;
}

static void draw_dropdown() {
    struct win *w;
    char **items;
    int n, x, y, k, iy, mh;
    if (!(w = win_get(menu_win))) return;
    items = drop_items(w);
    n = count_items(items);
    x = drop_x(w); y = drop_y(w);
    mh = 6;
    k = 0; while (k < n) { if (items[k][0] == '-') mh = mh + 8; else mh = mh + 20; ++k; }
    gui_rect(x, y, DROP_W, mh, C_FACE);
    d_bevel(x, y, DROP_W, mh, B_RAISED);
    iy = y + 3;
    k = 0;
    while (k < n) {
        if (items[k][0] == '-') { gui_rect(x + 3, iy + 3, DROP_W - 6, 1, C_SHADOW); gui_rect(x + 3, iy + 4, DROP_W - 6, 1, C_LIGHT); iy = iy + 8; }
        else {
            if (k == menu_hot) gui_rect(x + 3, iy, DROP_W - 6, 20, C_SEL);
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
    int n, x, k, iy;
    if (!(w = win_get(menu_win))) return 0 - 1;
    items = drop_items(w);
    n = count_items(items);
    x = drop_x(w);
    if (px < x || px >= x + DROP_W) return 0 - 1;
    iy = drop_y(w) + 3;
    k = 0;
    while (k < n) {
        if (items[k][0] == '-') iy = iy + 8;
        else { if (py >= iy && py < iy + 20) return k; iy = iy + 20; }
        ++k;
    }
    return 0 - 1;
}

static void toggle_max(struct win *w) {
    if (!w->max) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->x = wk.ax; w->y = wk.ay; w->w = wk.aw; w->h = wk.ah; w->max = 1;
    } else {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh; w->max = 0;
    }
    wk_full = 1;
}

static void sys_command(struct win *w, int k) {
    if (w->dialog || k == SYS_CLOSE) { wk_ask_close(w->id); return; }
    if (k == SYS_RESTORE && w->max) toggle_max(w);
    else if (k == SYS_MAX && !w->max) toggle_max(w);
    else if (k == SYS_MIN) { w->min = 1; wk_full = 1; }
}

// ---- drawing the scene --------------------------------------------------------------

struct wk_look *wk_look() {
    if (wk.style == S_CLASSIC) return &look_classic;
    return &look_flat;
}

static void draw_window(struct win *w, int focused) {
    struct wk_look *l;
    int cx, cy, cw, ch;
    l = wk_look();
    l->frame(w, focused);
    if (w->nmenus) l->menubar(w, focused, menu_win == w->id ? menu_idx : 0 - 1, menu_hot);
    cx = wk_client_x(w); cy = wk_client_y(w); cw = wk_client_w(w); ch = wk_client_h(w);
    wk_client_clip(cx, cy, cw, ch);
    wk_origin(cx, cy);
    if (w->cls->draw) w->cls->draw(w->id, cw, ch, focused);
    wk_origin(0, 0);
    wk_noclip();
    if (l->grip) l->grip(w);
}

static void draw_windows() {
    struct win *w, *f;
    f = focus();
    w = zbot;
    while (w) {
        if (!w->min && wk_in_damage(w->x, w->y, w->w, w->h)) draw_window(w, w == f);
        w = w->above;
    }
}

static void redraw() {
    wk_part = 0;
    wk_origin(0, 0);
    gui_clear(C_DESK);
    if (shell && shell->desktop) shell->desktop(0, 0, wk.sw, wk.sh);
    draw_windows();
    if (menu_win) draw_dropdown();
    if (shell && shell->front) shell->front();
    gui_show();
    wk_full = 0;
    wk_dmg = 0;
}

// Only the damage: the desktop under it, the windows that reach into it,
// back to front, and what the shell has over them. A menu open is drawn
// over everything, so that is still the whole scene.
static void redraw_part() {
    if (menu_win || (shell && shell->popup && shell->popup())) { redraw(); return; }
    wk_part = 1;
    wk_origin(0, 0);
    wk_noclip();
    if (shell && shell->desktop) shell->desktop(wk_dmg_x0, wk_dmg_y0, wk_dmg_x1 - wk_dmg_x0, wk_dmg_y1 - wk_dmg_y0);
    else gui_rect(wk_dmg_x0, wk_dmg_y0, wk_dmg_x1 - wk_dmg_x0, wk_dmg_y1 - wk_dmg_y0, C_DESK);
    draw_windows();
    if (shell && shell->front) shell->front();
    gui_noclip();
    gui_show();
    wk_part = 0;
    wk_dmg = 0;
}

int wk_dirty() { return wk_full || wk_dmg; }

int wk_redraw(int force) {
    if (force || wk_full) { redraw(); return 1; }
    if (wk_dmg) { redraw_part(); return 1; }
    return 0;
}

void wk_drawn() {
    struct win *w;
    w = first;
    while (w) { if (w->used && w->cls->drawn) w->cls->drawn(w->id); w = w->next; }
}

// ---- the screen's size -----------------------------------------------------------------
//
// The display's size is the kit's to ask for (gui_size) and the page's to
// give: the RESIZE that comes back, or one the page sends of its own, is
// when the windows are laid out again. Maximised windows fill the new
// area; the rest are pulled back inside it.

void wk_set_area(int x, int y, int w, int h) { wk.ax = x; wk.ay = y; wk.aw = w; wk.ah = h; }

static void relayout(int sw, int sh) {
    struct win *w;
    if (sw < 320) sw = 320;
    if (sh < 240) sh = 240;
    if (sw > WK_MAX_W) sw = WK_MAX_W;
    if (sh > WK_MAX_H) sh = WK_MAX_H;
    wk.sw = sw; wk.sh = sh;
    wk_set_area(0, 0, sw, sh);
    if (shell && shell->resized) shell->resized();
    w = first;
    while (w) {
        if (w->used && w->max) { w->x = wk.ax; w->y = wk.ay; w->w = wk.aw; w->h = wk.ah; }
        else if (w->used) {
            if (w->x + w->w > wk.ax + wk.aw) w->x = wk.ax + wk.aw - w->w;
            if (w->y + w->h > wk.ay + wk.ah) w->y = wk.ay + wk.ah - w->h;
            if (w->x < wk.ax) w->x = wk.ax;
            if (w->y < wk.ay) w->y = wk.ay;
            // too big for it now: as big as fits, if it can be sized
            if (w->resizable && w->w > wk.aw) w->w = wk.aw > MIN_W ? wk.aw : MIN_W;
            if (w->resizable && w->h > wk.ah) w->h = wk.ah > MIN_H ? wk.ah : MIN_H;
        }
        w = w->next;
    }
    menu_win = 0; drag = 0; sizing = 0;
    wk_full = 1;
}

void wk_screen(int w, int h) {
    gui_size(w, h);
    relayout(w, h);
}

void wk_init(struct wk_shell *s) {
    shell = s;
    if (!next_id) next_id = 1;
    wk.sw = gui_width(); wk.sh = gui_height();
    if (wk.sw <= 0) wk.sw = 640;
    if (wk.sh <= 0) wk.sh = 480;
    wk_set_area(0, 0, wk.sw, wk.sh);
    if (shell && shell->resized) shell->resized();
    ptr_shape = PTR_ARROW;
    wk_full = 1;
}

// ---- input ------------------------------------------------------------------------------

static void client_mouse(struct win *w, int ev, int x, int y, int b) {
    if (w->cls->mouse) w->cls->mouse(w->id, ev, x - wk_client_x(w), y - wk_client_y(w), b);
}

static void mouse_down(int x, int y, int button) {
    int k, i, t, m, ht;
    struct win *w;
    if (menu_win) {
        w = win_get(menu_win);
        k = dropdown_hit(x, y);
        i = menu_idx;
        menu_win = 0; wk_full = 1;
        if (!w) return;
        if (k >= 0) {
            if (i < 0) sys_command(w, k);
            else if (w->cls->command) w->cls->command(w->id, WK_CMD(i, k));
            return;
        }
        // a click on another title of the same bar opens that one instead
        if (i >= 0 && y >= w->y + BORDER + TITLE_H + 1 && y < w->y + BORDER + TITLE_H + 1 + MENU_H) {
            m = 0;
            while (m < w->nmenus) {
                if (m != i && x >= wk_menu_x(w, m) && x < wk_menu_x(w, m) + wk_menu_w(w, m)) {
                    menu_win = w->id; menu_idx = m; menu_hot = 0 - 1; return;
                }
                ++m;
            }
        }
        return;
    }
    if (shell && shell->mouse_front && shell->mouse_front(E_DOWN, x, y, button)) return;
    // windows, topmost first; a dialog keeps the input to itself
    w = ztop;
    while (w) {
        i = w->id;
        if (!w->min && ui_hit(x, y, w->x, w->y, w->w, w->h)) {
            if (wk_modal() && wk_modal() != i) return;
            wk_raise(i);
            ht = wk_look()->hit(w, x, y);
            t = wk_now();
            if (ht == HT_CLOSE) { wk_ask_close(i); return; }
            if (ht == HT_MAX) { toggle_max(w); return; }
            if (ht == HT_MIN) { w->min = 1; wk_full = 1; return; }
            if (ht == HT_SYSMENU) {
                // the control-menu box: a double click closes, a click opens its menu
                if (down_win == i && t - down_t < 500) { down_win = 0; wk_ask_close(i); return; }
                down_win = i; down_t = t;
                menu_win = i; menu_idx = 0 - 1; menu_hot = 0 - 1;
                return;
            }
            if (ht == HT_CAPTION) {
                if (!w->dialog && down_win == i && t - down_t < 500) { toggle_max(w); down_win = 0; return; }
                down_win = i; down_t = t;
                if (!w->max) { drag = i; drag_dx = x - w->x; drag_dy = y - w->y; }
                return;
            }
            if (ht >= HT_SIZE) {
                sizing = i; size_edges = ht - HT_SIZE;
                size_w0 = w->w; size_h0 = w->h; size_x0 = x; size_y0 = y; size_wx = w->x; size_wy = w->y;
                return;
            }
            if (ht == HT_MENUBAR) {
                m = 0;
                while (m < w->nmenus) {
                    if (x >= wk_menu_x(w, m) && x < wk_menu_x(w, m) + wk_menu_w(w, m)) {
                        menu_win = i; menu_idx = m; menu_hot = 0 - 1; wk_full = 1; return;
                    }
                    ++m;
                }
                return;
            }
            if (ht == HT_CLIENT) {
                client_mouse(w, E_DOWN, x, y, button);
                if (w->used && down_win == i && t - down_t < 500 && x - down_x < 5 && down_x - x < 5 && y - down_y < 5 && down_y - y < 5) {
                    client_mouse(w, E_DBL, x, y, button);
                    down_win = 0;
                } else { down_win = i; down_t = t; down_x = x; down_y = y; }
                wk_full = 1;
            }
            return;
        }
        w = w->below;
    }
    if (wk_modal()) return;
    if (shell && shell->mouse_desk) shell->mouse_desk(E_DOWN, x, y, button);
}

// ---- the pointer: the host's own, its shape chosen by what is under it ----
static int shape_at(int x, int y) {
    struct win *w;
    int cx, cy, s, ht;
    if (sizing) return PTR_RESIZE;
    if (drag || menu_win) return PTR_ARROW;
    if (shell && shell->pointer && (s = shell->pointer(x, y)) >= 0) return s;
    w = ztop;
    while (w) {
        if (!w->min && ui_hit(x, y, w->x, w->y, w->w, w->h)) {
            ht = wk_look()->hit(w, x, y);
            if (ht >= HT_SIZE) return PTR_RESIZE;
            if (ht != HT_CLIENT) return PTR_ARROW;
            cx = x - wk_client_x(w); cy = y - wk_client_y(w);
            if (w->cls->pointer) return w->cls->pointer(w->id, cx, cy);
            return w->cls->pointer_shape;
        }
        w = w->below;
    }
    return PTR_ARROW;
}
void wk_repoint() { ptr_shape = PTR_ARROW; }

static void pointer(int x, int y) {
    int s;
    s = shape_at(x, y);
    if (s != ptr_shape) { ptr_shape = s; gui_cursor(s); }
}

static void mouse_move(int x, int y, int b) {
    struct win *w;
    int k, dx, dy;
    pointer(x, y);
    if (drag && (w = win_get(drag))) {
        w->x = x - drag_dx; w->y = y - drag_dy;
        if (w->y < wk.ay) w->y = wk.ay;
        if (w->y > wk.ay + wk.ah - TITLE_H) w->y = wk.ay + wk.ah - TITLE_H;
        if (w->x < 60 - w->w) w->x = 60 - w->w;
        if (w->x > wk.sw - 60) w->x = wk.sw - 60;
        wk_full = 1;
        return;
    }
    if (sizing && (w = win_get(sizing))) {
        dx = x - size_x0; dy = y - size_y0;
        if (size_edges & SZ_R) w->w = size_w0 + dx;
        if (size_edges & SZ_B) w->h = size_h0 + dy;
        if (size_edges & SZ_L) w->w = size_w0 - dx;
        if (size_edges & SZ_T) w->h = size_h0 - dy;
        if (w->w < MIN_W) w->w = MIN_W;
        if (w->h < MIN_H) w->h = MIN_H;
        if (size_edges & SZ_L) w->x = size_wx + size_w0 - w->w;
        if (size_edges & SZ_T) w->y = size_wy + size_h0 - w->h;
        wk_full = 1;
        return;
    }
    if (shell && shell->mouse_front && shell->mouse_front(E_MOVE, x, y, b)) return;
    if (menu_win) { k = dropdown_hit(x, y); if (k != menu_hot) { menu_hot = k; wk_full = 1; } return; }
    if ((w = focus())) {
        // a thumb being dragged follows the pointer out of the window too
        if (ui_scroll_active() || ui_hit(x, y, wk_client_x(w), wk_client_y(w), wk_client_w(w), wk_client_h(w))) {
            client_mouse(w, E_MOVE, x, y, b);
            if (ui_scroll_active()) wk_full = 1;          // the thumb moved: draw it there
        }
    }
}

static void mouse_up(int x, int y, int b) {
    struct win *w;
    drag = 0; sizing = 0;
    ui_scroll_end();
    pointer(x, y);
    if ((w = focus())) client_mouse(w, E_UP, x, y, b);
}

static void wheel(int x, int y, int dy) {
    struct win *w;
    w = ztop;
    while (w) {
        if (!w->min && ui_hit(x, y, wk_client_x(w), wk_client_y(w), wk_client_w(w), wk_client_h(w))) {
            client_mouse(w, E_WHEEL, x, y, dy);
            wk_full = 1;
            return;
        }
        w = w->below;
    }
}

static void key_down(int code, int ch, int mods) {
    struct win *f;
    // typing between two clicks makes them two clicks, not a double click
    down_win = 0;
    if (shell && shell->key && shell->key(1, code, ch, mods)) return;
    if (menu_win && code == KEY_ESC) { menu_win = 0; wk_full = 1; return; }
    if (!(f = focus())) return;
    if (f->cls->key) f->cls->key(f->id, 1, code, ch, mods);
    else if (code == KEY_ENTER || code == KEY_ESC) wk_ask_close(f->id);   // About and the like
    wk_full = 1;
}

// A key let go matters only to a canvas: a game holds keys down.
static void key_up(int code, int ch, int mods) {
    struct win *f;
    if (!(f = focus())) return;
    if (f->cls->key) f->cls->key(f->id, 0, code, ch, mods);
}

void wk_event(int *ev) {
    int t;
    t = ev[0];
    if (t == GUI_EV_DOWN) mouse_down(ev[1], ev[2], ev[3]);
    else if (t == GUI_EV_UP) mouse_up(ev[1], ev[2], ev[3]);
    else if (t == GUI_EV_MOVE) mouse_move(ev[1], ev[2], ev[3]);
    else if (t == GUI_EV_KEYDOWN) { wk.mods = ev[3]; key_down(ev[1], ev[2], ev[3]); }
    else if (t == GUI_EV_KEYUP) { wk.mods = ev[3]; key_up(ev[1], ev[2], ev[3]); }
    else if (t == GUI_EV_WHEEL) wheel(ev[1], ev[2], ev[3]);
    else if (t == GUI_EV_RESIZE) {
        if (ev[1] > 0 && ev[2] > 0 && (ev[1] != wk.sw || ev[2] != wk.sh)) relayout(ev[1], ev[2]);
        wk_full = 1;
    } else if (t == GUI_EV_FOCUS) {
        if (!ev[1]) wk.mods = 0;            // let go of elsewhere
        wk_full = 1;
    }
}

// Every pass of the shell's loop: the windows closed since the last one
// freed, each window's idle hook, and the timers due.
void wk_tick() {
    struct win *w;
    sweep();
    w = first;
    while (w) { if (w->used && w->cls->idle) w->cls->idle(w->id); w = w->next; }
    timers();
}
