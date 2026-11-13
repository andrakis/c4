# 16-bit loads and stores, a logical shift and an unsigned byte: five opcodes

c4's machine had no 16-bit load or store and no logical shift right. c4lc
and c4fc called a helper for each, a routine laid down in every unit that
used it: `<ld16s>` for a `short`, `<ld16u>` for an `unsigned short`,
`<st16>` for a store to either, and `<lsr>` for an unsigned `>>` by a count
known only at run time. Since 2026-11-13 they are opcodes, 90 to 93:

| | opcode | does | was |
|---|---|---|---|
| 90 | `LH` | `a = *(short *)a` | `PSH; JSR <ld16s>; ADJ 1`, 22 instructions |
| 91 | `LHU` | `a = *(unsigned short *)a` | `PSH; JSR <ld16u>; ADJ 1`, 24 |
| 92 | `SH` | `*(short *)*sp++ = a`, `a` unchanged | `PSH; JSR <st16>; ADJ 2`, 25 |
| 93 | `LSR` | `a = (unsigned)*sp++ >> a` | `PSH; JSR <lsr>; ADJ 2`, 27 |

A short is two bytes, low byte first, at any address: the opcodes never
need an aligned one, as the helpers never did. `LH`'s high byte is loaded
as `LC` loads a char, so the value has the short's sign. `SH` leaves `a` as
it found it, as `SI` does and as `<st16>` returned it; the value of an
assignment to a short was already converted before the store, so nothing
the program sees changes. `LSR` takes its count modulo the word's bits,
as every machine's `SHR` already does and as x86 does; a count of 0 gives
the value back.

## Why

In Duke Nukem 3D's demo (docs/duke3d-port.md) `<ld16s>` was the biggest
single cost, 13% of the instructions and 58,700 calls a frame: `short`
fields and arrays are everywhere in the Build engine. In Quake's timedemo
(docs/quake-performance.md) the four helpers were 5.4%.

## Two ways to do it, and the one taken

**Open-coded by the compilers, no new opcode.** The load needs its address
twice, once for each byte, and a stack machine with no way to copy the top
of the stack has to put it somewhere: a frame word the function sets
aside. Under `-mfuse` the shortest signed load is

    STL t; PSH; IMM 1; ADD; LC; PSH; IMM 8; SHL; PSH; LDL t; LC; PSH; IMM 255; AND; OR

15 instructions against the helper's 22, and 18 for an unsigned one.
Without `-mfuse` (LEA, PSH, SI and LI for STL and LDL) it is longer than
the call. The store needs both the address and the value twice, two frame
words, about 17 instructions. No machine changes; c4fc copies what c4lc
writes.

**An opcode each.** One instruction, but every machine has to run it.

Counted on Duke's demo, frames 400 to 1000, the open-coded way would have
saved 7 instructions on each of the 58,716 signed loads a frame and about
8 on each of the 11,737 stores: about 0.5 M of the 7.12 M instructions a
frame, 7%. The opcodes took off 1.26 M, 17.7% (below). The difference is
not the call alone: the open-coded load is still fifteen instructions,
fourteen of them the bytes put together by hand, and a short load is as
common in Duke as a byte load.

So: the opcodes. What they cost is below, and it is paid once; each
machine's part is a few lines next to its `LC` and `SC`.

## Every machine

| machine | where | how |
|---|---|---|
| c4m, native and built by c4lc | `c4m.c` | in the hot part of the dispatch chain, after `SC`; plain C, no `unsigned` and no `short`, so plain c4 reading c4m.c as source (innerbench) still compiles it |
| c4mpg | `c4mpg.c` | as c4m, with the guard's 2-byte range check that `LC` and `SC` make for 1 |
| c4mp | `src/c4mp/vm.c`, `c4mp.h` | as c4m |
| libjs | `libjs/c4m.js` | `runPlain`'s switch: both bytes from `u8` when the address is plain memory below the boot stack's guard, `ld16`/`st16` (two `ld8`/`st8`) otherwise; `opcd()` for `OPCD` |
| libjs, MMU on | `libjs/mmu.js` | `runUser`'s switch: both bytes through the page when they are in one page, `pRd16`/`pWr16` otherwise, which fault from the instruction's start, so it runs again from nothing |
| c4bb sim | `src/c4bb/hw/microcode.uc` | microcode from transfers the board already makes, no new circuitry: `LH`/`LHU` 7 microsteps (two byte reads, `B_INC1` for the second address, the ALU's `SHL` and `OR`), `SH` 8, `LSR` 10 (`SHR`, then the sign's copies masked off: `~((MIN >> c) << 1)`) |
| oisc4 | `src/oisc4/oisc4.c` | expansions through the math device: `LH` 17 OISC instructions, `LHU` 20, `SH` 12, `LSR` 23. Byte mode (`RWR`, `RWW`) is on around the byte accesses alone: it applies to every source or target but a port |

