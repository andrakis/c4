# Accelerators

A game can hand its heaviest loops to the host, in its own JavaScript: a wall's column, a floor's
row, Quake's whole span lists, each one instruction however much it does. `libjs/accel.js` is the
loader and the kit those modules use; the modules are the games' own, `games/duke3d/c4/accel.mjs`,
`games/quake/c4/accel.mjs` and `games/rott/c4/accel.mjs`, and libjs knows no game. Duke Nukem 3D,
Quake and Rise of the Triad use theirs when the machine has them and their menu says so, and run
their own C everywhere else. The frames are the same either way, to the byte.

**In short** (2026-11-15, section 4):
- In the page (400 MHz, the MMU on), Quake's demos average 26 to 28 fps where they averaged 12 to 14,
  their slowest 1% of frames 12 to 14 where it was about 6; Duke's Atomic demos average 52 to 53 in
  play where they averaged 27 to 29, their slowest 1% 20 to 23 where it was 13 to 16, and the
  shareware script 60 where it was 28. Without the MMU, Quake 33 to 35, Duke 62 to 75.
- Quake's Video Options has Resolution, 320x200 to 1024x768 (section 3).
- On node's bare machine a median frame is a third of the instructions it was or less (Duke 4.1-5.2 M
  to 1.4-1.9 M; Quake 7.1-8.4 M to 2.9-3.2 M), and Quake's heaviest 1% about a third (16-18 M to 6 M).
- The same frames: every checksum is the one it was, with the accelerators and without, in node and
  in the page under the MMU; test-duke3d and test-quake match gcc's build frame for frame.
- Each game's video menu has the switch, on unless turned off, saved in its configuration.
- Duke's demo1 dies at frames 1949 to 2782, a desync between the game's versions rather than play,
  and is left out of these numbers.
- Rise of the Triad (2026-11-17, section 4.4): on node its four timedemos' frames are 1.3 to 1.4 M
  instructions on average where they were 3.6 to 4.2 M, and the worst 2.6 to 2.9 M where it was
  11.5 M. 0.89 M of each is the timedemo's own checksum, which play does not do: a frame a player
  sees is a fifth or less of what it was. Its switch is Extended User Options' ACCELERATED.
- C4IX has its own (section 7, 2026-11-17): libarc's CRC-32 and inflater, which unpack the root
  archive at boot. A boot to the desktop was 1,592 M instructions, 85% of them those two loops; it is
  now 202 M, and the page's boot 24 s to 10 s. A machine option of its own (`&osaccel=0`,
  `--no-osaccel`), which the desktop's Settings turns too.

## 1. How it works

The measurements in `docs/framebench.md` (sections 3 and 4) found that the frames a player sees as
choppy are drawing: the median frame was about 40% pixel loops, the heaviest 1% far more. On c4's
stack machine a textured pixel is 15 to 70 instructions; done by the host it is a few nanoseconds.

- **The game's own JavaScript.** A game that wants accelerators ships `games/NAME/c4/accel.mjs`,
  whose default export is given libjs's kit and returns an array of operations, functions of the
  machine. Its C side names them by their place in that array (`c4/c4accel.h`, `source/c4accel.h`).
  libjs knows no game and no loop.
- **Which modules.** build-images.sh lists the games that ship one in `src/c4bb/images/accel.json`,
  then an operating system's (`src/NAME/accel.mjs`, listed as `os/NAME`: C4IX's, section 7).
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
  The kit's `span(m, addr, len, write)` gives a stretch as a view of the arena's own bytes when it is
  one run of them (plain memory, or within one page under the MMU), and null otherwise: a copy goes a
  page at a time as one typed-array move where it can, byte by byte where it cannot.
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
| 10, 11 | `duke_tvline1`, `duke_tvline2` | `tvlineasm1`, `tvlineasm2` | translucent walls (glass): one column, or two side by side, blended through `transluc` |
| 12 | `duke_tsprite` | `DrawSpriteVerticalLine` | a translucent sprite's column |
| 13 | `duke_thline` | `thlineskipmodify` | a translucent floor sprite's row |
| 14 | `duke_scale` | `scale` (fixedPoint_math.h) | `(int32_t)((int64_t)a * b / c)`, any a, b and c but 0 |
| 15 | `duke_dmulscale` | `dmulscale` | `(int32_t)(((int64_t)a * b + (int64_t)c * d) >> s)` |
| 16 | `duke_copyr` | `copybufreverse` | a copy read backwards, where the two do not overlap: a mirror's rows turned round (`completemirror`), every row of every frame a mirror is on the screen |
| 17 | `duke_mhline` | `mhlineskipmodify` | a masked floor's or ceiling's row, left to right, texel 255 left out: a floor-aligned sprite, and the masked floors and ceilings `ceilspritehline` draws |

