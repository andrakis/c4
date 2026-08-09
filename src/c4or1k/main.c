// c4or1k test harness, console-mode, AND boot-mode entry point. All
// three drive the same cpu_run_batch(halt_pc, batch, &ran) loop (see
// cpu.h -- M9 folded what used to be a one-instruction-per-call
// cpu_step into a 64-instructions-per-call batch runner):
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
//     runs with no natural halt address (nwords = -1, so
//     cpu_run_batch's `pc == halt_pc` check never fires) -- a kernel
//     doesn't fall off the end of its own image. maxsteps (0 =
//     unbounded) exists because there's nothing else to bound it with
//     yet: no panic detection, just a instruction-count cutoff for
//     observing how far a boot attempt gets.
//
// stdin is polled once per batch (every 64 instructions) in all three
// modes via con_poll_and_feed() -- cheaply, thanks to its own internal
// rate gate (console.c) -- since test-mode programs never touch the
// UART and the poll is a no-op for them regardless.
//
// M0's throughput result (main.c's earlier, single-file content) is
// preserved in docs/c4or1k-design.md and README.md.

#include "cpu.h"
#include "mem.h"
#include "uart.h"
#include "console.h"
#include "boot.h"
#include "virtio.h"
#include "virtio9p.h"
#include "bootfs.h"
#include "jit.h"
#include "eth.h"
#include "net.h"

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
    int nwords, status, steps, t0, t1, dt_ms, ips, run_mode, boot_mode, maxsteps, length, batch, ran, jit_enable, ai;

    path = argc > 1 ? argv[1] : "src/c4or1k/tests/m1_test.bin";
    run_mode = (argc > 2 && argv[2][0] == '-' && argv[2][1] == 'r');
    boot_mode = (argc > 2 && argv[2][0] == '-' && argv[2][1] == 'b');
    maxsteps = (boot_mode && argc > 3) ? str_to_int(argv[3]) : 0; // 0 = unbounded
    bootfs_idx_path = (boot_mode && argc > 4) ? argv[4] : "src/c4or1k/images/bootfs.idx";
    bootfs_blob_path = (boot_mode && argc > 5) ? argv[5] : "src/c4or1k/images/bootfs.blob";

    // M13: the JIT build (c4or1k-jit.c4r) runs with translation ON by
    // default -- v2 measured decisively faster (docs, M13) -- with
    // `-nojit` as the opt-out; `-jit` is accepted for explicitness.
    // Both flags go last so they never disturb the positional
    // arguments. The default and -mcisc images compile the hooks out
    // entirely and ignore both.
    jit_enable = 1;
    ai = 1;
    while (ai < argc) {
        if (!memcmp(argv[ai], "-nojit", 7)) jit_enable = 0;
        ++ai;
    }

    mem_init();
#ifdef C4OR1K_JIT
    jit_init(jit_enable); // M13: must precede ANY guest RAM store (load_program/
                          // load_kernel/virtio all bump jit_pagegen via mem.c)
#endif
    cpu_reset();
    uart_reset();
    con_init();

    eth_reset();       // M15: ethmac device + its synthetic-LAN backend.
    net_reset();       // Harmless in test mode (the guest never touches
                       // 0x92000000 there); wired unconditionally so the
                       // device state is always defined.

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

    // M9: cpu_run_batch replaces the old one-instruction-per-call
    // cpu_step, so this loop now runs once per BATCH (64 instructions,
    // matching the cadence M8 already established for
    // con_poll_and_feed/cpu_tick_check -- see cpu.h) rather than once
    // per guest instruction. That collapses this loop's own iteration
    // overhead (status check, step counting, maxsteps check) and the
    // cpu_step call/return itself down to once per 64 instructions
    // instead of once per instruction. batch is shortened only for
    // the final iteration under a maxsteps cutoff, so "stopped after
    // N instructions (-b limit)" still reports exactly N, never an
    // overshoot past the requested budget.
    steps = 0;
    t0 = __time();
    while (1) {
        con_poll_and_feed();
        net_poll();       // M15: drain queued ethernet replies + reconcile
                          // the eth IRQ line here (between instructions),
                          // never from inside a guest store -- see net.h.
        cpu_tick_check(64);
        batch = 64;
        if (boot_mode && maxsteps && (maxsteps - steps) < batch) batch = maxsteps - steps;
        status = cpu_run_batch(nwords, batch, &ran);
        steps = steps + ran;
        if (status != 0) break;
        if (boot_mode && maxsteps && steps >= maxsteps) {
            printf("c4or1k: stopped after %d instructions (-b limit)\n", steps);
#ifdef C4OR1K_JIT
            if (jit_enable) printf("c4or1k: jit ran %d blocks covering %d instructions\n", jit_nblocks, jit_ninstr);
#endif
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
