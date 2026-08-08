// c4or1k test harness AND console-mode entry point. Loads a flat
// binary of big-endian 32-bit words (produced by tools/asm.py's "bin"
// mode) into guest RAM at address 0, runs it via the same
// cpu_step(halt_pc) loop either way:
//
//   - test mode (default): the program is straight-line and falls
//     off the end at a known word count; every register plus SR_F/
//     SR_CY/SR_OV is dumped in a format tools/or1k-oracle.js matches
//     line-for-line, so the two can be diffed directly.
//   - console mode (-r): the program (tests/m3_echo.s, for now) polls
//     the UART and loops until it decides to halt itself (Ctrl-D);
//     no dump at the end, since there's no oracle-comparable state
//     for an interactive session, and stdin is polled every
//     iteration via con_poll_and_feed() (cheaply, thanks to its own
//     internal rate gate -- see con.c).
//
// M0's throughput result (main.c's earlier, single-file content) is
// preserved in docs/c4or1k-design.md and README.md.

#include "cpu.h"
#include "mem.h"
#include "uart.h"
#include "con.h"

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
    int nwords, status, steps, t0, t1, dt_ms, ips, run_mode;

    path = argc > 1 ? argv[1] : "src/c4or1k/tests/m1_test.bin";
    run_mode = (argc > 2 && argv[2][0] == '-' && argv[2][1] == 'r');

    mem_init();
    cpu_reset();
    uart_reset();
    con_init();
    nwords = load_program(path);
    if (nwords < 0) return 1;

    steps = 0;
    t0 = __time();
    while (1) {
        con_poll_and_feed();
        status = cpu_step(nwords);
        if (status != 0) break;
        ++steps;
    }
    t1 = __time();

    if (status == 2) {
        printf("FAULT after %d instructions\n", steps);
        return 1;
    }

    if (run_mode) return 0; // no dump for an interactive/piped console session

    dt_ms = t1 - t0;
    printf("c4or1k: ran %d instructions from %s in %d ms\n", steps, path, dt_ms);
    if (dt_ms > 0) {
        ips = (steps * 1000) / dt_ms;
        printf("  guest instructions/sec: %d\n", ips);
    }

    cpu_dump();
    ram_dump(0x1000, 64); // covers every offset tests/m1_test.s / tests/m2_test.s write (see their header comments)
    return 0;
}