The translucent loops read the screen and blend into it, and a blend run twice blends twice: each
works out every pixel first, reading all it reads and making sure with the kit's `probe` that every
page it will write is there, and only then writes.

**Quake** (`games/quake/c4/accel.mjs`, `source/c4accel.h`; `QA_*`):

| op | macro | Quake's C | what it is |
|---|---|---|---|
| 0 | `qa_spans8` | `D_DrawSpans8` | a surface's whole span list: the perspective division every eight pixels in the game's floats, and the affine runs between |
| 1 | `qa_zspans` | `D_DrawZSpans` | the z buffer's whole span list |
| 2 | `qa_turb8` | `Turbulent8`, `D_DrawTurbulent8Span` | water and lava, the whole list, through the sine table |
| 3 | `qa_lblock` | `R_DrawSurfaceBlock8_mip0..3` | a surface-cache block, lit |
| 4 | `qa_sspan` | `D_SpriteDrawSpans` | a sprite's run against the z buffer |
| 5 | `qa_polyspans` | `D_PolysetDrawSpans8` | a model triangle's spans against the z buffer |
| 6 | `qa_particles` | `R_DrawParticles`, `D_DrawParticle` | every particle drawn and moved, once the C has freed the dead |
| 7, 8, 9 | `qa_lmfill`, `qa_dlight`, `qa_lmfin` | `R_BuildLightMap`, `R_AddDynamicLights` | a surface's light: ambient and lightmaps; one dynamic light's texels; bound and shift |
| 10 | `qa_aliasverts` | `R_AliasTransformAndProjectFinalVerts` | a model's vertices transformed, projected and lit |
| 11 | `qa_polytri` | `D_PolysetRecursiveTriangle` | a small triangle split to its pixels against the z buffer |
| 12 | `qa_sincos` | `AngleVectors`' `sin` and `cos` | an angle's sine and cosine as floats, by the browser's `Math.sin` and `Math.cos`: within a double's last bit of libc's, which a float hardly ever keeps (test-quake's frames are gcc's still) |

