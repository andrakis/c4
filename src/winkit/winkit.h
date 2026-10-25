// winkit.h -- the window kit: windows, their chrome, widgets and the
// accessories, written once and built into C4IX's desktop and into grin
// (docs/grin-plan.md, 3.4 and 3.5).
//
//   draw.c      the d_* drawing layer over gui.h: the origin and clip of
//               what is being drawn, damage, colour schemes, bevels, icons
//   ui.c        widgets, stateless, drawn through d_*
//   wmcore.c    windows: the list, z-order, focus, modality, dragging and
//               sizing, menu bars and drop-downs, timers, the clipboard,
//               events, and each window's class called for what is its
//   dialog.c    message, input and file boxes
//   look95.c    window chrome: the Flat and Classic looks
//   look31.c    window chrome: the Windows 3.1 look
//   textgrid.c  a VT100 text grid, its scrollback and line editor (textgrid.h)
//   savers.c    screen savers
//   calc.c, notepad.c, clock.c   the accessories, each a class
//
// A shell (C4IX's desktop/, grin) owns the display and the main loop. It
// attaches the display, calls wk_init, then each pass hands every event
// to wk_event, calls wk_tick and wk_redraw. What the shell draws itself --
// the desktop under the windows, a taskbar over them -- it gives the kit
// as hooks (struct wk_shell). Windows are numbered (int handles that are
// never reused), and each belongs to a class: a table of what to call
// for drawing, the mouse, keys, menus, timers and dialog answers.
//
// Everything here compiles under c4lc on the host and c4sc inside C4IX:
// function-pointer tables, designated initialisers, and no sizeof(struct)
// in a constant (a class's size is set at run time).

#ifndef WINKIT_H
#define WINKIT_H 1

// the largest screen the kit lays out for (arrays sized by it), a path,
// and the window chrome's measures, which every look shares
enum { WK_MAX_W = 1280, WK_MAX_H = 1024, WK_PATH = 256, WK_NAME = 64 };
enum { BORDER = 4, TITLE_H = 18, MENU_H = 20 };

// ---- what the kit and its windows share -----------------------------------
//
// The screen's size, the area windows keep to, the look, the modifiers
// held, and the colour scheme. One struct, so that grin's applications,
// which reach the kit through a table (3.4), can find it through one
// pointer.
struct wk_env {
    int sw, sh;                       // the screen now (gui_size, GUI_EV_RESIZE)
    int ax, ay, aw, ah;               // where windows go: the screen less a taskbar
    int style;                        // the look: S_FLAT, S_CLASSIC, S_WIN31
    int mods;                         // Shift, Ctrl, Alt as the last key left them
    int desk, face, light, shadow;    // the scheme's colours (Settings)
    int title, title2, ititle, ititle2, sel;
};
// A grin application (compiled with -D WK_IMPORT, src/grin/import.c) has
// no wk of its own: it reads grin's, through the pointer grin publishes
// in its table, so that every C_FACE below is a load through it.
#ifdef WK_IMPORT
extern struct wk_env *wk_import_env;
#define wk (*wk_import_env)
#else
extern struct wk_env wk;
#endif

// colours: these never change...
enum { C_DARK = 0x000000, C_WHITE = 0xffffff, C_GREEN = 0x00ff00 };
// ... and these are the scheme's
#define C_DESK    (wk.desk)
#define C_FACE    (wk.face)
#define C_LIGHT   (wk.light)
#define C_SHADOW  (wk.shadow)
#define C_TITLE   (wk.title)
#define C_TITLE2  (wk.title2)
#define C_ITITLE  (wk.ititle)
#define C_ITITLE2 (wk.ititle2)
#define C_SEL     (wk.sel)

// the looks: flat outlines and solid title bars, bevels and gradients,
// or Windows 3.1's chrome (with Classic's bevels)
enum { S_FLAT = 0, S_CLASSIC = 1, S_WIN31 = 2, N_STYLES = 3 };
#define ui_style (wk.style)

