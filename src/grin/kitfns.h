// kitfns.h -- the window kit's functions that grin offers its
// applications (docs/grin-plan.md 3.4), one line each: what winkit.h
// declares for a window's code, in the order of its table (gkit.h).
// Included three times, with K and V defined differently each time:
// gkit.h makes the table's members of them, import.c a forwarding stub
// each, and apps.c fills the table.
//
//   K(type, name, (parameters), (arguments))   a function returning type
//   V(name, (parameters), (arguments))         one returning nothing
//
// Not here: the shell's section of winkit.h (wk_init, wk_event, wk_tick,
// wk_redraw and the rest), which is grin's alone; the screen savers,
// which are grin's too; the accessories (calc_open, clock_open, ...),
// which an application links itself; and the host's memory and files,
// which import.c serves from the application's own libc, so that what an
// application allocates and opens is its own and goes when it does. No
// guard: it is meant to be included more than once. New functions go at
// the end, so that the table only grows (GRIN_KIT_VERSION).

// windows
K(int, wk_open, (struct wk_class *c, char *title, int w, int h, int flags, char *arg), (c, title, w, h, flags, arg))
V(wk_close, (int w), (w))
V(wk_ask_close, (int w), (w))
K(void *, wk_state, (int w), (w))
K(int, wk_alive, (int w), (w))
V(wk_title, (int w, char *s), (w, s))
K(char *, wk_get_title, (int w), (w))
V(wk_invalidate, (int w, int x, int y, int ww, int hh), (w, x, y, ww, hh))
V(wk_menu, (int w, struct wk_menu *m, int n), (w, m, n))
K(int, wk_timer, (int w, int id, int ms), (w, id, ms))
V(wk_raise, (int w), (w))
K(int, wk_focus, (), ())
V(wk_size, (int w, int *cw, int *ch), (w, cw, ch))
V(wk_place, (int w, int x, int y, int ww, int hh), (w, x, y, ww, hh))
K(int, wk_find, (struct wk_class *c), (c))
K(int, wk_next, (int w), (w))
K(struct wk_class *, wk_class_of, (int w), (w))
K(int, wk_modal, (), ())
V(wk_redraw_all, (), ())
K(int, wk_now, (), ())
K(int, wk_clock, (), ())
// dialogs, and the clipboard
V(wk_msgbox, (int owner, int dialog, char *title, char *text, int buttons), (owner, dialog, title, text, buttons))
V(wk_inputbox, (int owner, int dialog, char *title, char *prompt, char *buf, int max), (owner, dialog, title, prompt, buf, max))
V(wk_filebox, (int owner, int dialog, int save, char *pattern, char *buf, int max), (owner, dialog, save, pattern, buf, max))
K(int, wk_clip_set, (char *s, int n), (s, n))
K(char *, wk_clip_get, (int *n), (n))
K(int, wk_readdir, (char *dir, int i, char *name), (dir, i, name))
// drawing
V(d_rect, (int x, int y, int w, int h, int rgb), (x, y, w, h, rgb))
V(d_recto, (int x, int y, int w, int h, int rgb), (x, y, w, h, rgb))
V(d_line, (int x0, int y0, int x1, int y1, int rgb), (x0, y0, x1, y1, rgb))
V(d_disc, (int x, int y, int r, int rgb), (x, y, r, rgb))
V(d_text, (int x, int y, int rgb, int font, char *s), (x, y, rgb, font, s))
V(d_textn, (int x, int y, int rgb, int size, int font, int advance, char *s, int n), (x, y, rgb, size, font, advance, s, n))
V(d_bevel, (int x, int y, int w, int h, int style), (x, y, w, h, style))
V(d_icon, (int kind, int x, int y), (kind, x, y))
V(d_clip, (int x, int y, int w, int h), (x, y, w, h))
V(d_clip_end, (), ())
V(d_imgdef, (int id, int w, int h, int *px), (id, w, h, px))
V(d_image, (int id, int x, int y), (id, x, y))
V(d_blit8, (int x, int y, int w, int h, char *src, int pitch, int *pal, int dw, int dh), (x, y, w, h, src, pitch, pal, dw, dh))
K(int, ui_mix, (int a, int b, int t, int n), (a, b, t, n))
K(int, wk_scheme_count, (), ())
K(char *, wk_scheme_name, (int i), (i))
V(wk_scheme_use, (int i, int desk), (i, desk))
// widgets
V(ui_button, (int x, int y, int w, int h, char *label, int pressed, int deflt), (x, y, w, h, label, pressed, deflt))
K(int, ui_hit, (int px, int py, int x, int y, int w, int h), (px, py, x, y, w, h))
V(ui_scrollbar, (int x, int y, int h, int total, int visible, int top), (x, y, h, total, visible, top))
K(int, ui_scroll_click, (int px, int py, int x, int y, int h, int total, int visible, int top), (px, py, x, y, h, total, visible, top))
K(int, ui_scroll_drag, (int x, int y, int py), (x, y, py))
V(ui_hscrollbar, (int x, int y, int w, int total, int visible, int left), (x, y, w, total, visible, left))
K(int, ui_hscroll_click, (int px, int py, int x, int y, int w, int total, int visible, int left), (px, py, x, y, w, total, visible, left))
K(int, ui_scroll_active, (), ())
V(ui_scroll_end, (), ())
V(ui_field, (int x, int y, int w, char *text, int caret, int focused), (x, y, w, text, caret, focused))
K(int, ui_field_key, (char *text, int max, int *caret, int code, int ch, int mods), (text, max, caret, code, ch, mods))
K(int, ui_field_hit, (int px, int x, int w, char *text, int caret), (px, x, w, text, caret))
V(ui_tabs, (int x, int y, char **labels, int n, int sel), (x, y, labels, n, sel))
K(int, ui_tab_hit, (int px, int py, int x, int y, char **labels, int n), (px, py, x, y, labels, n))
V(ui_group, (int x, int y, int w, int h, char *label), (x, y, w, h, label))
V(ui_radio, (int x, int y, char *label, int on), (x, y, label, on))
V(ui_header, (int x, int y, int *widths, char **labels, int n), (x, y, widths, labels, n))
K(char *, ui_num, (char *buf, int n), (buf, n))
K(char *, ui_num_commas, (char *buf, int n), (buf, n))
K(int, ui_len, (char *s), (s))
V(ui_cpy, (char *d, char *s), (d, s))
V(ui_cat, (char *d, char *s), (d, s))
K(int, ui_ncpy, (char *d, char *s, int n), (d, s, n))
K(int, ui_ncat, (char *d, char *s, int n), (d, s, n))
K(int, ui_join, (char *out, char *dir, char *name, int n), (out, dir, name, n))
K(char *, ui_base, (char *path), (path))
K(int, ui_eq, (char *a, char *b), (a, b))
K(int, ui_ends, (char *s, char *suffix), (s, suffix))
