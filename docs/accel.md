# Drawing accelerators

`libjs/accel.js` runs a game's innermost drawing loops on the host: a wall's column, a floor's row,
a Quake span, surface block or sprite run, each one instruction however many pixels. Duke Nukem 3D and Quake
use them when the machine has them and their video menu says so, and draw with their own C loops
everywhere else. The frames are the same either way, to the byte.

**In short** (2026-11-15, section 4):
- In the page (400 MHz, the MMU on), Duke's Atomic demos average 42 to 44 fps in play where they
  averaged 27 to 29, their slowest 1% of frames 16 to 19 where they were 13 to 16; the shareware
  script 49.6 where it was 28.0. Quake's demos average 17 to 19 where they averaged 12 to 14, their
  slowest 1% 9 to 10 where they were about 6. Without the MMU, Duke reaches the display's 60 Hz.
- On node's bare machine, a median frame is about half the instructions it was (Duke 4.1-5.2 M to
  1.9-2.5 M; Quake 7.1-8.4 M to 4.7-5.4 M) and the heaviest 1% of Quake's about half (16-18 M to 9 M).
- The same frames: every checksum is the one it was, with the accelerators and without, in node and
  in the page under the MMU; test-duke3d and test-quake match gcc's build frame for frame.
- Each game's video menu has the switch, on unless turned off, saved in its configuration.
- Duke's demo1 dies at frames 1949 to 2782, a desync between the game's versions rather than play,
  and its tilted, floor-filled view there is left out of these numbers (the accelerators take it too).

## 1. What they are

The measurements in `docs/framebench.md` (sections 3 and 4) found that the frames a player sees as
choppy are drawing: the median frame is about 40% pixel loops, the heaviest 1% far more. On c4's
stack machine a textured pixel is 15 to 70 instructions; done by the host it is a few nanoseconds.

- **Their own numbers.** Opcodes `0x1000` to `0x10ff` (`ACC_BASE`, `ACC_END` in `libjs/c4m.js`) are
  the accelerators', outside the instruction set (0 up, append-only), C4KE's services (128 up) and
  C4IX's syscalls (200 up). No compiler and no other machine knows them. A program reaches one
  through `OPCD` alone: `__c4_opcode(args..., ACC_X)`, the arguments pushed as for a call.
- **Announced, never probed.** INFO bit `C4I_ACCEL` (`0x100000`) says the machine has them. libjs
  has them unless made without (`createMachine({ accel: false })`, `cli.js --no-accel`, the page's
  `&accel=0`, `framebench --no-accel`). C4IX passes INFO to its tasks as it is, and a CPU on a thread
  (`ap.js`) has them when CPU 0 does, so a task sees the same wherever it runs. On native c4m, c4bb,
  C4DOS's own machines and libjs without them, the bit is clear and the program keeps its loop; there
  the numbers would be custom opcodes nobody installed (`TRAP_ILLOP`), and are never reached.
- **The same pixels.** Each accelerator is its loop, step for step: the same reads, the same writes in
  the same order, integer arithmetic wrapping at 32 bits as c4's does, shifts taken modulo 32 as `SHL`
  and `LSR` take them, a single float's sum as `Math.fround` of the double one, as c4's FPU and soft
  float make it. What the loop leaves behind (Duke's `vplce[]`, `fpuasm` and `asm4`) it leaves too.
- **The MMU.** Every address goes through the task's pages, one page cached at a time for each of the
  accelerator's streams; an address the task may not use faults as the loop's own load or store would,
  and once the kernel has seen to it the instruction runs again from the start. Drawing the same
  pixels twice draws the same picture, so that is safe. Without the MMU it reads the arena directly,
  above the low page and short of the boot stack's guard, and takes `ld8`/`st8` for anything else.
- **One instruction.** It counts one cycle however many pixels it draws, as `MCPY` does: the point is
  that the machine's clock (400 MHz in the page) is not spent on them.
- **The guest's side** is `libjs/guest/accel.h`: `accel_present()` and a macro an accelerator. Only
  c4lc's builds ask (`__c4_inttypes__`); built by gcc (the games' host builds, which test-duke3d and
  test-quake hold the c4 builds to), `accel_present()` is 0 and the calls are never made.

## 2. The accelerators

