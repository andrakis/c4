// C4IX userland: shutdown -- the machine off, or restarted.
//
//   shutdown        every program ended, every disk written and unmounted
//                   clean, then the machine off (a PC's ACPI power-off:
//                   VMware's, QEMU's, a real one's)
//   shutdown -r     ... and restarted instead (reboot.c says the same)
//
// The kernel's SYS_SHUTDOWN (sched.c, sched_halt): programs get SIGTERM,
// and what is still there 3 seconds later is killed; then the kernel
// task's end unmounts the disks (boot.c). The machine's power button does
// the same (c4pc's pc.c hands it to the kernel as SIGTERM).

#include "c4ix_user.h"

enum { SYS_SHUTDOWN = 254 };

static int ksys(int num, int a, int b, int c) { return __c4_opcode(c, b, a, num); }

int main(int argc, char **argv) {
    int how, k;
    how = 0;
    k = 1;
    while (k < argc) {
        if (argv[k][0] == '-' && argv[k][1] == 'r' && !argv[k][2]) how = 1;
        else if (argv[k][0] == '-' && (argv[k][1] == 'h' || argv[k][1] == 'P') && !argv[k][2]) how = 0;
        else { ufprintf(STDERR, "usage: shutdown [-r]   (-r: restart the machine instead)\n"); return 1; }
        ++k;
    }
    if (ksys(SYS_SHUTDOWN, how, 0, 0) < 0) { ufprintf(STDERR, "shutdown: already shutting down\n"); return 1; }
    return 0;
}
