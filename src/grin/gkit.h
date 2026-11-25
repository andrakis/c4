// gkit.h -- the table grin publishes in C4DOS's slot 25 while it runs
// (docs/grin-plan.md 3.4): what an application's import.c finds there.
// grin (apps.c) fills it and import.c reads it, so both include this.

#ifndef GRIN_GKIT_H
#define GRIN_GKIT_H 1

// 'G','R','I','N', written out (c4lc folds it, plain c4 would not), and
// the table's version: a later grin adds functions at the end of
// kitfns.h and a higher number, so an application built against this
// one still finds everything it calls where it expects. Version 2 (G6)
// put the game window's three entries before the kit's functions, which
// moved them all one way once: every application is built with grin.
// Version 3 (G7) added the clipboard's keys at the end, version 4 the
// fonts (Notepad's Format menu). Version 6 put a fourth game entry,
// fwin_ctl, after the other three, moving the kit's functions one word
// as version 2 did: a game's menu bar, and (fwin_open's flag 4) a window
// of any size, as C4IX's window.h gives them (docs/grin.md 3.5).
enum { GRIN_MAGIC = 1196575054, GRIN_KIT_VERSION = 6 };

struct grin_kit {
    int magic, version;
    int gen;                          // grin's heap generation, which the kit's calls run in (0: no HEAP.SYS)
    struct wk_env *env;               // the kit's wk: the screen, the look and the colours
    void (*hello)();                  // an image's import.c, as it loads: "I am an application"
    // Version 2: a DOS program's frames in a grin window (fwin.c), for the
    // games' bare platform layers, which declare these words themselves
    // and need nothing else of grin's: so they stay here, words 5 to 8,
    // whatever kitfns.h grows to
    int  (*fwin_open)(char *title, int w, int h, int flags);   // 1, or 0: flags 1, the mouse steers; 2, it clicks;
                                      // 4 (v6), any size: the window may be sized and maximised, and each new
                                      // size comes from fwin_poll as FW_EV_SIZE
    void (*fwin_frame)(char *pixels, int w, int h, int pitch, int *pal);   // pal: 256 0x00RRGGBB
    int  (*fwin_poll)(int *ev);       // gui.h's event: 1, 0 none, -1 the window has closed; also
                                      // FW_EV_MENU (100) menu, item and FW_EV_SIZE (101) w, h
    int  (*fwin_ctl)(int op, int n, char *s);   // v6: FWC_MENU (1) starts menu n (0 the bar anew),
                                      // FWC_ITEM (2) adds item s to menu n ("-" a rule, "*x" checked)
#define K(r, n, p, a) r (*n) p;
#define V(n, p, a) void (*n) p;
#include "kitfns.h"
#undef K
#undef V
};

#endif