| number | macro | the loop | arguments |
|---|---|---|---|
| `0x1000` | `accel_tcol` | Duke's walls, `vlineasm1`, `vlineasm4` | `n, v, inc, sh, tex, pal, dest, stride`: `*dest = pal[tex[v >>> sh]]`, `v += inc`, `dest += stride`; gives `v` |
| `0x1001` | `accel_tcol_masked` | masked walls, `mvlineasm1`, `mvlineasm4` | the same, a texel of 255 left out |
| `0x1002` | `accel_hline` | floors and ceilings, `hlineasm4` | `n, a, b, da, db, sh1, bits, sh2, tex, pal, dest`: right to left, `pal[tex[((a >>> sh1) << bits) \| (b >>> sh2)]]` |
| `0x1003` | `accel_rhline` | the tilted screen, `rhlineasm4` | `n, tex, pal, p, c, s, x, y, da, db, ax, bx`, as draw.c keeps them |
| `0x1004` | `accel_slope` | sloped floors, `slopevlin` | its 18 values from the first eight pixels on, the floats as their bits |
| `0x1010` | `accel_span` | Quake's texture spans, `D_DrawSpans8`'s runs | `n, s, t, sstep, tstep, pbase, width, dest`: `*dest++ = pbase[(s >> 16) + (t >> 16) * width]` |
| `0x1011` | `accel_zspan` | z spans, `D_DrawZSpans` | `n, izi, izistep, dest`: `*dest++ = izi >> 16` as shorts |
| `0x1012` | `accel_lblock` | surface blocks, `R_DrawSurfaceBlock8_mip0..3` | `rows, sh, ll, lr, lls, lrs, src, srcstep, cm, dest, deststep`: a whole block |
| `0x1013` | `accel_sspan` | sprites, `D_SpriteDrawSpans`' runs | `n, s, t, sstep, tstep, pbase, width, dest, pz, izi, izistep`: a texel not 255 drawn where `*pz <= izi >> 16`, which it sets; gives `izi` |

`D_DrawSpans8` keeps its perspective division in C, every eight pixels, and hands each run of up to
eight to `accel_span`; the span's floats stay the game's. The others take a whole column, row, span or
block.

## 3. In the games

**Duke Nukem 3D**: Options, Video Setup, **ACCELERATED** (on, off; "OFF" where the machine has none,
with "NOT ON THIS MACHINE" under the cursor). It is `Accelerated` in `duke3d.cfg`'s Screen Setup, on
unless that says 0. `Engine/src/draw.c` asks in `vlineasm1`, `mvlineasm1`, `vlineasm4`, `mvlineasm4`,
`hlineasm4`, `rhlineasm4` and `slopevlin` (`accel_draw`, which the game sets through `accel_set`).

**Quake**: Options, **Video Options**, "Browser accelerated" (on, off, or n/a). It is the cvar
`vid_accel`, saved in `config.cfg`, on unless that says 0; `vid_null.c` reads it once a frame into
`d_accel`, which `D_DrawSpans8`, `D_DrawZSpans`, `R_DrawSurfaceBlock8_mip*` and `D_SpriteDrawSpans` ask.

Doom, Wolf3D, ROTT and Tyrian keep their loops: they run at a good speed in the page now, with idle to
spare.

## 4. What they do

Before is `c4ix_and_toppings` 692eca0 (the loops, as `--no-accel` draws them); after is this
branch. Quake's after also has `R_AddDynamicLights`' sum through int (section 4.3).

### 4.1 Node: instructions a frame

`node tools/framebench.mjs RUN` and `--no-accel`, M instructions, median / 99th percentile / worst.
Duke's demo1 without its death (frames 1949 to 2782).

| run | before | after |
|---|---|---|
| Duke, shareware script | 4.11 / 5.42 / 5.82 | 1.87 / 3.25 / 3.63 |
| Duke Atomic demo1, in play | 5.23 / 9.57 / 11.06 | 2.26 / 4.31 / 4.94 |
| Duke Atomic demo2 | 4.41 / 7.66 / 9.74 | 2.25 / 5.40 / 7.32 |
| Duke Atomic demo3 | 4.09 / 6.20 / 7.19 | 2.48 / 4.60 / 5.53 |
| Quake demo1 | 7.40 / 17.71 / 19.46 | 4.87 / 8.89 / 9.58 |
| Quake demo2 | 7.14 / 16.20 / 20.07 | 4.67 / 8.96 / 9.93 |
| Quake demo3 | 8.43 / 16.23 / 19.32 | 5.36 / 9.18 / 10.47 |

### 4.2 The page: frame rates

`framebench --page` on CDP 9222, 400 MHz, one CPU, `-show`, sound off; frames a second, average /
1% low / worst frame. The page's frame times are its display's, 16.7 ms steps, and a game drawing
faster than 60 frames a second shows 60.

