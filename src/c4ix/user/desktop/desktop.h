// desktop.h -- what the desktop's modules share. docs/c4ix-desktop.md.
//
// The desktop is one C4IX task built from several modules:
//   wm.c        the window manager: display, events, windows, taskbar,
//               Go menu, menu bars, dialogs, and the drawing layer
//   ui.c        widgets drawn through that layer
//   term.c      Terminal: a terminal running c4ix-sh over pipes
//   files.c     Files
//   taskmgr.c   System Monitor
//   apps.c      Text Editor, Calculator, and the small windows (About)
//   edit.c      Editor: the IDE, a VIM-mode editor that builds with b4ke
//
// Only wm.c includes gui.h and touches the display. Everything else draws
// through the d_* functions below, which wm.c clips to the window being
// drawn, and receives events in its own client coordinates.

#include "c4ix_user.h"

enum { SW = 800, SH = 600, TASK_H = 30, BORDER = 4, TITLE_H = 18, MENU_H = 20,
       ST_BYTES = 73728, SIGTERM = 15 };

// window kinds
enum { K_TERM = 1, K_ABOUT = 2, K_FILES = 3, K_TASKS = 4, K_NOTE = 5, K_CALC = 6,
       K_MSG = 7, K_INPUT = 8, K_EDIT = 9, K_SETTINGS = 10 };

// colours: these never change...
enum { C_DARK = 0x000000, C_WHITE = 0xffffff, C_GREEN = 0x00ff00 };
// ... and these are the colour scheme's (Settings), defined in wm.c
extern int C_DESK;
extern int C_FACE;
extern int C_LIGHT;
extern int C_SHADOW;
extern int C_TITLE;
extern int C_TITLE2;
extern int C_ITITLE;
extern int C_ITITLE2;
extern int C_SEL;

// fonts: monospace, a UI face, and its bold
enum { F_MONO = 0, F_SANS = 1, F_BOLD = 2 };

// bevel styles
enum { B_RAISED = 1, B_SUNKEN = 2, B_FIELD = 3, B_PRESSED = 4, B_ETCHED = 5 };
// the look (Settings): flat outlines and solid title bars, or bevels and gradients
enum { S_FLAT = 0, S_CLASSIC = 1 };
extern int ui_style;
void theme_set_style(int style);   // remembered in /etc/desktop.cfg, as the rest
int  ui_mix(int a, int b, int t, int n);   // a colour T/N of the way from A to B

// mouse events, in client coordinates
enum { E_DOWN = 1, E_UP = 2, E_MOVE = 3, E_DBL = 4, E_WHEEL = 5 };

// keys (browser keyCodes)
enum { KEY_BACK = 8, KEY_TAB = 9, KEY_ENTER = 13, KEY_ESC = 27, KEY_PGUP = 33, KEY_PGDN = 34,
       KEY_END = 35, KEY_HOME = 36, KEY_LEFT = 37, KEY_UP = 38, KEY_RIGHT = 39, KEY_DOWN = 40,
       KEY_DEL = 46, KEY_F2 = 113, KEY_F5 = 116 };
enum { M_SHIFT = 1, M_CTRL = 2, M_ALT = 4 };
extern int wm_mods;      // the modifiers held now, for a click (wm.c)

// dialog results reach the window that asked through app_answer()
enum { A_NONE = 0, A_DELETE = 1, A_RENAME = 2, A_NEWDIR = 3, A_OPEN = 4, A_SAVEAS = 5,
       A_RUN = 6, A_ENDPROC = 7 };

// Each window is its own allocation, so there are as many as memory
// allows. A window's number (win_open's answer, win_get's question) is
// never reused, so a number kept for a window that has since closed
// finds nothing.
struct win {
    int id;                       // its number
    struct win *next;             // the next window, in the order they opened
    struct win *above, *below;    // z-order (wm.c's)
    int slot;                     // where it cascaded to when it opened
    int used, kind, x, y, w, h;
    int min, max, rx, ry, rw, rh;
    int resizable;
    char title[48];
    char *st;                     // this window's state, ST_BYTES, the tool's own struct
    // a menu bar: titles, and for each an array of items ending in 0
    // ("-" is a separator)
    int nmenus;
    char **menu_titles;
    char ***menu_items;
    // a dialog: which window asked, and what for
    int owner, action;
};

