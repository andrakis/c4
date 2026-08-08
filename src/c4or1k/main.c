// c4or1k test harness: loads a flat binary of big-endian 32-bit
// words (produced by tools/asm.py's "bin" mode) into guest RAM at
// address 0, runs it as a straight-line (non-looping) program until
// pc falls off the end, and dumps every register plus SR_F/SR_CY/
// SR_OV -- a format tools/or1k-oracle.js matches line-for-line, so
// the two can be diffed directly.
//
// M0's throughput result (main.c's earlier, single-file content) is
// preserved in docs/c4or1k-design.md and README.md; this file now
// serves as the general test-program runner from M1 onward, and
// still reports guest-instructions/sec each run as a cheap ongoing
// check that the full ~100-instruction decode hasn't cratered
// throughput relative to that result.

#include "cpu.h"
#include "mem.h"

enum { FILEBUFSZ = 0x10000 }; // c4lc enum initializers must be a literal, not "1 << 16"
char filebuf[FILEBUFSZ];

// Loads a big-endian word stream into ram[] starting at address 0.
// Returns the word count, or -1 on error.
int load_program(char *path) {
    int fd, n, i, addr, w;

    fd = open(path, 0);
    if (fd < 0) {
        printf("load_program: could not open %s\n", path);
        return -1;
    }
    n = read(fd, filebuf, FILEBUFSZ);
    close(fd);
    if (n <= 0) {
        printf("load_program: read of %s failed\n", path);
        return -1;
    }
    if (n & 3) {
        printf("load_program: %s length %d is not a multiple of 4\n", path, n);
        return -1;
    }

    addr = 0;
    i = 0;
    while (i < n) {
        w = ((filebuf[i] & 0xFF) << 24) | ((filebuf[i + 1] & 0xFF) << 16) |
            ((filebuf[i + 2] & 0xFF) << 8) | (filebuf[i + 3] & 0xFF);
        ram_sw(addr, w);
        addr = addr + 4;
        i = i + 4;
    }
    return addr >> 2;
}

int main(int argc, char **argv) {
    char *path;
    int nwords, status, steps, t0, t1, dt_ms, ips;

    path = argc > 1 ? argv[1] : "src/c4or1k/tests/m1_test.bin";

    mem_init();
    cpu_reset();
    nwords = load_program(path);
    if (nwords < 0) return 1;

    steps = 0;
    t0 = __time();
    while ((status = cpu_step(nwords)) == 0) {
        ++steps;
    }
    t1 = __time();

    if (status == 2) {
        printf("FAULT after %d instructions\n", steps);
        return 1;
    }

    dt_ms = t1 - t0;
    printf("c4or1k: ran %d instructions from %s in %d ms\n", steps, path, dt_ms);
    if (dt_ms > 0) {
        ips = (steps * 1000) / dt_ms;
        printf("  guest instructions/sec: %d\n", ips);
    }

    cpu_dump();
    ram_dump(0x1000, 64); // covers every offset tests/m1_test.s writes (see its header comment)
    return 0;
}
