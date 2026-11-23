// C4IX userland: reboot -- the machine restarted: shutdown -r
// (shutdown.c). Every program ended, every disk written and unmounted
// clean, then the PC reset (c4pc's pc.c; elsewhere the host just ends).

#include "c4ix_user.h"

enum { SYS_SHUTDOWN = 254 };

static int ksys(int num, int a, int b, int c) { return __c4_opcode(c, b, a, num); }

int main(int argc, char **argv) {
    if (argc > 1) { ufprintf(STDERR, "usage: reboot\n"); return 1; }
    if (ksys(SYS_SHUTDOWN, 1, 0, 0) < 0) { ufprintf(STDERR, "reboot: already shutting down\n"); return 1; }
    return 0;
}
