// desktop.h -- what the desktop's modules share. docs/c4ix-desktop.md.
//
// The desktop is one C4IX task, the C4IX shell over the window kit
// (src/winkit, winkit.h): the kit has the windows, their chrome, the
// widgets, dialogs, the text grid, the screen savers and the Calculator
// and Text Editor (the kit's Notepad); these modules are C4IX's own:
//   shell.c     the display, the main loop, the desktop's icons and
//               picture, the taskbar and Go menu, Settings' choices kept
//   tools.c     each window kind's class (its hooks, below), and the
//               calls the tools were written against, over the kit's
//   host.c      what the kit asks of its host: memory and files
//   term.c      Terminal: a terminal running c4ix-sh over pipes
//   files.c     Files
//   taskmgr.c   System Monitor
//   apps.c      About and Settings
//   edit.c      Editor: the IDE, a VIM-mode editor that builds with b4ke
//   wall.c      the desktop's picture
//
// Only the kit and shell.c touch the display. Everything else draws
// through the kit's d_* functions, which draw from the window's corner
// (the tools here are handed ox, oy = 0), clipped to it, and receives
// events in its own client coordinates.

#include "c4ix_user.h"
#include "wkwin.h"

enum { TASK_H = 30, SIGTERM = 15 };

// window kinds (the kit's icons have the same numbers)
enum { K_TERM = 1, K_ABOUT = 2, K_FILES = 3, K_TASKS = 4, K_NOTE = 5, K_CALC = 6,
       K_MSG = 7, K_INPUT = 8, K_EDIT = 9, K_SETTINGS = 10, K_CLOCK = 11, N_KINDS = 12 };

// dialog results reach the window that asked through its answer hook
enum { A_NONE = 0, A_DELETE = 1, A_RENAME = 2, A_NEWDIR = 3, A_OPEN = 4, A_SAVEAS = 5,
       A_RUN = 6, A_ENDPROC = 7 };

#define wm_mods (wk.mods)     // the modifiers held now, for a click

// ---- tools.c: windows as the tools see them (struct win, wkwin.h) ----
int  win_open(int kind, char *title, int w, int h, int size);   // size: the tool's state
void win_close(int i);
void win_title(struct win *w, char *s);
void win_raise(int i);
int  win_focused();
void mark_dirty();                    // the whole scene is to be drawn again
void mark_win_dirty(struct win *w);   // only this window's contents changed
int  now_ms();
void msgbox(struct win *owner, char *title, char *text, int yesno, int action);
void inputbox(struct win *owner, char *title, char *prompt, char *initial, int action);
char *dialog_text();                  // the text an input dialog was answered with
void run_command(char *cmd);          // a new Terminal running cmd
void launch(int kind);                // a new window of that kind (the Go menu, the icons)
struct wk_class *kind_class(int kind);

// ---- the tools: each window kind's hooks (tools.c calls these) ----
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

int  note_open(char *path);       // the kit's Notepad, as the Text Editor

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

int  settings_open();
void settings_draw(struct win *w, int ox, int oy, int cw, int ch, int focused);
void settings_mouse(struct win *w, int ev, int x, int y, int cw, int ch);
void settings_key(struct win *w, int code, int ch, int mods);

// ---- shell.c: the colour scheme, the look, the picture and the saver ----
// each choice used at once, redrawn, and remembered in /etc/desktop.cfg
int  theme_count();
char *theme_name(int i);
int  theme_scheme();               // the scheme in use
int  theme_bg();                   // the desktop's own colour, or -1 for the scheme's
void theme_set(int scheme, int bg); // the picture off
void theme_set_wall(int on);        // the picture (wall.c) on or off
int  theme_wall();
int  theme_saver();
int  theme_wait();
void theme_set_saver(int which, int wait);   // the screen saver and its minutes
void theme_set_style(int style);   // S_FLAT, S_CLASSIC or S_WIN31
void saver_preview();              // the chosen saver now (Settings)
int  theme_swatch(int i);          // the desktop colours Settings offers, 0..7

// the screen's size (Settings): one of the sizes offered, asked of the
// display at once and when the desktop starts, and remembered with the rest
int  display_count();
char *display_name(int i);         // "800x600"
int  display_cur();                // the one in use
void display_set(int i);

// ---- wall.c: the desktop's picture ----
int  wall_load(char *path);
int  wall_draw(int x, int y, int w, int h);   // 0: no picture, fill with C_DESK
int  wall_shown();
void wall_resize();                           // the screen changed size: the tiles again
