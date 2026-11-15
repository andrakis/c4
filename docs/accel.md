# Accelerators

A game can hand its heaviest loops to the host, in its own JavaScript: a wall's column, a floor's
row, Quake's whole span lists, each one instruction however much it does. `libjs/accel.js` is the
loader and the kit those modules use; the modules are the games' own, `games/duke3d/c4/accel.mjs`
and `games/quake/c4/accel.mjs`, and libjs knows no game. Duke Nukem 3D and Quake use theirs when the
machine has them and their video menu says so, and run their own C everywhere else. The frames are
the same either way, to the byte.

**In short** (2026-11-15, section 4):
- In the page (400 MHz, the MMU on), Quake's demos average 24 to 27 fps where they averaged 12 to 14,
  their slowest 1% of frames 11 to 12 where it was about 6; Duke's Atomic demos average 52 to 53 in
  play where they averaged 27 to 29, their slowest 1% 20 to 23 where it was 13 to 16, and the
  shareware script 60 where it was 28. Without the MMU, Quake 29 to 34, Duke 62 to 75.
- On node's bare machine a median frame is a third of the instructions it was or less (Duke 4.1-5.2 M
  to 1.4-1.9 M; Quake 7.1-8.4 M to 3.1-3.6 M), and Quake's heaviest 1% under half (16-18 M to 7 M).
- The same frames: every checksum is the one it was, with the accelerators and without, in node and
  in the page under the MMU; test-duke3d and test-quake match gcc's build frame for frame.
- Each game's video menu has the switch, on unless turned off, saved in its configuration.
- Duke's demo1 dies at frames 1949 to 2782, a desync between the game's versions rather than play,
  and is left out of these numbers.

## 1. How it works

The measurements in `docs/framebench.md` (sections 3 and 4) found that the frames a player sees as
choppy are drawing: the median frame was about 40% pixel loops, the heaviest 1% far more. On c4's
stack machine a textured pixel is 15 to 70 instructions; done by the host it is a few nanoseconds.

- **The game's own JavaScript.** A game that wants accelerators ships `games/NAME/c4/accel.mjs`,
  whose default export is given libjs's kit and returns an array of operations, functions of the
  machine. Its C side names them by their place in that array (`c4/c4accel.h`, `source/c4accel.h`).
  libjs knows no game and no loop.
- **Which modules.** build-images.sh lists the games that ship one in `src/c4bb/images/accel.json`.
  A machine loads those before it starts (`loadAccel`, `accelManifest`: cli.js, framebench, the
  page's worker, which holds the page's messages until they are in), from the site's own `games/`
  folder and nowhere else, in the list's order. A program can name a module, never supply one: a
  name is `[a-z0-9_-]`, and a module is code of the site, as libjs is.
- **Their own numbers.** Opcodes `0x1000` to `0x1fff` (`ACC_BASE`, `ACC_END` in `libjs/c4m.js`),
  outside the instruction set (0 up, append-only), C4KE's services (128 up) and C4IX's syscalls (200
  up). `0x1000` is `ACC_FIND`: `__c4_opcode("quake", 0x1000)` gives the first of that module's 256
  numbers, `0x1000 + 0x100 * (k + 1)` for the k-th module loaded, or 0. A program asks once and
  reaches its op i through `OPCD` as `base + i`, the arguments pushed as for a call. A number nobody
  has is a custom opcode as any other (`TRAP_ILLOP`).
- **Announced, never probed.** INFO bit `C4I_ACCEL` (`0x100000`) says the machine has the loader;
  `accel_find()` (`libjs/guest/accel.h`) asks for a game's only then, and is 0 everywhere else. libjs
  has it unless made without (`createMachine({ accel: false })`, `cli.js --no-accel`, the page's
  `&accel=0`, `framebench --no-accel`). C4IX passes INFO to its tasks as it is; a CPU on a thread
  (`ap.js`) is given CPU 0's module names and loads them in the same order before it runs, so a task
  sees the same numbers wherever it runs. Native c4m, c4bb and C4DOS's machines never say so.
