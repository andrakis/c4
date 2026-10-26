// shell.h -- inside grin, C4DOS's graphical shell (docs/grin-plan.md):
// what its modules share. (grin.h is the applications' header.) grin is built on the window kit (src/winkit),
// and as a shell it looks inside the kit's windows (wkwin.h) to cascade,
// tile and list them.

#ifndef GRIN_SHELL_H
#define GRIN_SHELL_H 1
#include "wkwin.h"

// GRIN.INI (grin.c): the screen, the colours, the desktop's pattern and
// the screen saver. Written where C4DOS writes a file with no drive: the
// home drive (DRIVES.SYS HOME=B:), which the page keeps, else the RAM
// disk.
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
int  grin_start(char *cmdline);       // ... a command line: a built-in window, or an application
int  filemgr_open();                  // filemgr.c: File Manager
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
                                      // (a DOS program it finds goes to grin_full)
int  grin_heapgen();                  // grin's own heap generation (0 without HEAP.SYS)

// DOS programs (docs/grin-plan.md 3.6, 3.7): one at a time, run from the
// top of the loop, full screen (grin.c) or in a DOS Prompt (dosprmpt.c)
extern int  dos_busy;                 // one is running
extern char dos_busy_name[64];        // its name, in capitals
void dos_busy_set(char *cmdline);
int  dos_refuse();                    // one is running or waiting: a message box says so, and 1
int  grin_full(char *cmdline);        // full screen, at the top of the loop: 0, or -1 (refused)
int  grin_full_waiting();
int  dosprompt_open(char *cmd);       // a DOS Prompt window; cmd (or 0) typed at its first prompt
void dosprompt_top();                 // the top of the loop: a DOS Prompt's line runs
void grin_pump();                     // one pass of the loop, for a DOS program waiting in grin
void grin_draw_now();                 // the damage drawn now (a DOS Prompt's output, as it comes)

// a 32x32 picture with a label under it, centred on cx (Program Manager's
// items, Control Panel's), drawn client-relative; and its hit box
enum { ITEM_W = 76, ITEM_H = 60 };
void grin_item(int cx, int y, int pic, char *label, int sel);
int  grin_item_hit(int px, int py, int cx, int y);
enum { PIC_CONTROL = 1, PIC_DISPLAY = 2, PIC_COLORS = 3, PIC_DESKTOP = 4, PIC_CLOCK = 5, PIC_CALC = 6,
       PIC_FILES = 7, PIC_DOS = 8, PIC_NOTEPAD = 9, PIC_GAME = 10, PIC_APP = 11, N_PICS = 12 };
void grin_item_dim(int cx, int y, int pic, char *label, int sel, int dim);   // dim: greyed, not available
void grin_picture(int pic, int x, int y);

// C4DOS's files, as grin sees them (disk.c)
enum { DRV_RAM = 26 };                // the RAM disk, where a drive's number would be
int   gd_drives();                    // how many drives (DRIVES.SYS), 1 without
int   gd_home();                      // where a file with no drive is written, or -1: the RAM disk
int   gd_unlink(char *name);          // 0, or -1
int   gd_exec(char *line);            // a command line, as A> runs it: its status
int   gd_sink(int w, int r, int ctx); // the console's sink (slot 24): w(ctx, buf, n), r(ctx, buf, n); 0s give it back
int   gd_setgen(int g);               // the heap generation now: the previous one, or -1
int   gd_drive();                     // the drive DOS's prompt is on, 0 = A: (v6)
int   gd_size(char *name);            // bytes, read through; -1 if it is not there
int   gd_list(int drive, char *names, int max);   // a drive's files, WK_NAME bytes each: how many
char *gd_name(int drive, char *name, char *out, int n);   // "B:NAME", "RAM:NAME"

// grin's dialogs: windows of the kit's dialog chrome (WK_DIALOG), whose
// state begins as the kit's own dialogs' does, with no buffer, so that
// closing one from its control menu or with Esc (wk_dialog_answer) is
// safe
struct grin_dlg { char *buf; int max; };
int grin_dialog(struct wk_class *c, int size, char *title, int w, int h);

#endif
