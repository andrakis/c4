# c4or1k

An OR1000/OpenRISC emulator, capable of booting Linux, written in C
and compiled by c4lc. A port of [jor1k](https://github.com/s-macke/jor1k)
(a JavaScript OR1000 emulator) to the C4 VM toolchain. See
`docs/c4or1k-design.md` for the milestone roadmap and design rationale.

Console only -- no framebuffer, no keyboard device. Terminal raw mode
is handled by `run-c4or1k.sh`, an external wrapper, not by the VM (the
C4 VM has no ioctl/termios facility, see that script's header comment).

## Status: M3 done (M0/M1/M2 preserved in docs/c4or1k-design.md)

`cpu.c`/`mem.c` implement the full non-privileged OR1000 integer ISA,
SPRs, the SR flag register, exception delivery (`l.sys`/`l.trap`/
`l.rfe`), and DTLB/ITLB miss + permission checks (not a page-table
walk -- that's the guest kernel's job from M4 on). `uart.c` (a real
16550-compatible device) and `mmio.c` (address-space routing between
`ram[]` and devices) make the emulator interactive: `con.c` polls
stdin without blocking the VM, and `run-c4or1k.sh` puts the terminal
in raw mode around it.

    make c4or1k-m1-check           # M1: full ISA, diffed against jor1k
    make c4or1k-m2-check           # M2: SPRs/exceptions/TLB, diffed against jor1k
    make c4or1k-m3                 # M3: interactive echo server (real terminal)
    make c4or1k-m3-check           # M3: same, piped input, diffed byte-for-byte
    make c4or1k-m3-int-check       # M3: interrupt-driven variant of the same test

**Result: exact/correct match for all of it.** M1/M2 are diffed
bit-for-bit against jor1k's own `safecpu.js` (`tools/or1k-oracle.js`);
M3's two echo servers (polled and interrupt-driven, the latter
exercising the whole M2+M3 IRQ pipeline end to end) are verified by
piping known input through and diffing the echoed output, not against
the oracle -- extending it to simulate real-time stdin arrival wasn't
worth building for this milestone, see the design doc. M1 caught
three real bugs worth knowing before touching anything downstream --
a delay-slot bug in the *test program*, not cpu.c, is the one to
actually worry about repeating -- plus two more that lived in the
oracle itself. M2 added a `.org` assembler directive (`tools/asm.py`)
since exception handlers live at real vector addresses, not inline
with the code that triggers them, and surfaced the EPCR-offset
distinction between `EXCEPT_SYSCALL` and every other exception type
that a handler must get right to return to the correct place. See
`docs/c4or1k-design.md`'s M1/M2/M3 sections for all of it.

M0's throughput result (~1.37M guest instructions/sec on a tight
8-opcode loop) is preserved in the design doc; the full decode hasn't
been re-benchmarked yet (`main.c` still prints instructions/sec each
run, but the test programs are far too short to produce a meaningful
number).
