// desktop.h -- what the desktop's modules share. docs/c4ix-desktop.md.
//
// The desktop is one C4IX task built from several modules:
//   wm.c        the window manager: display, events, windows, taskbar,
//               Start menu, menu bars, dialogs, and the drawing layer
//   ui.c        widgets drawn through that layer
//   term.c      Command Prompt: a terminal running c4ix-sh over pipes
//   files.c     Explorer
//   taskmgr.c   Task Manager
//   apps.c      Notepad, Calculator, and the small windows (About)
//
// Only wm.c includes gui.h and touches the display. Everything else draws
// through the d_* functions below, which wm.c clips to the window being
// drawn, and receives events in its own client coordinates.

#include "c4ix_user.h"

enum { SW = 800, SH = 600, TASK_H = 30, BORDER = 4, TITLE_H = 18, MENU_H = 20,
       MAXW = 8, ST_BYTES = 73728, SIGTERM = 15 };

// window kinds
enum { K_TERM = 1, K_ABOUT = 2, K_FILES = 3, K_TASKS = 4, K_NOTE = 5, K_CALC = 6,
       K_MSG = 7, K_INPUT = 8 };

// colours
enum { C_DESK = 0x008080, C_FACE = 0xc0c0c0, C_LIGHT = 0xffffff, C_SHADOW = 0x808080,
       C_DARK = 0x000000, C_TITLE = 0x000080, C_TITLE2 = 0x1084d0, C_ITITLE = 0x808080,
       C_ITITLE2 = 0xb4b4b4, C_SEL = 0x000080, C_WHITE = 0xffffff, C_GREEN = 0x00ff00 };

// fonts: monospace, a UI face, and its bold
enum { F_MONO = 0, F_SANS = 1, F_BOLD = 2 };

// bevel styles
enum { B_RAISED = 1, B_SUNKEN = 2, B_FIELD = 3, B_PRESSED = 4, B_ETCHED = 5 };

// mouse events, in client coordinates
enum { E_DOWN = 1, E_UP = 2, E_MOVE = 3, E_DBL = 4, E_WHEEL = 5 };

// keys (browser keyCodes)
enum { KEY_BACK = 8, KEY_TAB = 9, KEY_ENTER = 13, KEY_ESC = 27, KEY_PGUP = 33, KEY_PGDN = 34,
       KEY_END = 35, KEY_HOME = 36, KEY_LEFT = 37, KEY_UP = 38, KEY_RIGHT = 39, KEY_DOWN = 40,
       KEY_DEL = 46, KEY_F2 = 113, KEY_F5 = 116 };
enum { M_SHIFT = 1, M_CTRL = 2, M_ALT = 4 };

// dialog results reach the window that asked through app_answer()
enum { A_NONE = 0, A_DELETE = 1, A_RENAME = 2, A_NEWDIR = 3, A_OPEN = 4, A_SAVEAS = 5,
       A_RUN = 6, A_ENDPROC = 7 };

struct win {
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

// ---- wm.c: windows ----
struct win *win_get(int i);
int  win_index(struct win *w);
int  win_open(int kind, char *title, int w, int h);
void win_close(int i);
void win_title(struct win *w, char *s);
void win_raise(int i);
int  win_focused();
void win_menus(struct win *w, int n, char **titles, char ***items);
void mark_dirty();
int  now_ms();
void msgbox(struct win *owner, char *title, char *text, int yesno, int action);
void inputbox(struct win *owner, char *title, char *prompt, char *initial, int action);
char *dialog_text();              // the text an input dialog was answered with
void run_command(char *cmd);      // a new Command Prompt running cmd

// ---- ui.c: widgets ----
void ui_button(int x, int y, int w, int h, char *label, int pressed, int deflt);
int  ui_hit(int px, int py, int x, int y, int w, int h);
void ui_scrollbar(int x, int y, int h, int total, int visible, int top);
int  ui_scroll_click(int px, int py, int x, int y, int h, int total, int visible, int top);
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
void term_tick(struct win *w);
void term_close(struct win *w);

int  files_open(char *path);
void files_draw(struct win *w, int ox, int oy, int cw, int ch, int focused);
void files_mouse(struct win *w, int ev, int x, int y, int cw, int ch);
void files_key(struct win *w, int code, int ch, int mods);
void files_command(struct win *w, int menu, int item);
void files_answer(struct win *w, int action, int yes);

int  tasks_open();
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

void about_open();
void about_draw(struct win *w, int ox, int oy, int cw, int ch, int focused);