// fonts: monospace, a UI face, and its bold
enum { F_MONO = 0, F_SANS = 1, F_BOLD = 2 };
// bevel styles
enum { B_RAISED = 1, B_SUNKEN = 2, B_FIELD = 3, B_PRESSED = 4, B_ETCHED = 5 };
// mouse events, in client coordinates
enum { E_DOWN = 1, E_UP = 2, E_MOVE = 3, E_DBL = 4, E_WHEEL = 5 };
// keys (browser keyCodes) and modifiers
enum { KEY_BACK = 8, KEY_TAB = 9, KEY_ENTER = 13, KEY_ESC = 27, KEY_PGUP = 33, KEY_PGDN = 34,
       KEY_END = 35, KEY_HOME = 36, KEY_LEFT = 37, KEY_UP = 38, KEY_RIGHT = 39, KEY_DOWN = 40,
       KEY_DEL = 46, KEY_F2 = 113, KEY_F4 = 115, KEY_F5 = 116 };
enum { M_SHIFT = 1, M_CTRL = 2, M_ALT = 4 };
// pointer shapes, GUI_PTR_*'s numbers
enum { PTR_ARROW = 0, PTR_TEXT = 1, PTR_RESIZE = 2 };
// 16x16 icons (d_icon), which a class names by its kind; the numbers are
// the C4IX desktop's window kinds
enum { ICON_TERM = 1, ICON_INFO = 2, ICON_FILES = 3, ICON_TASKS = 4, ICON_NOTE = 5,
       ICON_CALC = 6, ICON_MSG = 7, ICON_INPUT = 8, ICON_EDIT = 9, ICON_SETTINGS = 10,
       ICON_CLOCK = 11, ICON_FILEBOX = 12 };          // (the dialogs are drawn with none)

// ---- classes ----------------------------------------------------------------
//
// What a window's code sees. Every hook may be 0, and the kit checks
// before calling. Coordinates are the client area's.
struct wk_class {
    int size;                         // bytes of per-window state, zeroed (set at run time)
    int kind;                         // the shell's number for the class, and its icon
    int pointer_shape;                // PTR_* over the client area, unless pointer says
    void (*open)(int w, char *arg);   // just opened (wk_open's arg)
    void (*draw)(int w, int cw, int ch, int focused);
    void (*mouse)(int w, int ev, int x, int y, int button);   // E_WHEEL: button is the steps
    void (*key)(int w, int down, int code, int ch, int mods);
    void (*command)(int w, int id);                           // a menu item: WK_CMD(menu, item)
    void (*timer)(int w, int id);
    void (*answer)(int w, int dialog, int result);            // a dialog it asked closed
    int  (*close)(int w);             // asked to close (its close box, Alt+F4): 0 vetoes
    void (*destroy)(int w);           // closing: free what it holds
    int  (*pointer)(int w, int x, int y);                     // PTR_* there
    void (*idle)(int w);              // every pass of the loop (a pipe to read)
    void (*drawn)(int w);             // a frame with it in has been shown
};

// a menu bar: titles, and for each an array of items ending in 0 ("-" a
// separator); an item chosen reaches command as WK_CMD(menu, item)
struct wk_menu { char *title; char **items; };
enum { WK_CMD_STEP = 256 };
#define WK_CMD(m, i) ((m) * WK_CMD_STEP + (i))

// wk_open's flags
enum { WK_FIXED = 1,                  // not resizable
       WK_DIALOG = 2,                 // a dialog's chrome (no icon, no minimise); modal
       WK_CENTER = 4 };               // centred on the screen, a little high
// message box buttons, and what an answer's result means
enum { WK_OK = 0, WK_YESNO = 1, WK_OKCANCEL = 2 };
enum { WK_NO = 0, WK_YES = 1 };       // also Cancel and OK

