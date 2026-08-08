// c4or1k test harness, console-mode, AND boot-mode entry point. All
// three drive the same cpu_step(halt_pc) loop:
//
//   - test mode (default): loads a flat binary of big-endian 32-bit
//     words (tools/asm.py's "bin" mode) at guest address 0. The
//     program is straight-line and falls off the end at a known word
//     count; every register plus SR_F/SR_CY/SR_OV is dumped in a
//     format tools/or1k-oracle.js matches line-for-line, so the two
//     can be diffed directly.
//   - console mode (-r): same loader, but the program (tests/
//     m3_echo*.s) polls the UART and loops until it decides to halt
//     itself (Ctrl-D); no dump at the end, since there's no
//     oracle-comparable state for an interactive session.
//   - boot mode (-b [maxsteps]): loads a raw kernel image via
//     boot.c's load_kernel/patch_kernel instead, starts execution at
//     the real OR1000 reset vector (0x100) instead of address 0, and
//     runs with no natural halt address (nwords = -1, so cpu_step's
//     `pc == halt_pc` check never fires) -- a kernel doesn't fall off
//     the end of its own image. maxsteps (0 = unbounded) exists
//     because there's nothing else to bound it with yet: no panic
//     detection, just a instruction-count cutoff for observing how
//     far a boot attempt gets.
//
// stdin is polled every loop iteration in all three modes via
// con_poll_and_feed() -- cheaply, thanks to its own internal rate
// gate (con.c) -- since test-mode programs never touch the UART and
// the poll is a no-op for them regardless.
//
// M0's throughput result (main.c's earlier, single-file content) is
// preserved in docs/c4or1k-design.md and README.md.

#include "cpu.h"
#include "mem.h"
#include "uart.h"
#include "con.h"
#include "boot.h"
#include "virtio.h"
#include "virtio9p.h"
#include "bootfs.h"

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

// c4 has no atoi/library string routines.
int str_to_int(char *s) {
    int n;
    n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        ++s;
    }
    return n;
}

int main(int argc, char **argv) {
    char *path, *bootfs_idx_path, *bootfs_blob_path;
    int nwords, status, steps, t0, t1, dt_ms, ips, run_mode, boot_mode, maxsteps, length;

    path = argc > 1 ? argv[1] : "src/c4or1k/tests/m1_test.bin";
    run_mode = (argc > 2 && argv[2][0] == '-' && argv[2][1] == 'r');
    boot_mode = (argc > 2 && argv[2][0] == '-' && argv[2][1] == 'b');
    maxsteps = (boot_mode && argc > 3) ? str_to_int(argv[3]) : 0; // 0 = unbounded
    bootfs_idx_path = (boot_mode && argc > 4) ? argv[4] : "src/c4or1k/images/bootfs.idx";
    bootfs_blob_path = (boot_mode && argc > 5) ? argv[5] : "src/c4or1k/images/bootfs.blob";

    mem_init();
    cpu_reset();
    uart_reset();
    con_init();

    if (boot_mode) {
        virtio_reset();
        virtio9p_init();
        bootfs_init(bootfs_idx_path, bootfs_blob_path);

        length = load_kernel(path);
        if (length < 0) return 1;
        patch_kernel(length, RAM_SIZE / 0x100000);
        pc = 0x100 >> 2; nextpc = pc + 1; // real OR1000 reset vector, not address 0
        nwords = -1; // no natural halt address for a kernel image
        printf("c4or1k: booting %s (%d bytes) from the reset vector\n", path, length);
    } else {
        nwords = load_program(path);
        if (nwords < 0) return 1;
    }

    steps = 0;
    t0 = __time();
    while (1) {
        con_poll_and_feed();
        if (!(steps & 63)) cpu_tick_check(64); // cadence matches safecpu.js's Step loop, see cpu.h
        status = cpu_step(nwords);
        if (status != 0) break;
        ++steps;
        if (boot_mode && maxsteps && steps >= maxsteps) {
            printf("c4or1k: stopped after %d instructions (-b limit)\n", steps);
            break;
        }
    }
    t1 = __time();

    if (status == 2) {
        printf("FAULT after %d instructions\n", steps);
        return 1;
    }

    if (run_mode || boot_mode) return 0; // no register/RAM dump: neither has oracle-comparable final state

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