| run | MMU on, before | MMU on, after | MMU off, before | MMU off, after |
|---|---|---|---|---|
| Duke, shareware script | 28.0 / 17.9 / 67 ms | 49.6 / 25.6 / 50 ms | 34.7 / 18.0 / 83 ms | 61.1 / 29.8 / 34 ms |
| Duke Atomic demo2 | 26.9 / 13.4 / 100 ms | 43.7 / 16.0 / 67 ms | 33.9 / 16.5 / 67 ms | 54.1 / 19.6 / 67 ms |
| Duke Atomic demo3 | 28.9 / 15.6 / 67 ms | 42.3 / 18.8 / 67 ms | 36.7 / 3.2 / 1005 ms, one stall | 52.6 / 25.8 / 50 ms |
| Duke Atomic demo1, with its death | 17.4 / 9.9 / 117 ms | 39.9 / 19.8 / 66 ms | 22.5 / 12.0 / 84 ms | 49.1 / 24.7 / 50 ms |
| Quake demo1 | 13.1 / 5.7 / 233 ms | 18.5 / 9.5 / 133 ms | 16.5 / 7.2 / 150 ms | 23.0 / 11.5 / 117 ms |
| Quake demo2 | 13.9 / 6.1 / 183 ms | 19.2 / 9.7 / 117 ms | 17.4 / 7.6 / 150 ms | 23.7 / 11.5 / 100 ms |
| Quake demo3 | 12.2 / 6.2 / 184 ms | 17.2 / 9.2 / 117 ms | 15.3 / 7.4 / 150 ms | 22.0 / 12.0 / 84 ms |

The checksums, in the page with the MMU and the accelerators (`-- -sum 1`): Duke's shareware script
96ab05bb, Quake's demo1 ea3fefe7 and demo3 1f87d635, as gcc's build and the loops make them.

### 4.3 Where the frames go now

`--profile` with the accelerators, the heaviest 1% of frames against the median 10%.

- **Duke in play** (`--leave 1949-2782`): a median frame is 2.32 M, its heaviest 1% 5.25 M. Neither is
  drawing loops any more: the engine's arithmetic (`c4_mulscale`, `c4_udiv64`, `__c4ll_mul`), `wallscan`,
  sprites (`drawsprite`, `dorotatesprite`), `inside` and the actors. Without the accelerators the same
  frames were 4.53 and 9.58 M, and their growth was `slopevlin` (48%), `rhlineasm4` (37%, the view
  tilting as the player is hit just before the death) and `vlineasm4` (10%).
- **Quake**: a median frame 4.89 M, its heaviest 1% 9.18 M, explosions: `R_AddDynamicLights`'
  arithmetic, particles (`R_DrawParticles`, `D_DrawParticle`, `R_ParticleExplosion`), `R_BuildLightMap`,
  the edges, and `D_DrawSpans8`'s perspective division every eight pixels. Until this branch a fifth of
  those frames' growth was `R_AddDynamicLights` adding a float to an unsigned, which went through the
  library both ways (`__c4f_from_ull_s`, `__c4f_to_ull`, long long shifts), some 380 instructions a
  texel; it now converts through int, which c4's FPU does in one instruction, wherever the sum is
  below 2^31, which a light always is. The same lights, the same frames (games/quake f3ac7ad).
- Left for later, if wanted: Quake's water (`D_DrawTurbulent8Span`, 4% of a median frame where there is
  water), its models' spans (`D_PolysetDrawSpans8`, 7%) and particles are the next loops of the same
  kind; the rest of both games' frames is arithmetic and game logic.

## 5. Tests

- `libjs/tests/src/accel.c` (built as `src/c4bb/images/libjs-accel.c4r`, the games' way: c4lc,
  `-mfuse`, the bare libc) runs each accelerator against its loop as the game has it, 1,600 cases of
  made-up textures, palettes, steps and shifts, comparing every byte of the buffer and what is left
  behind. test-libjs runs it, and again with `--no-accel`, where it must say "accel: not fitted".
- test-duke3d and test-quake hold the games' c4 frames to gcc's host build, frame by frame; libjs runs
  them with the accelerators, so every one of those frames is drawn by them. framebench's `--no-accel`
  gives the same checksums with the loops.

## 6. Adding one

An accelerator belongs here when a loop is most of a frame a player sees and is plain enough to be
the same, pixel for pixel, on the host. The rules the ones here keep:

- a number in `0x1000`..`0x10ff`, a case in `accel()`, a macro in `accel.h`, a row in section 2;
- only the arguments and memory the guest could read; only the writes its loop makes, in its order;
- `reader`, `writer`, `reader32` for memory, never `u8` directly, so the MMU and the guard hold;
- safe to run again from the start after a fault (no read of what it wrote);
- the game keeps its loop for every other machine, and asks `accel_present()` once.

Another machine need do nothing: the bit is clear there. A machine that wanted them (c4bb's
simulator, native c4m) would implement the same nine loops behind the same bit, some 200 lines of
C or JavaScript, and its own test is `libjs-accel.c4r`.
