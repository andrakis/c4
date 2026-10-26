// gkit.h -- the table grin publishes in C4DOS's slot 25 while it runs
// (docs/grin-plan.md 3.4): what an application's import.c finds there.
// grin (apps.c) fills it and import.c reads it, so both include this.

#ifndef GRIN_GKIT_H
#define GRIN_GKIT_H 1

// 'G','R','I','N', written out (c4lc folds it, plain c4 would not), and
// the table's version: a later grin adds functions at the end of
// kitfns.h and a higher number, so an application built against this
// one still finds everything it calls where it expects
enum { GRIN_MAGIC = 1196575054, GRIN_KIT_VERSION = 1 };

struct grin_kit {
    int magic, version;
    int gen;                          // grin's heap generation, which the kit's calls run in (0: no HEAP.SYS)
    struct wk_env *env;               // the kit's wk: the screen, the look and the colours
    void (*hello)();                  // an image's import.c, as it loads: "I am an application"
#define K(r, n, p, a) r (*n) p;
#define V(n, p, a) void (*n) p;
#include "kitfns.h"
#undef K
#undef V
};

#endif
