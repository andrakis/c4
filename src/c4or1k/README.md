# c4or1k

An OR1000/OpenRISC emulator, capable of booting Linux, written in C
and compiled by c4lc. A port of [jor1k](https://github.com/s-macke/jor1k)
(a JavaScript OR1000 emulator) to the C4 VM toolchain. See
`docs/c4or1k-design.md` for the milestone roadmap and design rationale.

Console only -- no framebuffer, no keyboard device. Terminal raw mode
is handled by `run-c4or1k.sh`, an external wrapper, not by the VM (the
C4 VM has no ioctl/termios facility, see that script's header comment).

## Status: M2 done (M0/M1 preserved in docs/c4or1k-design.md)

`cpu.c`/`mem.c` implement the full non-privileged OR1000 integer ISA
plus SPRs, the SR flag register, exception delivery (`l.sys`/`l.trap`/
`l.rfe`), and DTLB/ITLB miss + permission checks (not a page-table
walk -- that's the guest kernel's job from M4 on). `main.c` loads a
flat binary of assembled words and runs it as a test-program harness.

    make c4or1k-m1-check       # M1: full ISA, diffed against jor1k
    make c4or1k-m2-check       # M2: SPRs/exceptions/TLB, diffed against jor1k

**Result: exact match against jor1k** for both. M1 caught three real
bugs worth knowing before touching anything downstream -- a
delay-slot bug in the *test program*, not cpu.c, is the one to
actually worry about repeating -- plus two more that lived in the
oracle itself (real endianness/aliasing quirks in jor1k's own memory
model, not obvious from reading the CPU code alone). M2 added a new
assembler directive (`.org`, in `tools/asm.py`) since exception
handlers live at real vector addresses, not inline with the code that
triggers them -- see `docs/c4or1k-design.md`'s M1/M2 sections for
all of it, including the EPCR-offset distinction between
`EXCEPT_SYSCALL` and every other exception type that a handler must
get right to return to the correct place.

M0's throughput result (~1.37M guest instructions/sec on a tight
8-opcode loop) is preserved in the design doc; the full decode hasn't
been re-benchmarked yet (`main.c` still prints instructions/sec each
run, but the test programs are far too short to produce a meaningful
number).