// ---- wm.c: the drawing layer (clipped to the window being drawn) ----
void d_rect(int x, int y, int w, int h, int rgb);
void d_recto(int x, int y, int w, int h, int rgb);
void d_line(int x0, int y0, int x1, int y1, int rgb);
void d_disc(int x, int y, int r, int rgb);
void d_text(int x, int y, int rgb, int font, char *s);
void d_textn(int x, int y, int rgb, int size, int font, int advance, char *s, int n);
void d_bevel(int x, int y, int w, int h, int style);
void d_icon(int kind, int x, int y);
void d_clip(int x, int y, int w, int h);  // narrower than the window, until d_clip_end
void d_clip_end();

// ---- wm.c: windows ----
struct win *win_get(int i);          // the window with this number, or 0
int  win_index(struct win *w);       // ... and a window's number
struct win *win_first();             // the windows in the order they opened:
                                     //   w = win_first(); while (w) { ...; w = w->next; }
                                     // a window closed meanwhile is still on it, with used 0
int  win_open(int kind, char *title, int w, int h);
void win_close(int i);
void win_title(struct win *w, char *s);
void win_raise(int i);
int  win_focused();
void win_menus(struct win *w, int n, char **titles, char ***items);
void mark_dirty();                    // the whole scene is to be drawn again
void mark_win_dirty(struct win *w);   // only this window's contents changed
void mark_rect_dirty(int x, int y, int w, int h);  // only this rectangle
int  now_ms();
void msgbox(struct win *owner, char *title, char *text, int yesno, int action);
void inputbox(struct win *owner, char *title, char *prompt, char *initial, int action);
char *dialog_text();              // the text an input dialog was answered with
void run_command(char *cmd);      // a new Terminal running cmd

// ---- ui.c: widgets ----
void ui_button(int x, int y, int w, int h, char *label, int pressed, int deflt);
int  ui_hit(int px, int py, int x, int y, int w, int h);
void ui_scrollbar(int x, int y, int h, int total, int visible, int top);
int  ui_scroll_click(int px, int py, int x, int y, int h, int total, int visible, int top);
int  ui_scroll_drag(int x, int y, int py);     // the top line while the bar at x,y is dragged, or -1
void ui_hscrollbar(int x, int y, int w, int total, int visible, int left);
int  ui_hscroll_click(int px, int py, int x, int y, int w, int total, int visible, int left);
                                               // ... and a bar across: ui_scroll_drag(x, y, px)
int  ui_scroll_active();
void ui_scroll_end();
void ui_field(int x, int y, int w, char *text, int caret, int focused);
void ui_tabs(int x, int y, char **labels, int n, int sel);
int  ui_tab_hit(int px, int py, int x, int y, char **labels, int n);
void ui_group(int x, int y, int w, int h, char *label);
void ui_radio(int x, int y, char *label, int on);
void ui_header(int x, int y, int *widths, char **labels, int n);
char *ui_num(char *buf, int v);                         // decimal
char *ui_num_commas(char *buf, int v);                  // 1,234,567
int  ui_len(char *s);
void ui_cpy(char *d, char *s);
void ui_cat(char *d, char *s);
int  ui_eq(char *a, char *b);
int  ui_ends(char *s, char *suffix);
int  ui_field_key(char *text, int max, int *caret, int code, int ch, int mods);

// ---- the tools: each window kind's hooks (wm.c calls these) ----
int  term_open(char *cmd);
void term_draw(struct win *w, int ox, int oy, int cw, int ch, int focused);
void term_key(struct win *w, int code, int ch, int mods);
void term_keyup(struct win *w, int code, int ch, int mods);
void term_drawn(struct win *w);
void term_tick(struct win *w);
void term_close(struct win *w);
void term_mouse(struct win *w, int ev, int x, int y, int cw, int ch);

