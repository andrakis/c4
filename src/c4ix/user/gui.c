// C4IX userland: gui. The libjs display demo, run as a C4IX task.
//
// The program is libjs/guest/gui-demo.c, unchanged; this file only maps
// the three host calls it makes onto libc4ix. printf becomes a write to
// fd 1, so it goes wherever the shell pointed it; memory comes from the
// kernel; and the frame delay is a real sleep, so the task parks on the
// clock and the shell (and everyone else) runs between frames.
//
// The display registers are plain loads and stores, and protected mode
// gates syscall opcodes only, so a user task reaches the device with no
// help from the kernel. INFO is gated, and the kernel answers it for the
// task (sys.c), which is how gui_present() sees C4I_GUI.
//
// Run from the shell: `gui` (or `gui &` to keep the prompt), or `gui N`
// for N frames. On a host without the display it prints "gui: not
// fitted" and exits.
//
// The display is the desktop's while it runs, so the demo asks the kernel
// for it first (include/display.h): the desktop gives way, and has it
// back, and draws again, when the demo detaches -- or dies.

#include "c4ix_user.h"

#define printf uprintf
#define malloc ualloc
#define __c4_usleep(us) umsleep((us) / 1000)

#include "libjs/guest/gui.h"

// gui-demo.c is c4cc's too, which has no preprocessor: the taking and the
// giving back go round its attach and detach here
static int gui_take(int *region, int bytes, int w, int h) {
    if (!udisplay_take(0)) { uprintf("gui: a program has the display\n"); return 0; }
    if (gui_attach(region, bytes, w, h)) return 1;
    udisplay_give();
    return 0;
}
static void gui_give() { gui_detach(); udisplay_give(); }
#define gui_attach gui_take
#define gui_detach gui_give

#include "libjs/guest/gui-demo.c"