- **The same results.** An operation is its loop, step for step: the same reads, the same writes in
  the same order, integer arithmetic wrapping at 32 bits, shifts modulo 32 as c4's `SHL` and `LSR`
  take them, and floats as c4's FPU computes them: each single operation `Math.fround` of the double
  one, a conversion to int toward zero with what does not fit the most negative int (the kit's
  `fval`, `fbits`, `toInt`). What the loop leaves behind (Duke's `vplce[]`, `fpuasm`, `asm4`; Quake's
  `d_aspancount`, `errorterm`) it leaves too.
- **The MMU.** An operation reads and writes through the kit's `reader`, `writer`, `reader32` and
  `writer32`: under the MMU every address goes through the task's pages, one page cached at a time
  for each stream, and one the task may not use faults as the loop's own load or store would. The
  instruction then runs again from the start once the kernel has seen to it, so an operation gives the
  same result run again after a partial run: what it writes it reads back only where writing the same
  again is what the loop would do (a z buffer it has already set reads equal, and is set again).
  Without the MMU they read the arena directly, above the low page and short of the boot stack's guard.
- **One instruction.** It counts one cycle however much it does, as `MCPY` does: the point is that the
  machine's clock (400 MHz in the page) is not spent on it. A test that times the game in the
  machine's clock sees it finish sooner: test-duke3d's General MIDI check counts the notes in the
  loops' 600 frames, and runs with `--no-accel`.

## 2. The operations

**Duke Nukem 3D** (`games/duke3d/c4/accel.mjs`, `c4/c4accel.h`; `DA_*`):

| op | macro | Duke's C | what it is |
|---|---|---|---|
| 0, 1 | `duke_tcol`, `duke_tcol_masked` | `vlineasm1`, `vlineasm4`, `mvlineasm1`, `mvlineasm4` | a wall's column, `*dest = pal[tex[v >>> sh]]`; the masked one leaves texel 255 out |
| 2 | `duke_hline` | `hlineasm4` | a floor's or ceiling's row, right to left |
| 3 | `duke_rhline` | `rhlineasm4` | the tilted screen's row |
| 4 | `duke_slope` | `slopevlin` | a sloped floor's column, 1/z from a float's bits every eight pixels |
| 5 | `duke_mulscale` | `mulscale` (fixedPoint_math.h) | `(int32_t)(((int64_t)a * b) >> s)` |
| 6 | `duke_udiv64` | `c4_udiv64` | a 64-bit quotient's low word, for `divscale` and `scale` |
| 7 | `duke_qinterp16` | `qinterpolatedown16short` | a row of shorts stepping in 16.16 |
| 8, 9 | `duke_clearb`, `duke_copyb` | `clearbufbyte`, `copybufbyte` | a byte fill of a word's pattern; a copy where the two do not overlap |

**Quake** (`games/quake/c4/accel.mjs`, `source/c4accel.h`; `QA_*`):

| op | macro | Quake's C | what it is |
|---|---|---|---|
| 0 | `qa_spans8` | `D_DrawSpans8` | a surface's whole span list: the perspective division every eight pixels in the game's floats, and the affine runs between |
| 1 | `qa_zspans` | `D_DrawZSpans` | the z buffer's whole span list |
| 2 | `qa_turb8` | `Turbulent8`, `D_DrawTurbulent8Span` | water and lava, the whole list, through the sine table |
| 3 | `qa_lblock` | `R_DrawSurfaceBlock8_mip0..3` | a surface-cache block, lit |
| 4 | `qa_sspan` | `D_SpriteDrawSpans` | a sprite's run against the z buffer |
| 5 | `qa_polyspans` | `D_PolysetDrawSpans8` | a model triangle's spans against the z buffer |