int  files_open(char *path);
void files_draw(struct win *w, int ox, int oy, int cw, int ch, int focused);
void files_mouse(struct win *w, int ev, int x, int y, int cw, int ch);
void files_key(struct win *w, int code, int ch, int mods);
void files_command(struct win *w, int menu, int item);
void files_answer(struct win *w, int action, int yes);

int  tasks_open();
int  tasks_open_perf();           // System Monitor on its Performance tab
int  tasks_cpu_usage();           // the machine's CPU use now, in percent (the taskbar's meter)
void tasks_draw(struct win *w, int ox, int oy, int cw, int ch, int focused);
void tasks_mouse(struct win *w, int ev, int x, int y, int cw, int ch);
void tasks_key(struct win *w, int code, int ch, int mods);
void tasks_tick(struct win *w);
void tasks_answer(struct win *w, int action, int yes);

int  note_open(char *path);
void note_draw(struct win *w, int ox, int oy, int cw, int ch, int focused);
void note_mouse(struct win *w, int ev, int x, int y, int cw, int ch);
void note_key(struct win *w, int code, int ch, int mods);
void note_command(struct win *w, int menu, int item);
void note_answer(struct win *w, int action, int yes);

int  calc_open();
void calc_draw(struct win *w, int ox, int oy, int cw, int ch, int focused);
void calc_mouse(struct win *w, int ev, int x, int y, int cw, int ch);
void calc_key(struct win *w, int code, int ch, int mods);

// ---- wm.c: colour schemes ----
int  theme_count();
char *theme_name(int i);
int  theme_scheme();               // the scheme in use
int  theme_bg();                   // the desktop's own colour, or -1 for the scheme's
void theme_set(int scheme, int bg); // use, redraw, and remember them (/etc/desktop.cfg); the picture off
void theme_set_wall(int on);        // the picture (wall.c) on or off, remembered too
int  theme_wall();
int  theme_saver();
int  theme_wait();
void theme_set_saver(int which, int wait);   // the screen saver and its minutes, remembered
void saver_preview();                        // the chosen saver now (Settings)
// savers.c: the screen savers
enum { SAVER_MS = 33 };
int  saver_count();
char *saver_name(int i);
int  saver_active();
void saver_start(int which);
void saver_frame();
void saver_stop();
int  saver_frames();
// wall.c: the desktop's picture
int  wall_load(char *path);
int  wall_draw(int x, int y, int w, int h);   // 0: no picture, fill with C_DESK
int  wall_shown();
void d_imgdef(int id, int w, int h, int *px);
void d_image(int id, int x, int y);
// w x h 8-bit pixels at src through 256 colours at pal, scaled to dw x dh
void d_blit8(int x, int y, int w, int h, char *src, int pitch, int *pal, int dw, int dh);
int  theme_swatch(int i);          // the desktop colours Settings offers, 0..7

int  settings_open();
void settings_draw(struct win *w, int ox, int oy, int cw, int ch, int focused);
void settings_mouse(struct win *w, int ev, int x, int y, int cw, int ch);
void settings_key(struct win *w, int code, int ch, int mods);

// pointer shapes, GUI_PTR_*'s numbers (gui.h is wm.c's alone)
enum { PTR_ARROW = 0, PTR_TEXT = 1, PTR_RESIZE = 2 };
int  edit_pointer(struct win *w, int x, int y, int cw, int ch);   // PTR_* for the pointer there
int  edit_open(char *path);
void edit_draw(struct win *w, int ox, int oy, int cw, int ch, int focused);
void edit_mouse(struct win *w, int ev, int x, int y, int cw, int ch);
void edit_key(struct win *w, int code, int ch, int mods);
void edit_command(struct win *w, int menu, int item);
void edit_answer(struct win *w, int action, int yes);
void edit_tick(struct win *w);
void edit_close(struct win *w);

void about_open();
void about_draw(struct win *w, int ox, int oy, int cw, int ch, int focused);