The span lists' operations read the game's `espan_t` and `spanpackage_t` as c4's 32-bit layout has
them, and the nine gradients from a copy the C makes (`d_scan.c`'s `c4_grad`); the particles, the
models and the small triangles read what they need from a struct the C fills (`c4_pp`, `c4_av`,
`c4_tri`). The particles' moves, a dynamic light's sums and the light maps' last pass cannot be run
twice, so they first make sure, with the kit's `probe`, that nothing they write can fault: under the
MMU a page that is not there faults then, before anything is written.

**Rise of the Triad** (`games/rott/c4/accel.mjs`, `c4/c4accel.h`; `RA_*`):

| op | macro | ROTT's C | what it is |
|---|---|---|---|
| 0 | `rott_col` | `R_DrawColumn`, `R_DrawClippedColumn`, `R_DrawWallColumn` (rt_scale.c) | a column, `*dest = shade[src[frac >> sh]]`, the shift signed or not as each has it |
| 1 | `rott_trans` | `R_TransColumn` | a translucent column, `*dest = shade[*dest]` |
| 2 | `rott_row` | `DrawRow` (rt_floor.c) | a floor's or ceiling's row out of its 128x128 tile |
| 3 | `rott_rotrow` | `DrawRotRow`, `DrawMaskedRotRow` (rt_draw.c) | the turned screen's row (the player's death, a level's start), out of its image, at 320, 640 or 800 across, each size's bounds and shift as arguments; the masked one leaves 255 out |
| 4 | `rott_sky` | `DrawSkyPost` | the sky's column; above 320 across a column taller than the sky's 400 goes back over it, as the port's does |
| 5, 6 | `rott_fixmul`, `rott_fixmulshift` | `FixedMul`, `FixedMulShift` (watcom.c) | `(int32_t)(((int64_t)a * b + 0x8000) >> 16)`; `(int32_t)((uint64_t)((int64_t)a * b) >> s)` |
| 7, 8 | `rott_fixdiv2`, `rott_fixscale` | `FixedDiv2`, `FixedScale` | `(int32_t)((int64_t)a * 65536 / b)`; `(int32_t)((int64_t)a * b / c)`, each through c4's `__int64` library in the C |
| 9 | `rott_tile` | `DrawTiledRegion` (rt_vid.c) | a region tiled with a pic's four planes: the border behind a small view, a message's line |
| 10 | `rott_post` | `ScaleMaskedPost`, `ScaleClippedPost`, `ScaleTransparentPost`, `ScaleTransparentClippedPost` | a sprite's post, every run of it, solid and translucent, leaving `dc_yl`, `dc_yh` and `dc_source` as the C does |

`watcom.h` makes the four fixed-point helpers macros that choose the operation where they stand, as
Duke's `mulscale` does (`FixedMul` was 1,100 calls a frame, and `FixedScale` and `FixedDiv2` went
through the 64-bit library's division); the functions are there for every other machine and until
the game has asked. The translucent column and posts work out every pixel and make sure of the pages
they write before writing, as Duke's translucent loops do.

## 3. In the games

**Duke Nukem 3D**: Options, Video Setup, **ACCELERATED** (on, off; "OFF" where the machine has none,
with "NOT ON THIS MACHINE" under the cursor). It is `Accelerated` in `duke3d.cfg`'s Screen Setup, on
unless that says 0. `accel_draw` (draw.c) is the first of Duke's numbers, or 0: the drawing loops and
fixedPoint_math.c's helpers ask it (`DUKE_ACCEL()`).

**Quake**: Options, **Video Options**, "Browser accelerated" (on, off, or n/a). It is the cvar
`vid_accel`, saved in `config.cfg`, on unless that says 0; `vid_null.c` reads it once a frame into
`d_accel`, the first of Quake's numbers or 0, which `d_scan.c`, `d_sprite.c`, `d_polyse.c` and
`r_surf.c` ask.

Quake's Video Options also has **Resolution**: 320x200 (the original's, and what a timedemo's
checksums and the tests are made at), 320x240, 400x300, 512x384, 640x400, 640x480, 800x600 and
1024x768, the cvar `vid_mode` in `config.cfg`, taking at the next frame (`vid_null.c`'s
`VID_C4SetMode`: the surface cache made again, the view and the console worked out again). The
buffers for a frame bigger than 320x200 are made the first time one is chosen, so a machine that keeps
320x200 (C4DOS's, which has little memory to spare) pays nothing for them; where there is no room the
game stays at 320x200 and says so. Each frame goes to the display at its own size, and the display
becomes the frame's size above 640x480 (`qg-gui.c`, writing `GUI_W` and `GUI_H`). The menus and the
console keep Quake's 8-pixel font, so they are small at 1024x768, as they were in the original.

**Rise of the Triad**: Options, Extended User Options, **ACCELERATED** (its button on or
off; the item is greyed out where the machine has none). It is `BrowserAccelerated` in the game's
config, on unless that says 0. `rott_accel` (rt_scale.c) is the first of ROTT's numbers, or 0: the
columns, posts, rows, sky, turned screen, tiled regions and watcom.h's macros ask it
(`ROTT_ACCEL()`).

ROTT's Extended User Options also has **RESOLUTION**: 320x200 (the original's, and what the
timedemos and tests are made at), 640x480 and 800x600, the port's own sizes, taking at once and kept
in the config (`ScreenWidth`, `ScreenHeight`). The display becomes the frame's size above 640x480,
as Quake's does. Demo 4 at 640x480 and 800x600 gives the same frames with the accelerators, without
them and in gcc's build.

Doom, Wolf3D and Tyrian keep their loops: they run at a good speed in the page now, with idle to
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
| Quake demo1 | 7.40 / 17.71 / 19.46 | 2.91 / 5.55 / 6.12 |
| Quake demo2 | 7.14 / 16.20 / 20.07 | 2.94 / 6.16 / 6.50 |
| Quake demo3 | 8.43 / 16.23 / 19.32 | 3.24 / 5.88 / 6.56 |
| ROTT demo 1 | 3.90 / 5.28 / 11.52 | 1.37 / 1.62 / 2.69 |
| ROTT demo 2 | 3.53 / 5.08 / 11.52 | 1.29 / 1.57 / 2.58 |
| ROTT demo 3 | 3.72 / 5.44 / 11.52 | 1.36 / 1.74 / 2.90 |
| ROTT demo 4 | 4.17 / 5.44 / 11.52 | 1.55 / 1.77 / 2.79 |