The span lists' operations read the game's `espan_t` and `spanpackage_t` as c4's 32-bit layout has
them, and the nine gradients from a copy the C makes (`d_scan.c`'s `c4_grad`).

## 3. In the games

**Duke Nukem 3D**: Options, Video Setup, **ACCELERATED** (on, off; "OFF" where the machine has none,
with "NOT ON THIS MACHINE" under the cursor). It is `Accelerated` in `duke3d.cfg`'s Screen Setup, on
unless that says 0. `accel_draw` (draw.c) is the first of Duke's numbers, or 0: the drawing loops and
fixedPoint_math.c's helpers ask it (`DUKE_ACCEL()`).

**Quake**: Options, **Video Options**, "Browser accelerated" (on, off, or n/a). It is the cvar
`vid_accel`, saved in `config.cfg`, on unless that says 0; `vid_null.c` reads it once a frame into
`d_accel`, the first of Quake's numbers or 0, which `d_scan.c`, `d_sprite.c`, `d_polyse.c` and
`r_surf.c` ask.

Doom, Wolf3D, ROTT and Tyrian keep their loops: they run at a good speed in the page now, with idle to
spare.

## 4. What they do

Before is `c4ix_and_toppings` 692eca0 (the games' C, as `--no-accel` runs it); after is this branch.
Quake's after also has `R_AddDynamicLights`' sum through int (section 4.3).

### 4.1 Node: instructions a frame

`node tools/framebench.mjs RUN` and `--no-accel`, M instructions, median / 99th percentile / worst.
Duke's demo1 without its death (frames 1949 to 2782).

| run | before | after |
|---|---|---|
| Duke, shareware script | 4.11 / 5.42 / 5.82 | 1.38 / 2.32 / 2.62 |
| Duke Atomic demo1, in play | 5.23 / 9.57 / 11.06 | 1.70 / 3.20 / 3.64 |
| Duke Atomic demo2 | 4.41 / 7.66 / 9.74 | 1.69 / 4.19 / 5.96 |
| Duke Atomic demo3 | 4.09 / 6.20 / 7.19 | 1.93 / 3.68 / 4.62 |
| Quake demo1 | 7.40 / 17.71 / 19.46 | 3.12 / 6.90 / 7.73 |
| Quake demo2 | 7.14 / 16.20 / 20.07 | 3.13 / 6.58 / 7.52 |
| Quake demo3 | 8.43 / 16.23 / 19.32 | 3.64 / 7.11 / 8.39 |

### 4.2 The page: frame rates

`framebench --page` on CDP 9222, 400 MHz, one CPU, `-show`, sound off; frames a second, average /
1% low / worst frame. The page's frame times are its display's refresh steps.

| run | MMU on, before | MMU on, after | MMU off, before | MMU off, after |
|---|---|---|---|---|
| Duke, shareware script | 28.0 / 17.9 / 67 ms | 60.4 / 29.9 / 34 ms | 34.7 / 18.0 / 83 ms | 75.5 / 29.9 / 33 ms |
| Duke Atomic demo2 | 26.9 / 13.4 / 100 ms | 53.4 / 19.6 / 67 ms | 33.9 / 16.5 / 67 ms | 64.7 / 23.9 / 50 ms |
| Duke Atomic demo3 | 28.9 / 15.6 / 67 ms | 51.7 / 23.3 / 66 ms | 36.7 / 3.2 / 1005 ms, one stall | 62.0 / 28.9 / 51 ms |
| Duke Atomic demo1, with its death | 17.4 / 9.9 / 117 ms | 47.4 / 25.7 / 66 ms | 22.5 / 12.0 / 84 ms | 56.8 / 28.6 / 67 ms |
| Quake demo1 | 13.1 / 5.7 / 233 ms | 26.0 / 12.0 / 84 ms | 16.5 / 7.2 / 150 ms | 32.8 / 14.3 / 100 ms |
| Quake demo2 | 13.9 / 6.1 / 183 ms | 27.2 / 11.5 / 117 ms | 17.4 / 7.6 / 150 ms | 33.9 / 15.0 / 67 ms |
| Quake demo3 | 12.2 / 6.2 / 184 ms | 23.9 / 11.2 / 117 ms | 15.3 / 7.4 / 150 ms | 29.1 / 14.3 / 83 ms |

The checksums, in the page with the MMU and the accelerators (`-- -sum 1`): Duke's shareware script
96ab05bb, Quake's demo1 ea3fefe7 and demo3 1f87d635, as gcc's build and the C make them. The two
machines on CDP here are not fast ones; a faster machine shows more of it, as the host does the work.

### 4.3 Where the frames go now

`--profile` with the accelerators, the heaviest 1% of frames against the median 10%.

- **Duke in play** (`--leave 1949-2782`): a median frame is 1.75 M, its heaviest 1% 4.11 M. `wallscan`
  (10% of a median frame), what is left of `mulscale` where its operands are small (6%),
  `dorotatesprite`, `drawalls`; the heavy frames grow in `inside` (the point-in-sector test, 14% of
  their growth), sprites (`drawsprite`), translucent walls (`tvlineasm2`, `thlineskipmodify`) and
  `florscan`. Game logic and the engine's arithmetic, more than drawing.
- **Quake**: a median frame 3.12 M, its heaviest 1% 7.13 M. A median frame is now the edges and
  surfaces (`R_RecursiveWorldNode`, `R_LeadingEdge`, `R_ClipEdge`, `R_RenderFace`, `R_TrailingEdge`,
  `R_EmitEdge`: 43%) and the models' edge setup (`D_PolysetScanLeftEdge`); its heaviest frames are
  explosions: `R_AddDynamicLights`' arithmetic (14% of their growth), particles (`D_DrawParticle`,
  `R_DrawParticles`, `R_ParticleExplosion`: 27%), `R_BuildLightMap`.
- `R_AddDynamicLights` added a float to an unsigned, which went through the library both ways
  (`__c4f_from_ull_s`, `__c4f_to_ull`, long long shifts), some 380 instructions a texel, a fifth of the
  explosions' growth; it now converts through int, which c4's FPU does in one instruction, wherever
  the sum is below 2^31, which a light always is. The same lights, the same frames (games/quake f3ac7ad).
- Next, if wanted, in the same way: Quake's particles (`D_DrawParticle`, each a few pixels against the
  z buffer) and dynamic lights (`R_AddDynamicLights`, `R_BuildLightMap`), Duke's `inside` and
  translucent walls. The edges are float geometry over the BSP, more of the game than a loop.