// ---- windows ----------------------------------------------------------------
int   wk_open(struct wk_class *c, char *title, int w, int h, int flags, char *arg);  // -1: no memory
void  wk_close(int w);                // at once (destroy), whatever close would say
void  wk_ask_close(int w);            // as the close box does: close may veto
void *wk_state(int w);                // its class's state, or 0 for a window gone
int   wk_alive(int w);
void  wk_title(int w, char *s);
char *wk_get_title(int w);
void  wk_invalidate(int w, int x, int y, int ww, int hh);   // ww = 0: all of it
void  wk_menu(int w, struct wk_menu *m, int n);
int   wk_timer(int w, int id, int ms);                      // every ms (0 stops it); 1 if set
void  wk_raise(int w);
int   wk_focus();                     // the window with the keyboard, or 0
void  wk_size(int w, int *cw, int *ch);                     // its client area now
void  wk_place(int w, int x, int y, int ww, int hh);        // the whole window's rectangle
int   wk_find(struct wk_class *c);    // the first window of the class, or 0
int   wk_next(int w);                 // the windows in the order they opened: 0 starts, 0 ends
struct wk_class *wk_class_of(int w);
int   wk_modal();                     // a dialog holding the input, or 0
void  wk_redraw_all();                // the whole scene drawn again
int   wk_now();                       // milliseconds of machine time

// dialogs: the answer reaches the owner's class, or the shell's for 0
void  wk_msgbox(int owner, int dialog, char *title, char *text, int buttons);
void  wk_inputbox(int owner, int dialog, char *title, char *prompt, char *buf, int max);
void  wk_filebox(int owner, int dialog, int save, char *pattern, char *buf, int max);
                                      // buf: the start folder or name in, the path chosen out

// the clipboard: text, copied in
int   wk_clip_set(char *s, int n);    // 1 if it took it
char *wk_clip_get(int *n);

// ---- drawing (draw.c), only inside draw(): client-relative and clipped -----
void d_rect(int x, int y, int w, int h, int rgb);
void d_recto(int x, int y, int w, int h, int rgb);
void d_line(int x0, int y0, int x1, int y1, int rgb);
void d_disc(int x, int y, int r, int rgb);
void d_text(int x, int y, int rgb, int font, char *s);
void d_textn(int x, int y, int rgb, int size, int font, int advance, char *s, int n);
void d_bevel(int x, int y, int w, int h, int style);
void d_icon(int kind, int x, int y);
void d_clip(int x, int y, int w, int h);  // narrower than the client area, until d_clip_end
void d_clip_end();
void d_imgdef(int id, int w, int h, int *px);
void d_image(int id, int x, int y);
// w x h 8-bit pixels at src through 256 colours at pal, scaled to dw x dh
void d_blit8(int x, int y, int w, int h, char *src, int pitch, int *pal, int dw, int dh);
int  ui_mix(int a, int b, int t, int n);   // a colour T/N of the way from A to B

// colour schemes (draw.c)
int   wk_scheme_count();
char *wk_scheme_name(int i);
void  wk_scheme_use(int i, int desk);      // desk: the desktop's colour, or -1 for the scheme's

// ---- widgets (ui.c): stateless, so the window owns the state --------------
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
int  ui_field_key(char *text, int max, int *caret, int code, int ch, int mods);
int  ui_field_hit(int px, int x, int w, char *text, int caret);   // the caret for a click
void ui_tabs(int x, int y, char **labels, int n, int sel);
int  ui_tab_hit(int px, int py, int x, int y, char **labels, int n);
void ui_group(int x, int y, int w, int h, char *label);
void ui_radio(int x, int y, char *label, int on);
void ui_header(int x, int y, int *widths, char **labels, int n);
char *ui_num(char *buf, int v);                // decimal
char *ui_num_commas(char *buf, int v);         // 1,234,567
int  ui_len(char *s);
void ui_cpy(char *d, char *s);
void ui_cat(char *d, char *s);
int  ui_ncpy(char *d, char *s, int n);         // at most n - 1 bytes and a 0; 1 if all of s fit
int  ui_ncat(char *d, char *s, int n);         // ... onto the end of d, n its whole size
int  ui_join(char *out, char *dir, char *name, int n);   // dir/name, bounded; 1 if it fit
char *ui_base(char *path);                     // what follows the last '/'
int  ui_eq(char *a, char *b);
int  ui_ends(char *s, char *suffix);

