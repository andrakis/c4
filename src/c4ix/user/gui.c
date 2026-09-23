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
// Run from the shell: `gui` (or `gui &` to keep the prompt). On a host
// without the display it prints "gui: not fitted" and exits.

#include "c4ix_user.h"

#define printf uprintf
#define malloc ualloc
#define __c4_usleep(us) umsleep((us) / 1000)

#include "libjs/guest/gui.h"
#include "libjs/guest/gui-demo.c"
