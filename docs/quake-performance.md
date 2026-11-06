# Quake: what the compiler and the interpreter gained

Quake's timedemo, measured before and after each change, from the code review of 2026-11-06.
Everything here keeps the frames: each step's demo1 still has gcc's checksum, `ea3fefe7`
over 978 frames. `make test-quake` holds all of it to gcc's frames on demo3.

**How it is measured**, on helios:
- *Instructions:* demo1 on the bare machine under node with `-sum 0`, a virtual clock and no
  checksum work. These counts repeat exactly.
- *Node:* the wall time of the same run, three runs each, baseline and new taking turns.
- *Page:* the game's own `speed:` line, in a background tab of Chrome at 400 MHz with 128 MB,
  Quake at the C4IX shell on CPU 0. One run of each, baseline and new served in turn.

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

**Extra CPUs** cost a lot with the MMU off and almost nothing with it on, with Quake on CPU 0
and nothing else running: ×1.39 against ×1.02 in round 2. At 4 CPUs the MMU buys back nothing,
as 105.4 s against 107.7 s shows. That is not explained yet.

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

## What is left

The FPU opcode is now 2.2% of instructions, so math is no longer where the time goes. By
instructions, in round 2's image:

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
which would change results; `ceil` and `trunc` themselves; an opcode for a 16-bit load (parked).