ROTT's before is `c4ix_and_toppings` 38e0fe1 (2026-11-17), its runs `rott-demo1` to `rott-demo3`
and `rott`, and each of its figures has the timedemo's own checksum of the frame in it, 0.89 M,
which play does not do (section 4.4).

### 4.2 The page: frame rates

`framebench --page` on CDP 9222, 400 MHz, one CPU, `-show`, sound off; frames a second, average /
1% low / worst frame. The page's frame times are its display's refresh steps.

| run | MMU on, before | MMU on, after | MMU off, before | MMU off, after |
|---|---|---|---|---|
| Duke, shareware script | 28.0 / 17.9 / 67 ms | 60.4 / 29.9 / 34 ms | 34.7 / 18.0 / 83 ms | 75.5 / 29.9 / 33 ms |
| Duke Atomic demo2 | 26.9 / 13.4 / 100 ms | 53.4 / 19.6 / 67 ms | 33.9 / 16.5 / 67 ms | 64.7 / 23.9 / 50 ms |
| Duke Atomic demo3 | 28.9 / 15.6 / 67 ms | 51.7 / 23.3 / 66 ms | 36.7 / 3.2 / 1005 ms, one stall | 62.0 / 28.9 / 51 ms |
| Duke Atomic demo1, with its death | 17.4 / 9.9 / 117 ms | 47.4 / 25.7 / 66 ms | 22.5 / 12.0 / 84 ms | 56.8 / 28.6 / 67 ms |
| Quake demo1 | 13.1 / 5.7 / 233 ms | 27.8 / 13.6 / 83 ms | 16.5 / 7.2 / 150 ms | 34.8 / 14.6 / 83 ms |
| Quake demo2 | 13.9 / 6.1 / 183 ms | 28.4 / 12.8 / 100 ms | 17.4 / 7.6 / 150 ms | 35.4 / 14.0 / 84 ms |
| Quake demo3 | 12.2 / 6.2 / 184 ms | 26.3 / 12.2 / 84 ms | 15.3 / 7.4 / 150 ms | 32.7 / 14.0 / 100 ms |
| Quake demo1 at 640x480 | | 15.9 / 8.2 / 150 ms | | |
| Quake demo1 at 1024x768 | | 10.2 / 4.4 / 300 ms | | |

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
- **Quake**: a median frame 3.0 M, its heaviest 1% 6.2 M. With the particles, the light maps and the
  models' vertices and triangles the machine's, explosions are no longer what the heaviest frames are:
  they are views with many edges and models close to: the edges and surfaces (`R_EmitEdge`,
  `R_LeadingEdge`, `R_TrailingEdge`, `R_RenderFace`, `R_ClipEdge`, `R_GenerateSpans`: a third of their
  growth, and 43% of a median frame), `TransformVector`, `RecursiveLightPoint`, and libc's `ceil` and
  `trunc`, which the edges call.
- `R_AddDynamicLights` added a float to an unsigned, which went through the library both ways
  (`__c4f_from_ull_s`, `__c4f_to_ull`, long long shifts), some 380 instructions a texel, a fifth of the
  explosions' growth; it now converts through int, which c4's FPU does in one instruction, wherever
  the sum is below 2^31, which a light always is. The same lights, the same frames (games/quake f3ac7ad).
- Next, if wanted: Quake's edges (`R_ScanEdges` and what it calls) are the game's rasterizer, float
  geometry and linked lists over the BSP, a much larger thing to carry over than a loop; `ceil` and
  `trunc` as FPU operations would help every float program. Duke's `inside` and translucent walls are
  loops of the kind done here.

### 4.4 Rise of the Triad

The shareware's four timedemos on node's bare machine, M instructions a frame of the game's own
frames (framebench counts some 130 calls of the frame function for the loading bar at each demo's
start, which the game leaves out of its count and its checksum), mean / median / 99th percentile /
worst:

| demo | frames, checksum | before | after |
|---|---|---|---|
| 1 | 1,481, b0fe23ad | 4.04 / 3.90 / 5.28 / 11.52 | 1.37 / 1.37 / 1.62 / 2.69 |
| 2 | 745, 9460844c | 3.57 / 3.53 / 5.08 / 11.52 | 1.26 / 1.29 / 1.57 / 2.58 |
| 3 | 946, 3e81c403 | 3.74 / 3.72 / 5.44 / 11.52 | 1.35 / 1.36 / 1.74 / 2.90 |
| 4 | 475, 4c959773 | 4.17 / 4.17 / 5.44 / 11.52 | 1.44 / 1.55 / 1.77 / 2.79 |