## 5. Tests

- Each game's `c4/acceltest.c` holds its operations against the game's own C, on made-up textures,
  palettes, steps and shifts, comparing every byte written and what is left behind: Duke's 2,200
  cases, Quake's surface blocks and sprite runs 400. test-duke3d and test-quake build them (in
  `.duke3d/test/`, `.quake/test/`) and run them first.
- test-duke3d and test-quake hold the games' c4 frames to gcc's host build, frame by frame, on the
  bare machine, under C4DOS and inside C4IX; libjs runs them with the accelerators, so every one of
  those frames is drawn by them, Quake's span lists included. test-duke3d's menu run and its General
  MIDI run go without (`--no-accel`): gcc's build has no accelerators, so its Video Setup says OFF,
  and the music's time is the machine's.
- `libjs/tests/src/accel.c` (`libjs-accel.c4r`) tests the loader: INFO, a name nobody ships found as 0,
  each listed game as a number of its own; test-libjs runs it, and again with `--no-accel`.
- framebench's `--no-accel` gives the same checksums with the games' C. Its `--profile` compares the
  frames both of its runs made: a run stops at the end of the slice in which the next demo began,
  97 instructions while profiling and 2 M otherwise.

## 6. Adding one

A loop belongs in a game's module when it is much of a frame a player sees and plain enough to be the
same, to the bit, on the host. The rules the ones here keep:

- the operation is the module's, a function of the machine in its array; its number is its place,
  which the game's header names; libjs changes for none of it;
- only the arguments and memory the guest could read, through the kit's `reader`, `writer`,
  `reader32` and `writer32`, never the arena directly, so the MMU and the guard hold; only the writes
  its loop makes, in its order;
- the same result run again from the start after a fault;
- floats through `Math.fround` at every single operation and the kit's `toInt`, doubles as doubles;
- the game keeps its C for every other machine, asks `accel_find()` once, and tests the operation
  against its C in its own `c4/acceltest.c`.

Another machine need do nothing: INFO says nothing of the loader there. A machine that wanted the
games' accelerators (c4bb's simulator, which is JavaScript too) would load the same modules with the
same kit.
