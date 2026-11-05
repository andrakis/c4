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
loop. Extra CPUs still cost a lot with the MMU off, more than they do with it on, with Quake
on CPU 0 and nothing else running. That is not explained yet.