- Each figure has the timedemo's checksum of the frame in it, 0.89 M (`rott_frame`), which play does
  not do: without it a median frame is 0.4 to 0.7 M where it was 2.6 to 3.3 M, a fifth or less, and
  the worst 1.7 to 2.0 M where it was 10.6 M.
- Before, a median frame was the walls' columns (`R_DrawWallColumn`, a quarter of it), the floors'
  and ceilings' rows (`DrawRow`, a fifth), sprites' columns, the sky, and `FixedMul` (1,100 calls)
  with `FixedScale` and `FixedDiv2` in the 64-bit library's division. The heaviest frames were the
  turned screen (`DrawRotRow`, the player's death and a level's start), a full-screen
  `DrawTiledRegion`, and sprites close to.
- After, the heaviest 1% grow over the median in the status bar's and border's redraws
  (`SetBorderColor`, `DrawPPic`, `VL_MemToScreen`), which come once in a while, and in the
  intensity font of the messages; a median frame is the ray caster (`Cast`, `HitWall`,
  `Interpolate`), `DrawWallPost` and the sprites' set-up.
- Without the accelerators (`--no-accel`) a frame is what it was before, within 0.02 M: the
  macros' test of `rott_accel`.
- The same frames: the four checksums with the accelerators and without, with a sound card and
  without, and demo 4's inside C4IX under the MMU.

## 5. Tests

