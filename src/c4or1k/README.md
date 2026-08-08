# c4or1k

An OR1000/OpenRISC emulator, capable of booting Linux, written in C
and compiled by c4lc. A port of [jor1k](https://github.com/s-macke/jor1k)
(a JavaScript OR1000 emulator) to the C4 VM toolchain. See
`docs/c4or1k-design.md` for the milestone roadmap and design rationale.

Console only -- no framebuffer, no keyboard device. Terminal raw mode
is handled by `run-c4or1k.sh`, an external wrapper, not by the VM (the
C4 VM has no ioctl/termios facility, see that script's header comment).

## Status: M4 done -- it boots real Linux (M0-M3 preserved in docs/c4or1k-design.md)

    make c4or1k-boot                # boot a real kernel; N=<steps> to change the budget (default 2M)

Reaches `VFS: Cannot open root device "host" ... Kernel panic - not
syncing: VFS: Unable to mount root fs` -- the exact, generic Linux
failure any emulator without a working root filesystem produces (no
9P device yet; that's M5). Along the way: the real kernel boot banner,
memory/MMU setup, the full kernel command line, and dozens of
subsystem/driver init messages, all on the real UART console. Devices
this project doesn't implement (virtio-block, a second UART, DRM,
ATA, keyboard, ethernet, RTC) all probe and fail *gracefully* instead
of wedging the boot -- `mmio.c`/`mem.c` print one diagnostic line per
unrecognized access rather than faulting silently.

Getting here needed one real fix: automatic tick-timer interrupt
delivery, deferred since M2, turned out not to be optional -- the
first boot attempt hung forever retry-polling an unimplemented ATA
controller, because nothing was advancing jiffies for its timeout to
expire against. `cpu_tick_check` (cpu.c) is now wired into `main.c`'s
loop. See `docs/c4or1k-design.md`'s M4 section for the full story.

`cpu.c`/`mem.c` implement the full non-privileged OR1000 integer ISA,
SPRs, the SR flag register, exception delivery, and DTLB/ITLB miss +
permission checks (not a page-table walk -- that's the guest kernel's
job). `uart.c`/`mmio.c`/`con.c` make it interactive. `boot.c` loads a
raw kernel image and patches its DTB's memory-size property, matching
jor1k's `OnKernelLoaded`/`PatchKernel` exactly.

    make c4or1k-m1-check           # M1: full ISA, diffed against jor1k
    make c4or1k-m2-check           # M2: SPRs/exceptions/TLB, diffed against jor1k
    make c4or1k-m3                 # M3: interactive echo server (real terminal)
    make c4or1k-m3-check           # M3: same, piped input, diffed byte-for-byte
    make c4or1k-m3-int-check       # M3: interrupt-driven variant of the same test

M1/M2 are diffed bit-for-bit against jor1k's own `safecpu.js`
(`tools/or1k-oracle.js`); M3's two echo servers are verified by piping
known input through and diffing the echoed output. M1 caught a
delay-slot bug in the *test program* (not cpu.c) plus two bugs in the
oracle itself. M2 added a `.org` assembler directive (`tools/asm.py`)
for planting exception handlers at real vector addresses, and
surfaced the EPCR-offset distinction between `EXCEPT_SYSCALL` and
every other exception type. See `docs/c4or1k-design.md`'s per-
milestone sections for all of it.

M0's throughput result (~1.37M guest instructions/sec on a tight
8-opcode loop) is preserved in the design doc; the full decode hasn't
been re-benchmarked in isolation, though the M4 boot (~100M
instructions in a few minutes) gives a rough real-workload figure.
