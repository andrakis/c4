// exitkey.c -- EXITKEY.C4R, a test application (libjs/tests/test-grin.mjs,
// docs/grin-plan.md G3): a window whose key callback calls exit(). grin
// catches the exit, closes the window and unloads the image, with the
// memory main took for itself, and carries on.

#include <stdio.h>
#include <stdlib.h>
#include "winkit.h"
#include "grin.h"

enum { HOLD = 100000 };
static char *hold;                    // memory of its own, which the unload gives back

static void ek_draw(int w, int cw, int ch, int focused) {
    d_rect(0, 0, cw, ch, C_WHITE);
    d_text(10, 12, C_DARK, F_SANS, "A key here calls exit(3).");
}

static void ek_key(int w, int down, int code, int ch, int mods) {
    if (down) exit(3);
}

static struct wk_class ek_class = { .draw = ek_draw, .key = ek_key };

int main(int argc, char **argv) {
    if (!grin_present()) { printf("EXITKEY is a test application for grin: run it from grin's File > Run.\n"); return 1; }
    if (!hold && (hold = (char *)malloc(HOLD))) hold[HOLD - 1] = 1;
    ek_class.size = 16;
    if (wk_open(&ek_class, "Exit Test", 260, 110, 0, 0) < 0) return 1;
    return GRIN_RESIDENT;
}
