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

// What a TASK is told. host_info() was read at boot, before the kernel
// installed its trap handler, so it lacks C4I_TRAPH (0x400) -- and that
// bit is the machine's promise that probing a custom opcode is safe,
// which for a task under C4IX it is: every one of them lands in this
// kernel. C4KE's tools test it before asking for OP_VFS_PUT, so without
// it c4cc could not write what it compiled. Everything else verbatim.
int task_info() {
    return host_info() | 0x400;
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
