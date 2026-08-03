//
// C4IX host detection.
//
// One INFO opcode tells the kernel what it is standing on: plain c4
// returns 0 (no traps, no cycle interrupt -- degraded cooperative
// mode), c4m returns its C4I_* feature bits. Everything that must
// differ between the two hosts asks this module instead of probing.
//

#include "c4ix.h"

static int hinfo;
static int detected;

int host_detect() {
    hinfo = __c4_info();
    detected = 1;
    return hinfo;
}

int host_info() {
    if (!detected) host_detect();
    return hinfo;
}

int host_type() {
    return (host_info() & C4IX_I_C4M) ? HOST_C4M : HOST_C4;
}

int host_has(int bit) {
    return (host_info() & bit) == bit;
}

char *host_name() {
    if (host_type() == HOST_C4M) return "c4m";
    return "c4 (degraded)";
}