- Each game's `c4/acceltest.c` holds its operations against the game's own C, on made-up textures,
  palettes, steps and shifts, comparing every byte written and what is left behind: Duke's 4,200
  cases, Quake's surface blocks, sprite runs and light maps 1,000, ROTT's 7,200 (its posts with
  solid and translucent runs, and the fixed-point helpers against `__int64`'s). test-duke3d,
  test-quake and test-rott build them (in `.duke3d/test/`, `.quake/test/`, `.rott/test/`) and run
  them first.
- test-rott holds ROTT's demo 4 to gcc's build on the bare machine, under C4DOS and inside C4IX,
  and with a sound card, with the accelerators where the machine has them (C4DOS's has none).
- test-duke3d and test-quake hold the games' c4 frames to gcc's host build, frame by frame, on the
  bare machine, under C4DOS and inside C4IX; libjs runs them with the accelerators, so every one of
  those frames is drawn by them, Quake's span lists included. test-duke3d's menu run and its General
  MIDI run go without (`--no-accel`): gcc's build has no accelerators, so its Video Setup says OFF,
  and the music's time is the machine's.
- `libjs/tests/src/accel.c` (`libjs-accel.c4r`) tests the loader: INFO, a name nobody ships found as 0,
  each listed game as a number of its own; test-libjs runs it, and again with `--no-accel`.
- framebench's `--no-accel` gives the same checksums with the games' C (`--lines '^sum:'` prints the
  game's own sums: the Atomic demos' 31, every 250 frames, are the same either way). Its `--profile` compares the
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

## 7. C4IX's own

An operating system ships accelerators as a game does: `src/c4ix/accel.mjs`, listed in `accel.json`
as `os/c4ix` and asked for as `c4ix`; `src/c4ix/include/c4ix_accel.h` is its C side (`CX_*`,
`cx_find()`, `cx_os()`). libjs still knows no system and no loop.

**Where the time went.** `libjs/tests/try-osprof.mjs` charges each 97 instructions to the function
they ran in, kernel or user (the images found in memory, so its breakdown is without the MMU), in a
set of situations. On node, 400 MHz, 800x600, before:

| situation | instructions | where |
|---|---:|---|
| boot to the desktop's prompt | 1,592 M (1,624 M with the MMU) | libarc's `arc_crc32` 36.5%, its inflater (`codes`, `bits`, `decode`, `construct`) 49%: the root archive, 4.46 MB of gzip, 18.26 MB of tar; 32 instructions a byte for the CRC, 43 to inflate |
| a window dragged, 120 moves | 0.40 M a move | the kernel 43% (syscalls; `top` writing the console), `gui_text2` 8%, `gui_frame` 8% |
| System Monitor in front | 0.56 M a second | `gui_text2` 12%, syscalls 11%, `cpu_load` 7.5% |
| the Terminal's `cat` of 20 KB | 23.4 M, 1,100 a byte | `gui_text2` 27%, the text grid (`tg_feed`, `putc_`, `clear`, `row_text`) 36% |
| Files, a turn of the wheel | 0.55 M | `gui_text2` 25%, `gui_frame` 9% |
| the Text Editor, Page Down | 1.03 M | `gui_text2` 20%, `gui_frame` 16%, `gui_commit` 7% |
| the Start menu, opened and closed | 2.66 M | `gui_text2` 33%, `gui_frame` 13% |

The desktop draws through the display's command ring, so its rectangles, images and glyphs are the
host's already: what it spends is in making the commands, `gui_text2`'s characters packed into words
a byte at a time and each command's frame and commit, and each of those situations is 1 to 7 ms of a
400 MHz machine. `memcpy`, `memset` and `memcmp` are opcodes already (`MCPY`, `MSET`, `MCMP`), and
`strlen` and `strcmp` never reached 0.3%; the kernel's MMU and scheduler paths are not loops. So the
accelerators are the boot's two loops, and nothing of the desktop's: what would help it there is C
(`gui_text2`'s packing as one `memcpy`), not an accelerator.

**The operations** (`CX_*`):

| op | macro | the C | what it is |
|---|---|---|---|
| 0 | `cx_crc32` | `arc_crc32` | the CRC-32 of a buffer, from a CRC so far |
| 1 | `cx_inflate` | `codes()`'s loop | literals and matches, turn after turn, from the state in `cx_st` and back into it |

`codes()` asks for `CX_INFLATE` at the top of a turn while a whole turn can reach neither the
window's end (a match is at most 258 bytes) nor the input's (a turn reads fewer than 16 bytes); the
operation runs turns while that stays so, at most 64 KB of output and of input at a call, and stops
before a turn that would meet an edge, a bad code among them. Then a turn of the C, which flushes the
window, reads on or says what was wrong as it always did, and the operation again. It probes what it
will touch first (the state, the input, the window behind and the output ahead, the code tables), so
under the MMU a page that is not there faults before anything is done, and a run again from the start
is the same run. The gzip trailer's CRC checks every byte of a boot's unpacking.

**What it gained.** A boot to the desktop's prompt, node, 400 MHz: 1,608 M instructions before,
202 M after (what is left is mostly the desktop's own start: `wall_tiles` 38 M, the inflater's
`construct` 23 M, `gui_imgdef` 21 M). `bench-desktop` with the modules loaded: 9.98 s to 1.68 s,
and with the MMU 13.01 s to 2.31 s. The page in Chrome, `?persist=0`, from loading to the taskbar's
clock: 24.1 s to 10.0 s (1,612 M to 200 M cycles), and with the MMU 23.4 s to 9.4 s (1,643 M to
235 M).

**The switch.** The machine's option `osAccel` (`createMachine`, `cli.js --no-osaccel`, the page's
`&osaccel=0` and its Configuration dialog's "C4IX accelerators"; "Accelerators", `&accel=0`, turns off
every one, the games' too). A program reads and turns it with `ACC_OS` (`0x1001`, the loader's, beside
`ACC_FIND`): `__c4_opcode(v, ACC_OS)` gives it as it was, 3 on and 2 off where the machine has an OS
module (bit 1), and turns it on for v > 0, off for v == 0. While it is off `ACC_FIND` gives 0 for an
OS module, so libarc, which asks before each piece of work (`cx_find()`), runs its own C. It is one
word, shared with the CPUs on threads (`ap.js` adopts it with the module names), so a turn on one CPU
is every CPU's at once.

The desktop's **Settings** has it under the font: "Accelerators", On or Off ("Not on this machine"
where there are none), turned at once. The page sends it in its status, and when it differs from the
page's own the page writes it into its URL (`&osaccel=0`, or none for on): the next boot, and the
Configuration dialog, keep what Settings chose. Under node it lasts as long as the machine.

**Tests.** `libjs/tests/test-osaccel.mjs` holds both operations to zlib and to libarc's own C, carried
over line for line with its hand-over to `CX_INFLATE` (1,300 cases: zlib's levels and strategies,
windows and input buffers small enough to meet every edge, damaged streams that must fail with the
same message, CRCs whole and in two); `libjs-accel.c4r` turns the switch and finds `c4ix` on and off.
test-libjs runs both. Every C4IX boot under cli.js and the page unpacks its root archive with them,
and the archive's CRC must match.

