# Quake: what the compiler and the interpreter gained

Quake's timedemo, measured before and after each change, from the code review of 2026-11-06.
Everything here keeps the frames: each step's demo1 still has gcc's checksum, `ea3fefe7`
over 978 frames. `make test-quake` holds all of it to gcc's frames on demo3.

**How it is measured**, on helios:
- *Instructions:* demo1 on the bare machine under node with `-sum 0`, a virtual clock and no
  checksum work. These counts repeat exactly.
- *Node:* the wall time of the same run, three runs each, baseline and new taking turns.
- *Page:* the game's own `speed:` line, in a background tab of Chrome at 400 MHz with 128 MB,
  Quake at the C4IX shell. One run of each, baseline and new served in turn. At 4 CPUs it
  ran on CPU 3 (see "Extra CPUs" below), unless it says otherwise.

## Round 1: the compiler's easy wins, and the FPU in the loop

Each step's instructions are measured on top of the steps above it.

| step | M instructions a frame | change | image bytes |
|---|---:|---:|---:|
| baseline | 18.154 | | 2,773,978 |
| `+0`: `PSH; IMM 0; ADD` is nothing | 17.961 | −1.06% | 2,745,462 |
| an unused `x++` keeps no old value | 17.692 | −1.50% | 2,732,862 |
| integer constants as float literals, `(float)3` | 17.255 | −2.47% | 2,700,726 |
| known offsets: chains, `&local + k`, `&global + k` | 16.876 | −2.20% | 2,610,218 |
| no mask before `SC` | 16.392 | −2.87% | 2,607,858 |
| `STL` for a word-sized local | 15.618 | −4.72% | 2,593,162 |
| **all of them** | | **−14.0%** | **−6.5%** |

The FPU's place in the interpreter's loop changes no instruction, and shows only on the wall:

| same image, 15.618 M a frame | wall |
|---|---:|
| a float's FPU opcode leaves the loop | 82.2 s |
| a float's FPU opcode stays in the loop | 73.1 s (−11%) |

| Where it runs | baseline | round 1 | time |
|---|---:|---:|---:|
| node, bare machine | 93.4 s | 73.2 s | −21.6% |
| page, MMU on, 1 CPU | 216.9 s, 4.5 fps | 156.1 s, 6.2 fps | −28.0% |
| page, MMU off, 1 CPU | 162.9 s, 6.0 fps | 117.9 s, 8.2 fps | −27.6% |
| page, MMU on, 4 CPUs | 214.3 s, 4.5 fps | 162.2 s, 6.0 fps | −24.3% |
| page, MMU off, 4 CPUs | 233.9 s, 4.1 fps | 149.0 s, 6.5 fps | −36.3% |

The page gains more than node does because more of its time was the FPU's way out of the
loop.

## Round 2: the FPU where the code stands

| step | M instructions a frame | change | image bytes |
|---|---:|---:|---:|
| round 1 | 15.618 | | 2,593,162 |
| `-mfpu`: a float's or a double's operation is the opcode, not a call | 11.411 | −26.9% | 2,321,594 |
| a float against a double constant compares as floats | 11.402 | −0.08% | 2,317,746 |
| **round 2** | | **−27.0%** | **−10.6%** |
| **both rounds, against the baseline** | | **−37.2%** | **−16.4%** |

The compare gains little because inlining had already taken most of what a double compare
cost. It is kept for being exact, and because it works without the flag.

Round 1's loop handled a float's opcode. Doubles take the same place, and again no instruction
changes:

| same image, 11.4 M a frame | wall |
|---|---:|
| a double's FPU opcode leaves the loop | 52.6 s |
| a double's FPU opcode stays in the loop | 49.3 s (−6.3%) |

## All three, from one sitting

Node took turns, three runs each. The page ran round 2, then the baseline, then round 1, one
run of each. All on 2026-11-06.

| Where it runs | baseline | round 1 | round 2 | round 1 | round 2 | speed |
|---|---:|---:|---:|---:|---:|---:|
| node, bare machine | 94.7 s | | 49.3 s | | −47.9% | ×1.92 |
| page, MMU on, 1 CPU | 216.8 s, 4.5 fps | 153.9 s, 6.3 fps | 103.6 s, 9.4 fps | −29.0% | −52.2% | ×2.09 |
| page, MMU off, 1 CPU | 161.8 s, 6.0 fps | 114.6 s, 8.5 fps | 77.3 s, 12.6 fps | −29.2% | −52.2% | ×2.09 |
| page, MMU on, 4 CPUs | 224.6 s, 4.3 fps | 161.2 s, 6.0 fps | 105.4 s, 9.2 fps | −28.2% | −53.1% | ×2.13 |
| page, MMU off, 4 CPUs | 254.9 s, 3.8 fps | 171.2 s, 5.7 fps | 107.7 s, 9.0 fps | −32.8% | −57.7% | ×2.37 |