// ---- the shell -------------------------------------------------------------
//
// What the shell draws and handles itself. Every hook may be 0.
struct wk_shell {
    void (*desktop)(int x, int y, int w, int h);  // under the windows, in this rectangle
    void (*front)();                  // over the windows (a taskbar, a menu of its own)
    int  (*popup)();                  // 1 while it has a menu open over everything
    int  (*mouse_front)(int ev, int x, int y, int button);  // before the windows: 1 took it
    void (*mouse_desk)(int ev, int x, int y, int button);   // where no window is
    int  (*key)(int down, int code, int ch, int mods);      // before the focused window: 1 took it
    int  (*pointer)(int x, int y);    // PTR_* there, or -1 for the kit's
    void (*answer)(int dialog, int result);                 // a dialog with no owner
    void (*resized)();                // the screen changed size: set the area, relayout
};

void wk_init(struct wk_shell *s);     // after gui_attach: the screen's size from the display
void wk_event(int *ev);               // one event from gui_poll
void wk_tick();                       // every pass: sweep closed windows, idle hooks, timers
int  wk_redraw(int force);            // draw what needs it (all of it if force); 1 if it drew
int  wk_dirty();                      // 1 if anything waits to be drawn
int  wk_next_timer();                 // ms until a timer is due, or -1 for none
void wk_drawn();                      // after the frame was shown: drawn hooks
void wk_repoint();                    // the display's pointer is an arrow again (it was away)
void wk_screen(int w, int h);         // ask for a new screen size (gui_size) and relayout
void wk_set_area(int x, int y, int w, int h);
void wk_close_all();                  // every window, at once (shutting down)
void wk_min_icons(int on);            // minimised windows as icons along the bottom of the
                                      // area (Windows 3.1's), not only hidden
void wk_minimize(int w);
void wk_arrange_icons();              // the icons packed along the bottom again, in order
void wk_dirty_rect(int x, int y, int w, int h);   // screen coordinates
int  wk_in_damage(int x, int y, int w, int h);    // while drawing: 1 if this is being redrawn
void wk_clip_screen(int x, int y, int w, int h);  // the shell's own clip, inside the damage
void wk_noclip();

// ---- the host: what the shell provides the kit (desktop/host.c, grin's own)
char *wk_alloc(int n);                // zeroed, or 0
void  wk_free(char *p);
int   wk_fopen(char *path, int write);   // to read, or created/truncated to write; -1 if not
int   wk_fread(int fd, char *buf, int n);
int   wk_fwrite(int fd, char *buf, int n);
void  wk_fclose(int fd);
int   wk_readdir(char *dir, int i, char *name);   // entry i: 1 a folder, 0 a file, -1 no more
                                                  // (name holds WK_NAME bytes)

// ---- the accessories --------------------------------------------------------
extern struct wk_class calc_class;
extern struct wk_class notepad_class;
extern struct wk_class clock_class;
int  calc_open();
int  notepad_open(char *path);        // 0: a new document
int  clock_open();
extern char *notepad_name;            // in its titles ("Notepad"; the desktop's "Text Editor")
extern char *notepad_about;           // its Help > About line
extern char *notepad_home;            // where Open starts
extern char *notepad_untitled;        // where Save As offers to put a new document
extern int notepad_filebox;           // 1: Open and Save As are file boxes (grin's); 0: typed paths
int  wk_clock();                      // the host's time of day, in seconds since midnight

// ---- screen savers (savers.c) ----------------------------------------------
enum { SAVER_MS = 33 };
int  saver_count();
char *saver_name(int i);
int  saver_active();
void saver_start(int which);
void saver_frame();
void saver_stop();
int  saver_frames();

#endif // WINKIT_H
