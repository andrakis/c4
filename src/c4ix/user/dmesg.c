// C4IX userland: dmesg -- print the kernel's log.
//
// The log is the kernel's ring of every line it has said since it
// booted, each stamped with the machine's time (console.c), and
// /proc/kmsg is that ring as a file, made when it is read. Unlike
// Linux's /proc/kmsg, reading it takes nothing out.
//
//   dmesg        the whole log
//   dmesg -t     without the times

#include "c4ix_user.h"

enum { DMESG_BUF = 4096 };

int main(int argc, char **argv) {
    char buf[DMESG_BUF];
    int fd, n, i, k, notime, mid, skip;

    notime = argc > 1 && argv[1][0] == '-' && argv[1][1] == 't' && !argv[1][2];
    if (argc > 1 && !notime) {
        ufprintf(STDERR, "usage: dmesg [-t]\n");
        return 2;
    }
    if ((fd = uopen("/proc/kmsg", O_RD)) < 0) {
        ufprintf(STDERR, "dmesg: cannot open /proc/kmsg\n");
        return 1;
    }
    mid = 0;
    skip = 0;
    while ((n = uread(fd, buf, DMESG_BUF)) > 0) {
        if (!notime) { write(STDOUT, buf, n); continue; }
        // -t: each line from after its "] "
        k = 0;
        i = 0;
        while (i < n) {
            if (!mid) { mid = 1; skip = buf[i] == '['; }
            if (skip) {
                if (buf[i] == ']') { skip = 2; }
                else if (skip == 2 && buf[i] == ' ') { skip = 0; }
                ++i;
                continue;
            }
            buf[k] = buf[i];
            ++k;
            if (buf[i] == '\n') mid = 0;
            ++i;
        }
        if (k) write(STDOUT, buf, k);
    }
    uclose(fd);
    return 0;
}