How far to trust one run: against round 1's table above, the same builds at 1 CPU came within
3%, MMU on at 4 CPUs within 5%, and MMU off at 4 CPUs differed by 9% and 15%. That last row's
gain is the least certain.

The wall gains more than the instructions do because what is gone was the expensive part: an
opcode that stays in the loop costs far less than a call and a way out of it.

**The MMU** costs ×1.34 at 1 CPU, in all three builds: 216.8/161.8, 153.9/114.6, 103.6/77.3.
Nothing here touched it, so it shrinks only in proportion.

**Extra CPUs** cost a lot with the MMU off and almost nothing with it on: ×1.39 against ×1.02
in round 2. This is not the extra CPUs as such. At 4 CPUs Quake does not run on CPU 0: the
scheduler gives a task the highest idle CPU (`smp_idlefor`, `sched.c`), so it runs on CPU 3, on a
worker of its own, and `pin 0 quake` puts it back. The cost is that of running on such a CPU with
the MMU off.

**So no game a person plays pays this.** These timedemos draw nothing. A program that draws is
pinned to CPU 0 (`sched_softpin`), so with `-show` Quake goes there unasked: 29.5 s for 300 frames,
the same as `pin 0`. To compare builds at several CPUs, use `-show` or `pin 0`. The 4 CPU rows above
measure a place no displayed game runs.

Measured below in one sitting, a slower one than the table above, nothing else open.

| 4 CPUs, MMU off, demo1 | time | fps |
|---|---:|---:|
| on CPU 3, as it was | 111.0 s | 8.8 |
| on CPU 3, the guard read plainly | 101.8 s | 9.6 |
| `pin 0` | 87.7 s | 11.1 |
| 1 CPU, for comparison | 84.4 s | 11.5 |

With the MMU on, CPU 3 and CPU 0 come within 1 s over 300 frames (36.3 s and 35.3 s; later, in a
slower sitting, 37.9 s both), so the MMU's cost and this one nearly cancel at 4 CPUs.

About 3.5 s of the 9.2 s this cost in 300 frames was the boot stack's guard. On another CPU
`stackGuard`, `stackBase` and `stackTop` were each an `Atomics.load`, and Quake makes 60,000 `MCPY`
calls a frame, each looking at the guard five times: 278 M loads in 300 frames. With the MMU on
`MCPY` goes to `mmu.block` and never looks. They are plain reads now. Under node, on helios and on a
Windows machine, that closes the gap altogether.

**The rest, about 5 s in 300 frames, is not explained.** It shows only in the page. CPU 3 runs the
same 3.892 G instructions, at a steady 111 M a second against CPU 0's 140 M, so it is not extra
work. A profile puts the difference at the top of `runPlain`'s loop, `++cycle > lim` and the fetch,
and not in the opcodes. What it is not:

- the CPU: `pin 1`, `pin 2` and `pin 3` all take 35 s;
- `ApMachine.sys` inlined into the loop: made too big to inline, 34.6 s against 35.1 s;
- `cycle` as a double: an int32 counter in the loop, 34.3 s against 34.9 s, and none on CPU 0;
- warm-up: a second timedemo in the same machine takes the same, and 300 frames and 978 show the
  same ratio;
- the slice, 16K cycles instead of 64K, or `cycle` starting past any Smi: no change;
- `runUser` on the same thread loses about 1 s, so the thread itself is not slow.

Two things point somewhere. One thread spinning in another tab takes CPU 0 from 29.6 s to 40.8 s,
and MMU on from 37.9 s to about 52 s, on a host with 12 logical CPUs, while CPU 3 with the MMU off
goes only from 34.5 s to 40.5 s: under that load the gap is gone. And for about twenty minutes in one
sitting CPU 3 ran 27-28 s (a whole demo in 89 s), which could not be repeated, so the code is able to
run that fast there. That may also be the 9-15% between sittings noted above.

It belongs to one machine or one Chrome. 300 frames, MMU off, the tab in front:

| | CPU 3 | CPU 0 |
|---|---:|---:|
| desktop, Windows, Chrome 159 | 34.3 s | 29.6 s |
| Chromebook, Chrome 151 | 28.8 s | 28.6 s |

These cannot say whether it is the version, the system or the processor. A tab being in front does
not change the desktop's figures. On the Chromebook a tab behind another costs 40-50%.

**What the round trips to CPU 0 cost** is small. Every request from CPU 3 timed, in 300 frames: 505
and 25 ms waiting without `-show`; with `pin 3` and `-show`, 301 presents at 0.4 ms each, 120-150 ms
in all. Forcing a drawing Quake onto CPU 3 takes 37.4-38.0 s on the desktop and 33.3 s on the
Chromebook, with 4% more instructions. The time goes on running, not on waiting for CPU 0.


**Other images.** Only Quake is built `-mfpu`, but the rest of the compiler's work reaches
everything. Their sizes moved; their speed has not been timed.

| image | baseline | now | change |
|---|---:|---:|---:|
| `games/quake.c4r` | 2,773,978 | 2,317,746 | −16.4% |
| `disk/c4ix-quake.c4r` | 2,463,965 | 2,009,781 | −18.4% |
| `games/rott.c4r` | 3,123,115 | 3,011,263 | −3.6% |
| `games/tyrian.c4r` | 2,398,529 | 2,317,973 | −3.4% |
| `games/doom.c4r` | 1,730,766 | 1,681,798 | −2.8% |
| `games/wolf3d.c4r` | 1,224,865 | 1,190,797 | −2.8% |
| `games/wolf3d6.c4r` | 1,194,810 | 1,161,642 | −2.8% |
| `c4ix32.c4r` | 634,124 | 630,072 | −0.6% |
| `c4dos32.c4r` | 166,311 | 166,311 | none |

## Round 3: libc's math, and `trunc`

Rounds 1 and 2 left `math.c` itself as it was: built without `-mfpu`, it calls a helper for every
operation on a double, and each helper asks whether there is an FPU. Counting each math function
with everything it calls, by walking the frames at each sample, round 2's image spent:

| function | share of instructions | calls a frame | instructions a call |
|---|---:|---:|---:|
| `ceil` | 5.42% | 792 | 779 |
| `sin` | 2.57% | 88 | 3,343 |
| `cos` | 2.12% | 88 | 2,742 |
| `floor` | 0.62% | 128 | 557 |
| **all of libc's math** | **10.77%** | | |

`ceil` costs more than `sin` and `cos` together, 3.9% of it from `R_EmitEdge`, because `trunc` took
a double's bits through 64-bit shift helpers. So:

| step | M instructions a frame | change | image bytes |
|---|---:|---:|---:|
| round 2 | 11.402 | | 2,317,746 |
| `trunc` through an `int` where the value fits (libc, for everyone) | 11.226 | −1.5% | 2,318,686 |
| Quake's own copy of `math.c`, built `-mfpu` (`games/quake/c4.mk`) | 10.533 | −6.2% | 2,236,734 |
| **round 3** | | **−7.6%** | **−3.5%** |
| **all three rounds, against the baseline** | | **−42.0%** | **−19.4%** |

That takes libc's math to 3.41%: `ceil` 1.65%, `sin` and `cos` 1.47%, `floor` 0.26%. Under C4IX
Quake used to share libc's math and now carries its own, so `disk/c4ix-quake.c4r` grows from
2,009,781 bytes to 2,079,971. `trunc` adds under 3 KB to the other games.

All in one sitting, 2026-11-06, a slower one than the table above. The page ran round 3, round 2,
then the baseline, one run of each, without the guard change described under "Extra CPUs". Node
took turns between rounds 2 and 3, three runs each, with one run of the baseline at the end.

| Where it runs | baseline | round 2 | round 3 | round 3 against round 2 | against the baseline | speed |
|---|---:|---:|---:|---:|---:|---:|
| node, bare machine | 99.3 s | 51.2 s | 44.9 s | −12.3% | −54.8% | ×2.21 |
| page, MMU on, 1 CPU | 224.9 s, 4.3 fps | 109.0 s, 8.9 fps | 94.0 s, 10.4 fps | −13.7% | −58.2% | ×2.39 |
| page, MMU off, 1 CPU | 169.8 s, 5.7 fps | 82.8 s, 11.8 fps | 74.6 s, 13.1 fps | −10.0% | −56.1% | ×2.28 |
| page, MMU on, 4 CPUs | 238.0 s, 4.1 fps | 110.5 s, 8.8 fps | 98.1 s, 9.9 fps | −11.2% | −58.8% | ×2.43 |
| page, MMU off, 4 CPUs | 244.2 s, 4.0 fps | 111.8 s, 8.7 fps | 93.9 s, 10.4 fps | −16.0% | −61.6% | ×2.60 |

