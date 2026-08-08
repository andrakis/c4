# c4or1k

An OR1000/OpenRISC emulator, capable of booting Linux, written in C
and compiled by c4lc. A port of [jor1k](https://github.com/s-macke/jor1k)
(a JavaScript OR1000 emulator) to the C4 VM toolchain. See
`docs/c4or1k-design.md` for the milestone roadmap and design rationale.

Console only -- no framebuffer, no keyboard device. Terminal raw mode
is handled by `run-c4or1k.sh`, an external wrapper, not by the VM (the
C4 VM has no ioctl/termios facility, see that script's header comment).

## Status: M1 done (M0 preserved in docs/c4or1k-design.md)

`cpu.c`/`mem.c` implement the full non-privileged OR1000 integer ISA
(no MMU/SPRs/exceptions yet -- that's M2). `main.c` loads a flat
binary of assembled words and runs it as a test-program harness.

    make c4or1k-m1            # build + run tests/m1_test.s
    make c4or1k-m1-check       # ...and diff every register/flag/RAM
                                # word against jor1k's own safecpu.js

**Result: exact match against jor1k**, across every instruction form
exercised by `tests/m1_test.s`. Building this caught three real bugs
worth knowing before touching M2 -- a delay-slot bug in the *test
program*, not cpu.c, is the one to actually worry about repeating;
see `docs/c4or1k-design.md`'s M1 section for all three plus two more
that lived in the oracle itself (real endianness/aliasing quirks in
jor1k's own memory model, not obvious from reading the CPU code
alone).

M0's throughput result (~1.37M guest instructions/sec on a tight
8-opcode loop) is preserved in the design doc; M1's full ~35-mnemonic
decode hasn't been re-benchmarked yet (`main.c` still prints
instructions/sec each run, but `tests/m1_test.s` is far too short to
produce a meaningful number).
