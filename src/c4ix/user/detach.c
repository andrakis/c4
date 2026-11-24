// C4IX userland: detach -- in a machine of the hypervisor's (c4pc --hv), the
// screen, keyboard and mouse back to the control machine's (VM 0), as
// Ctrl+Alt+Home does. docs/c4pc-hypervisor.md.

#include "c4ix_user.h"

enum { R_A0 = 0x564, R_CMD = 0x574, R_COUNT = 0x57c, C_DETACH = 8, C4I_MMU = 0x80000, C4I_HV = 0x400000, C4I_HVG = 0x800000 };

int main(int argc, char **argv) {
    if (__c4_info() & C4I_MMU) uioperm(UIOPERM_DEV);
    if (!(__c4_info() & (C4I_HV | C4I_HVG))) { ufprintf(STDERR, "detach: not a machine of the hypervisor's\n"); return 1; }
    *(int *)R_CMD = C_DETACH;
    return 0;
}