**Two things this rules out for now.**

*Tail calls.* Of 76.6 k calls a frame, 1.8 k (2.4%) are in tail position, a call with only `ADJ` and
`LEV` after it, and taking those out would save at most 0.03% of instructions. `c4opt` has a tail
pass already, and finds nothing in `math.c`: `return -k_sin(...)` still has to negate; a double
comes back through a hidden pointer, so `sin` copies the result after the call; and `k_sin` takes
five words of arguments where `sin` has three, and the caller clears them.

*`sin` and `cos` as FPU operations.* The FPU opcode has none, though the older `FLTI_SIN` and
`FLTI_COS` do. They would now save at most 1.4% of instructions, and at a risk: today's software
`sin` gives the same bits on every host, and a host's `sinf` or `Math.sin` need not, which
`test-quake` would notice. `ceil` and `floor` as FPU operations would save up to 1.9% and are exact
everywhere, so they would come first.

## What is left

The FPU opcode is now 2.2% of instructions, so math is no longer where the time goes. By
instructions, in round 2's image, before round 3 took most of the last row and the `__c4ll_shr`:

| function | share |
|---|---:|
| `D_DrawSpans8` | 23.5% |
| `R_DrawSurfaceBlock8_mip0`, `_mip1` | 18.8% |
| `D_DrawZSpans` | 8.6% |
| `D_PolysetDrawSpans8` | 3.0% |
| `R_RecursiveWorldNode` | 2.7% |
| `<ld16u>`, `<ld16s>`, `<st16>`, `<lsr>` | 5.4% |
| `R_LeadingEdge` | 2.6% |
| `__c4ll_shr` | 2.0% |
| `trunc`, `ceil` | 2.1% |

Not tried yet: writing a double operation straight to its target; double literals as data, not
temporaries; float arithmetic where a double literal is exact; `x * 0.1` as a float by a flag,
which would change results; `ceil` and `floor` as FPU operations; an opcode for a 16-bit load (done in round 4).

## Round 4: 16-bit loads and stores (2026-11-13)

Instructions only, as "How it is measured" says: demo1 on the bare machine under node, `-sum 0`,
the whole run of 978 frames, every instruction and every call counted. `test-quake` holds each
build to gcc's frames on demo3.

| step | M instructions a frame | change | calls a frame | image bytes |
|---|---:|---:|---:|---:|
| round 3 | 10.533 | | 56,360 | 1,571,465 |
| 16-bit loads and stores, LSR, as opcodes (docs/c4-16bit.md) | 9.921 | −5.8% | 24,169 | 1,563,937 |
| `LCU`, an unsigned byte's load (docs/c4-16bit.md) | 9.719 | −2.0% | 24,169 | 1,561,585 |
| `R_DrawSurfaceBlock8`'s rows and `D_DrawSpans8`'s runs written out (`C4QUAKE`) | 8.573 | −11.8% | 24,169 | 1,565,961 |

**16-bit loads and stores.** `<ld16u>`, `<ld16s>`, `<st16>` and `<lsr>` were 32,200 calls a frame
(`<ld16u>` 16,000 of them: the edges' and the surfaces' `unsigned short` fields), each a call and
19 to 24 instructions. They are opcodes now, `LHU`, `LH`, `SH` and `LSR`, one instruction each;
docs/c4-16bit.md says why opcodes and what each machine does.

**The two inner loops** that were 47% of the instructions, written for c4 under `C4QUAKE`, the same
pixels in the same order. On a stack machine a loop's counting is a large part of a pixel's work:
`for (b=15; b>=0; b--)` costs a compare, a branch, a decrement and a jump for every pixel, and
`*pdest++` keeps the old pointer on the stack and pops it back.

- `R_DrawSurfaceBlock8_mip0` to `_mip3` write a row's 16, 8, 4 or 2 pixels out, without the loop and
  without `pix` kept in memory between its load and its use: 21 instructions a pixel, from 40.
- `D_DrawSpans8` moves `pdest` past the run of up to eight pixels it draws between two divisions
  and jumps into the run written out, through a dense `switch`, which c4lc makes a jump table; each
  pixel is written back from past the run: 27 instructions a pixel, from 36.

