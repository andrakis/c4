// wkwin.h -- inside the window kit: the window record, and what its
// modules share. A shell may include it to look at its windows (C4IX's
// desktop does, for the tools written before the kit); an application
// never does.

#ifndef WKWIN_H
#define WKWIN_H 1
#include "winkit.h"

// Each window is its own allocation, so there are as many as memory
// allows. Its number is never reused, so a number kept for a window that
// has since closed finds nothing. A window closed is off the z-order at
// once and freed at the next wk_tick (sweep): whoever closed it, or is
// walking the list, may still be holding it.
enum { WK_TIMERS = 4 };
struct win {
    int id;                       // its number (1 up)
    struct win *next;             // the next window, in the order they opened
    struct win *above, *below;    // z-order
    struct wk_class *cls;
    int slot;                     // where it cascaded to when it opened
    int used, kind, x, y, w, h;   // kind: its class's
    int min, max, rx, ry, rw, rh;
    int resizable, dialog;
    char title[48];
    char *st;                     // its class's state: cls->size bytes
    struct wk_menu *menus;        // its menu bar, nmenus of them
    int nmenus;
    int owner, action;            // a dialog: which window asked, and what for
    int tid[WK_TIMERS], tms[WK_TIMERS], tnext[WK_TIMERS];   // timers: id, period, when due
};

struct win *win_get(int i);       // the window with this number, or 0
int  win_index(struct win *w);    // ... and a window's number (-1 for none)
struct win *win_first();          // the windows in the order they opened (closed ones still on it, used 0)

// the client area of a window, on the screen
int  wk_client_x(struct win *w);
int  wk_client_y(struct win *w);
int  wk_client_w(struct win *w);
int  wk_client_h(struct win *w);

// ---- draw.c's state, which wmcore.c and the looks drive ----
extern int wk_full;               // the whole scene is to be drawn
extern int wk_part;               // drawing only the damage
extern int wk_dmg, wk_dmg_x0, wk_dmg_y0, wk_dmg_x1, wk_dmg_y1;
void wk_origin(int x, int y);     // where d_* draw from (a client area, or 0, 0)
void wk_client_clip(int x, int y, int w, int h);   // the client area d_clip keeps inside

// ---- the looks (look95.c, look31.c) ----
// What a point on a window is: the chrome's parts, or the client area.
enum { HT_NONE = 0, HT_CLIENT = 1, HT_CAPTION = 2, HT_CLOSE = 3, HT_MAX = 4, HT_MIN = 5,
       HT_SYSMENU = 6, HT_MENUBAR = 7, HT_SIZE = 8 };
// HT_SIZE + edges: which edges a drag at that point moves
enum { SZ_L = 1, SZ_R = 2, SZ_T = 4, SZ_B = 8 };
struct wk_look {
    void (*frame)(struct win *w, int focused);         // the chrome, all but the menu bar
    void (*menubar)(struct win *w, int focused, int open, int hot);   // open: the menu shown, or -1
    int  (*hit)(struct win *w, int x, int y);          // HT_*, or HT_SIZE + SZ_* bits
    void (*grip)(struct win *w);                       // drawn over the client area's corner
    int  title_centred;
};
extern struct wk_look look_flat, look_classic;
struct wk_look *wk_look();        // the one in use (wk.style)
void wk_gradient(int x, int y, int w, int h, int a, int b);
int  wk_ink(int bg);              // white on a dark bar, the scheme's dark on a light one
int  wk_menu_x(struct win *w, int k);   // where menu k of a menu bar starts on the screen
int  wk_menu_w(struct win *w, int k);

// ---- dialog.c, for wmcore.c ----
void wk_dialog_answer(struct win *d, int yes);   // close a dialog with this answer
extern struct wk_class wk_msg_class;
extern struct wk_class wk_input_class;
extern struct wk_class wk_file_class;
void wk_answer_owner(int owner, int dialog, int result);   // wmcore.c: to the window, or the shell
#endif
