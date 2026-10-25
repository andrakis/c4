// shell.h -- inside grin, C4DOS's graphical shell (docs/grin-plan.md):
// what its modules share. (grin.h is the applications' header.) grin is built on the window kit (src/winkit),
// and as a shell it looks inside the kit's windows (wkwin.h) to cascade,
// tile and list them.

#ifndef GRIN_SHELL_H
#define GRIN_SHELL_H 1
#include "wkwin.h"

// GRIN.INI (grin.c): the screen, the colours, the desktop's pattern and
// the screen saver. Kept on the RAM disk for now: C4DOS gains a drive
// that the page keeps (HOME=B:) in G4.
struct grin_ini {
    int sw, sh;                       // 640x480 or 800x600
    int scheme;                       // the kit's colour scheme
    int pattern;                      // the desktop's (pattern_name), 0 none
    int saver, wait;                  // the screen saver (savers.c, 0 none), after how many minutes
};
extern struct grin_ini ini;
void ini_save();

// the desktop under the windows (grin.c)
int   pattern_count();
char *pattern_name(int i);
void  grin_set_scheme(int scheme);
void  grin_set_pattern(int pattern);
void  grin_set_screen(int sw, int sh);
void  grin_saver_test(int which);     // the saver now, past the click that asked
void  grin_quit();                    // the session ends: the loop stops

// the built-in windows
int  progman_open();                  // progman.c: Program Manager
int  tasklist_open();                 // tasklist.c: the Task List
void tasklist_cascade();
void tasklist_tile();
int  control_open();                  // control.c: Control Panel

// the applications (apps.c): the kit's table in DOS's slot 25, and the
// programs started as applications
void apps_init();                     // at the start: the table published
void apps_reap();                     // every pass, at the top of the loop: the dead and windowless unloaded
void apps_end();                      // at the end, after every window: all unloaded, the table taken back
int  apps_count();                    // how many are loaded
int  grin_run(char *cmdline);         // start one: 0, or -1 with a message box saying why

// a 32x32 picture with a label under it, centred on cx (Program Manager's
// items, Control Panel's), drawn client-relative; and its hit box
enum { ITEM_W = 76, ITEM_H = 60 };
void grin_item(int cx, int y, int pic, char *label, int sel);
int  grin_item_hit(int px, int py, int cx, int y);
enum { PIC_CONTROL = 1, PIC_DISPLAY = 2, PIC_COLORS = 3, PIC_DESKTOP = 4, PIC_CLOCK = 5, PIC_CALC = 6 };

// grin's dialogs: windows of the kit's dialog chrome (WK_DIALOG), whose
// state begins as the kit's own dialogs' does, with no buffer, so that
// closing one from its control menu or with Esc (wk_dialog_answer) is
// safe
struct grin_dlg { char *buf; int max; };
int grin_dialog(struct wk_class *c, int size, char *title, int w, int h);

#endif
