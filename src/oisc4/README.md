# OISC4 — C4 on a one-instruction computer

OISC4 runs `.c4r` images, from c4cc or c4lc, on a virtual machine that has
exactly one instruction. It loads an image, translates every C4 opcode into a
short sequence of that instruction (1 to 11 of them), and runs the result.
Output is checked bit for bit against c4m. The self-hosted `c4` compiler and
the `c4sp` Lisp interpreter both run inside it, and OISC4 compiled by c4lc
runs under c4m, under plain c4, and under itself.

It replaces an earlier, partial attempt that forked the c4 frontend
(`src/tests/oisc-c4.c`, `oisc-min.c`, `oisc-asm.c`); the design doc has the
post-mortem.

## The instruction

    add source, increment, destination

- `source`: an address to read a word from.
- `increment`: a literal added to that word.
- `destination`: an address to write the result to.

After every instruction three flag registers say whether the value written
was less than, equal to, or greater than zero: `LT0`, `EQ0`, `GT0`. A set
flag holds the size of one instruction, not 1. In `oisc4.c` that is 24 bytes
(three 8-byte words); the older experiment counted in words and used 3.

**Jumps.** The program counter is an ordinary memory word, so storing to
`PC` is a jump. A conditional branch adds a flag to an address: the result
is either the address itself or the address plus one instruction, and two
unconditional jumps placed there pick the path.

    EQ0, LABEL_1, PC                ; jump using the flag and the address of LABEL_1
    LABEL_1: ABS0, NOT_EQ0, PC      ; flag was false: go to NOT_EQ0
             ABS0, WAS_EQ0, PC      ; flag was true:  go to WAS_EQ0
    NOT_EQ0: ; do something
             ABS0, DONE, PC         ; unconditional jump
    WAS_EQ0: ; do something else
    DONE:

**Self-modifying code is the addressing mode.** There is no indirect
operand, so loading through a pointer is two instructions: the first writes
the pointer's value into the `source` field of the second. C4's `LI` (load
int) becomes:

          ABS0, some_variable, LOC1   ; read a variable (really a memory address, so ABS0
                                      ; is used to get the true address), and write it
                                      ; into the next instruction's source
    LOC1: PLACEHOLDER, 0, R0          ; read through it into register R0

`PLACEHOLDER` only marks the field that gets rewritten. Every placeholder is
written immediately before it is used.

**Devices.** Some addresses are ports: reading or writing them triggers
hardware. In `oisc4.c` there are three: `PUTC` (console output), `SYSCALL`
(the written value is a c4m opcode number: open, read, malloc, printf and the
rest, with arguments read off the VM stack), and a math device (write `X`,
`Y` and `OP`, read `V`) for the sixteen ALU operations an adder cannot do
cheaply, such as multiply, divide, shifts and compares. That is the one
place OISC4 steps outside a single instruction.

## Build

| Target | Output |
|---|---|
| `make oisc4` | `src/oisc4/oisc4`, the native build. The binary is tracked in git, so rebuilding it shows up as a change. |
| `make oisc4-lc.c4r` | OISC4 compiled by c4lc `-O`: runs as a guest. |

The source keeps to the dialect c4cc and c4lc accept (one `int` type, no
structs, locals at the top), which is what makes `oisc4-lc.c4r` possible.

## Run

    ./src/oisc4/oisc4 [-d] [-s] [-v] [-m megs] [-c cycles] [--] program.c4r [args...]

| Flag | Meaning |
|---|---|
| `-d` | Trace every OISC instruction. With `-c`, only the last 100 before the limit. |
| `-s` | Disassemble the translated code and exit. |
| `-v` | Translation stats, and the cycle count at exit. |
| `-m megs` | Arena size in MB (default 64). |
| `-c cycles` | Stop after this many OISC instructions. |

(`-c` is accepted by the option parser but not listed in the program's usage
line.)

    ./src/oisc4/oisc4 src/tests/hello.c4r
    ./src/oisc4/oisc4 c4.c4r src/tests/hello.c      # the c4 compiler, inside OISC

## Test

Neither is in `make test`.

| Target | Checks |
|---|---|
| `make test-oisc4` | The `src/tests` images against `./c4m load-c4r.c`, output compared bit for bit (c4m's missed-trap chatter filtered, `%p` lines excluded), then c4 and c4sp running inside OISC4. |
| `make test-oisc4-nested` | `oisc4-lc.c4r` running hello under c4m, under plain c4 via `c4l.c`, and under native oisc4. |

The fused opcodes are exercised on OISC4 by `make test-fuse`, which runs a
fused image on it.

## Layout

| File | What it is |
|---|---|
| `oisc4.c` | The loader, the translator and the VM, in one file. |
| `oisc4` | The native build (tracked). |
| `test-oisc4.sh` | The comparison ladder behind `make test-oisc4`. |

## Hosts

| Host | How |
|---|---|
| Native | `./src/oisc4/oisc4`. |
| c4m | `./c4m load-c4r.c -- oisc4-lc.c4r -m 32 program.c4r` |
| plain c4 | `./c4 c4l.c oisc4-lc.c4r -m 32 program.c4r` |
| itself | `./src/oisc4/oisc4 -m 192 oisc4-lc.c4r -m 32 program.c4r` |

Those three nested commands are the ones `test-oisc4-nested` runs.

## Opcodes

OISC4 translates c4m's opcodes and all ten fused opcodes (79–88, `LDL`..`POPA`,
[docs/fused-opcodes.md](../../docs/fused-opcodes.md)). Fused support arrived
in 958eeb1 and was cut down to the current ten in bbb9ec3. c4mp's opcodes
66–78 (`CPUI`..`TRAW`, including `LXI`/`SXI`) are named but not translated;
an image that uses one fails at translation with "unknown opcode".

## Limits and known issues

- Kernel machinery is not there: `ITH`, signals, `C4CF` and the other trap
  and capability opcodes report no capability, so tests that need a real trap
  handler (`test_customop`, `test_signal`, `test_timekeeping`) are out of
  contract.
- `FLT` prints "float instruction unsupported".
- `%p` prints arena offsets, not host addresses.
- `c4m.c4r` cannot run: it scans its own code for C4 opcode words, which
  translated code no longer contains. Anything that generates C4 code and
  jumps to it (`C4IV`) is outside what a static translator can do.
- It is about 5.8 times slower than native c4m on the c4sp round-trip
  workload (one measurement, in the design doc).

## Docs

[docs/oisc4-design.md](../../docs/oisc4-design.md): the machine, the devices,
the translation of each opcode, verification, and a SIMD/GPU feasibility
study. It predates the fused opcodes and does not mention them, and its
list of files does not include the tracked `oisc4` binary.