The name tables follow, appended as every opcode since 66 has been:
`load-c4r.c`, `c4l.c`, `src/c4cc/c4cc.c` (c4cc's token count is one more,
15148), `src/c4sp/lisp/c4r.lisp`, `src/c4fc/emit.f`, `src/c4bb/sim/devices.js`
(the opcode-name ROM, now to 0x3DA), `src/c4ix/user/desktop/explore.c`,
`libjs/web/opdoc.js`, and `src/c4bb/tools/opscan.mjs`, where the four are
c4m's own. Plain c4 does not have them: `c4l.c` names them in its refusal,
as it names the fused opcodes. A board built from `microcode.uc` needs its
microcode ROM made again, and nothing else.

c4lc emits `LH`, `LHU`, `SH` and `LSR` where it called the helpers
(`g:load!`, `g:store!`, an unsigned `>>` by a variable), and c4fc the same
(`LOAD,`, `STORE,`, its `>>`): their objects stay byte for byte the same,
which `test-c4fc` holds them to. Only `<udiv>` and `<umod>` are helpers
now.

`test-c4lc` runs `src/tests/c4lc_h16.c` -- shorts at every alignment, the
values of their assignments and post-increments, unsigned shifts by every
count -- on c4m at both word sizes, on libjs and on the c4bb sim, against
gcc's answers; c4fc's and c4sc's sweeps compile it too.

## What it gained

Instructions a frame on node's bare machine, every call counted (the
profiler of docs/duke3d-port.md). Every frame is the same: Duke's demo
ends on the same checksum, and `test-duke3d` and `test-quake` hold every
build to gcc's frames.

| | before | after | |
|---|---:|---:|---:|
| Duke Nukem 3D, Atomic `demo1.dmo`, frames 400-1000 | 7.119 M | 5.861 M | -17.7% |
| ... calls a frame | 95,555 | 23,365 | |
| Quake, `demo1` timedemo, 978 frames, whole run | 10.533 M | 9.921 M | -5.8% |
| ... calls a frame | 56,360 | 24,169 | |
| Duke's image, bare machine | 4,680,784 bytes | 4,157,388 | -11.2% |
| Quake's image, bare machine | 1,571,465 bytes | 1,563,937 | -0.5% |

Duke has fewer calls by three quarters: 58,700 of the 72,200 calls it no
longer makes were `<ld16s>`. What is left of its profile, and Quake's, is
in docs/duke3d-port.md section 10 and docs/quake-performance.md round 4.

The other games, with `LCU` (below) as well, each demo whole on node's bare
machine, every frame the same by its checksum:

| | before | after | | calls a frame |
|---|---:|---:|---:|---:|
| Doom, `-timedemo demo3`, 2,173 frames | 4.496 M | 3.856 M | -14.2% | 23,976 -> 10,267 |
| Wolfenstein 3D, `--timedemo 1`, 1,214 frames | 2.902 M | 2.152 M | -25.9% | 44,420 -> 4,646 |
| Rise of the Triad, `--timedemo 4`, 609 frames | 4.818 M | 4.340 M | -9.9% | 17,809 -> 9,890 |

Wolfenstein 3D's `ReadShort` and the scalers' tables are shorts. What each
spends the rest on: Doom `R_DrawColumn` 32%, `R_DrawSpan` 21% and its test's
own checksum (`dg_frame`) 22%; Wolfenstein 3D `ScalePost` 42% and the
display's `c4sdl_present` 19%; ROTT its checksum (`rott_frame`) 21%, the row
and wall drawers 37%, and `__c4ll_udivmod` 11% -- which c4lc's long long
division made 1.5% the same day (docs/c4lc-design.md 14.7): ROTT 3.916 M.
Then ROTT's `FixedMul` and `FixedMulShift` (`watcom.c`, under `C4ROTT`),
which c4lc made four long long calls each, from 32-bit operations as Duke's
`c4_mulscale` is made: 3.729 M, -4.8%; -22.6% in all.

## And an unsigned byte: `LCU`

The same machinery made a fifth opcode cheap. An `unsigned char` load was
`LC; PSH; IMM 255; AND`: `LC` sign-extends, and the mask takes the sign
off. Textures, palettes and colour maps are arrays of unsigned bytes, so
that sequence is in every game's inner loops: sampling where the pc was,
it began 1.5% of all the instructions Duke ran (6% of them were in it)
and 0.7% of Quake's.

| | opcode | does | was |
|---|---|---|---|
| 94 | `LCU` | `a = *(unsigned char *)a` | `LC; PSH; IMM 255; AND` |

Every machine as above: c4m, c4mpg and c4mp `*(char *)a & 255`; libjs
`u8[a]`; the c4bb microcode is `LC`'s two microsteps with `MEM_RDBU` (the
board already reads unsigned bytes, for `PUTC` and `MCMP`); oisc4 a byte
read and an `AND` through the math device, 7 instructions. c4lc's and
c4fc's `unsigned char` loads emit it. `src/tests/c4lc_u8.c` runs beside
`c4lc_h16.c`.

| | before | after | |
|---|---:|---:|---:|
| Duke, demo1, frames 400-1000 | 5.861 M | 5.595 M | -4.5% |
| Quake, demo1 timedemo | 9.921 M | 9.719 M | -2.0% |

Same checksums, same frames.

