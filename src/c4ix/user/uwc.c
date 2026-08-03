// C4IX userland: read fd 0 to end of file and report the counts,
// like wc. Reading an empty pipe whose writer is still alive blocks
// in the kernel; the syscall is restarted when data arrives.

#include "c4ix_user.h"

int main(int argc, char **argv) {
    char buf[128];
    int n, i, bytes, lines;

    bytes = 0;
    lines = 0;
    while ((n = uread(STDIN, buf, 128)) > 0) {
        bytes = bytes + n;
        i = 0;
        while (i < n) {
            if (buf[i] == '\n') ++lines;
            ++i;
        }
    }
    uprintf("uwc: %d bytes %d lines\n", bytes, lines);
    return 0;
}
