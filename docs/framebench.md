# Frame times: how the games' frames go

`tools/framebench.mjs` measures a game's frames, one table a run: the frame rate's low, average
and high, its 1% and 0.1% lows, the worst frame's time, and the VM instructions a frame, with the
worst frames' numbers. This page says how it measures, what it found on 2026-11-15, where the
heaviest frames' instructions go, whether hand-written code or a drawing opcode would do better
than c4lc's, and how long a key takes to show.

**In short** (2026-11-15, sections 2 to 6):
- In the page, with the MMU, Duke's Atomic demo1 averages 15.7 fps and its slowest 1% of frames 8.5;
  Quake's demos average 12 to 14 and their slowest 1% about 6. Without the MMU, 20% faster.
- The heaviest frames are two and a half times the median ones, and they are drawing of a kind the
  median frame hardly does: Duke's tilted screen when the player dies (`rhlineasm4`, the one inner loop
  not yet written for c4) and close sloped floors; Quake's surfaces lit again under dynamic lights,
  and explosions. Game logic does not grow.
- By hand, Duke's column loop is 13% shorter than c4lc's and a frame 2%. As one host instruction
  (`TCOL`, a prototype on `tcol-proto`) the loop is 16 to 22 times faster a pixel and the frame up to
  57% faster in the page, the same frames.
- Input lag is 1.5 to 2 frame times at every frame rate, with nothing buffered between: it is the
  frame rate.

## 1. How it measures

```
node tools/framebench.mjs [options] RUN [-- GAME ARGS...]
make framebench FB_RUN=RUN FB_ARGS="options"
```

`--list` prints the runs: `duke3d` (the Atomic Edition's three demos, from the person's own files
with `--data DIR`), `duke3d-sw` (test-duke3d's E1L1 script on the shareware), `quake-demo1` to
`quake-demo3`, `doom-demo1` to `doom-demo3`, `wolf3d` and `rott` (their timedemos, shareware).

**On node's bare machine** (the default). The game runs on its virtual clock (`-sum 0` for Duke and
Quake; a timedemo for the others), so every run of an image makes the same frames and counts the
same instructions. A frame ends where the game calls its frame function (`duke_sum_frame`,
`qg_sum_frame`, `dg_frame`, `c4sdl_present`, `rott_frame`). The tool catches the call itself: as
`libjs/c4m.js` loads, a module hook adds a look at the target of every `JSR`, `JSRI` and `JSRS`
(and of a `JMPA` that lands on a function's start, c4lc's call through a pointer), so a frame's count
is exact, to the instruction. Nothing in the tree changes, and if c4m.js's call
lines change the tool says so rather than measuring nothing. Frame rates are given at a host's
speed (`--mips`, default 100 and 250 M instructions a second: libjs runs 66 to 140 M in the page)
and by node's own clock, which shows the host's load too.

Frame 1 is the boot and is never counted. Each run leaves out its level's load, as frames that a
player does not play: Duke's demos the two frames after each demo opens (`opendemoread`, also where
one demo ends and the next begins), Quake's first nine (its console, then the map's load), Wolf3D's
second and ROTT's second and third. The table names what it left out, with their counts. `--skip N`
leaves out others.

**What a run counts besides drawing.** Duke and Quake with `-sum 0` checksum nothing. Doom, Wolf3D
and ROTT checksum every frame of a timedemo, and nothing turns it off: 0.84, 0.42 and 0.89 M
instructions a frame (`dg_frame`, `c4sdl_present`, `rott_frame`, by `--profile`), about a fifth of
each frame. It is in their counts and in their page runs, so they understate those games' speed.

**The numbers.** Frame times in ms. The *low* is the slowest frame's rate, the *high* the
fastest's, the *average* the frames over their time. A *1% low* is the rate of the slowest 1% of
frames taken together, at least one frame; the *0.1% low* the same for 0.1%. Instructions a frame:
minimum, median, 99th percentile and maximum, and the worst frames by number.

**`--profile`** runs the game a second time with its pc sampled every 97 instructions and every
call counted by its target, each charged to the frame being made. It compares the heaviest 1% of
frames with the median 10% (the middle of the frames sorted by instructions), by function: how
many M instructions a frame each takes in each group, how much it grows, and its calls a frame. It
checks that the second run made the same frames.

**`--page CDP`**: the page in a Chrome, through its DevTools port (`http://127.0.0.1:9222`), in a
tab of its own, closed at the end; it says if the tab count changed. The page is C4IX with
`--query` (default `mhz=400&mmu=1`), served from `--base` (an https proxy port, so that the page is
cross-origin isolated and the display takes its shared path). It stops the desktop for the shell
and types the game there with `-show`, so that the game draws on CPU 0, as a person's does. The
tool wraps the display's `drawCoalesced` and times every batch of commands it draws, a frame the
person sees, and counts the presents in it. The page presents more often than the game makes frames
(a palette shown on its own presents too, and Duke's flashes when the player is hit are palettes:
9,177 presents for 7,901 frames in the Atomic demos), so the presents are not the game's frames. A
run starts after its level's load, found as the longest wait for a batch where the start should be
(each of Duke's demos up to a quarter further on in presents than in frames), and ends where the
next demo's load starts or where the game's timedemo line came. Its *average* is the game's own
frames, node's count, over that time; the lows, the high and the worst are the frames the page
showed, each timed from the one before. Those come in sixtieths of a second, as the display draws on
the screen's refresh. The display draws on animation frames, which only a tab in front gets, so the
tab is brought to the front. With `--data`, the files are put in the page's Uploads (`duke3d/`) for
the run, unless they are there already, and taken out after.

**`--lag N`**: the game played in the page (Duke's E1L1, Quake's e1m1), a turn key held for 300 ms
N times, left and right in turn. For each press, the time from the page's `keydown` to the first
frame drawn that differs from the frame before it in a quarter of 2,560 sampled pixels, and how
many frames the page showed between the two.

`--json FILE` keeps every frame's numbers; `--again FILE --like NODE.json` reads a page run again
with a node run's demo boundaries (Duke's three demos).

## 2. The games' frames (2026-11-15)

c4ix_and_toppings at c4b8bee: Duke and Quake built `-O -mfuse`, Quake also `-mfpu`. Node on helios
(load average about 12 then: node's clock is shown, but it is the instructions that repeat). The page
on the Chrome of CDP 9222 (Chrome 151, a Chromebook), served from helios through the https proxy,
C4IX at 400 MHz with one CPU, the game typed at its shell with `-show`, sound off (`/ns /nm`,
`-nosound`), one run each.

### Instructions a frame (node, exact)

| run | frames | min | median | p99 | max | worst frames |
|---|---:|---:|---:|---:|---:|---|
| Duke Atomic demo1 | 3,671 | 1.46 | 6.49 | 12.82 | 15.23 | #2757, #1976, #2258 |
| Duke Atomic demo2 | 2,188 | 1.46 | 4.40 | 7.65 | 9.73 | #4278, #4284, #4269 |
| Duke Atomic demo3 | 1,733 | 1.46 | 4.08 | 6.19 | 7.18 | #6450, #7277, #6531 |
| Duke shareware, test's script | 598 | 3.20 | 4.11 | 5.41 | 5.81 | #314, #39, #97 |
| Quake demo1 | 969 | 4.14 | 7.36 | 17.66 | 19.41 | #177, #178, #530 |
| Quake demo2 | 985 | 3.88 | 7.09 | 16.15 | 20.01 | #794, #795, #792 |
| Quake demo3 | 1,090 | 3.73 | 8.39 | 16.18 | 19.26 | #748, #133, #134 |
| Doom demo1 | 5,064 | 0.84 | 3.94 | 4.82 | 5.90 | #1993, #1994, #4093 |
| Doom demo2 | 3,874 | 0.84 | 3.79 | 4.75 | 5.44 | #397, #424, #762 |
| Doom demo3 | 2,172 | 0.84 | 3.76 | 5.24 | 6.63 | #185, #182, #184 |
| Wolf3D timedemo 1 | 1,215 | 0.42 | 2.08 | 2.52 | 4.39 | #35, #783, #492 |
| ROTT timedemo 4 | 606 | 0.89 | 4.11 | 5.31 | 11.52 | #165, #137, #538 |

M instructions. Each game's level load is left out (section 1): Duke's demos 59.9, 93.5 and 61.7 M,
Quake's 68.8 to 72.1 M, Wolf3D's 147.8 M, ROTT's 151.0 M. The checksums of the runs that make
them (Doom's, Wolf3D's, ROTT's timedemos) are the tests': Doom demo3 `01914fe7`, Wolf3D `d7358845`,
ROTT `3b083b33`.

The spread is the point. Duke's demo1 has a median of 6.5 M and a 99th percentile twice that; Quake's
demos 7 to 8 M and 16 to 18 M. The heaviest frames are not a little heavier: they are two and a half
times the median.

### Frame rates (node, at a host's speed)

| run | at | low | 0.1% low | 1% low | avg | high | worst |
|---|---|---:|---:|---:|---:|---:|---:|
| Duke Atomic demo1 | 100 M/s | 6.6 | 7.2 | 7.6 | 13.1 | 68.7 | 152 ms |
| | 250 M/s | 16.4 | 17.9 | 19.1 | 32.8 | 171.7 | 61 ms |
| Duke Atomic demo2 | 100 M/s | 10.3 | 11.2 | 12.4 | 24.0 | 68.7 | 97 ms |
| Duke Atomic demo3 | 100 M/s | 13.9 | 14.2 | 15.4 | 25.3 | 68.7 | 72 ms |
| Quake demo1 | 100 M/s | 5.2 | 5.2 | 5.4 | 12.1 | 24.1 | 194 ms |
| | 250 M/s | 12.9 | 12.9 | 13.5 | 30.3 | 60.4 | 78 ms |
| Quake demo2 | 100 M/s | 5.0 | 5.0 | 5.4 | 12.5 | 25.7 | 200 ms |
| Quake demo3 | 100 M/s | 5.2 | 5.4 | 5.9 | 11.2 | 26.8 | 193 ms |
| Doom demo3 | 100 M/s | 15.1 | 15.5 | 17.8 | 26.1 | 118.6 | 66 ms |
| Wolf3D | 100 M/s | 22.8 | 28.1 | 36.9 | 49.4 | 239.1 | 44 ms |
| ROTT | 100 M/s | 8.7 | 8.7 | 13.5 | 28.8 | 112.4 | 115 ms |

At 250 M a second every figure is 2.5 times the 100 M one. **Even at 250 M a second, Duke's demo1
holds 19 fps for its slowest 1% of frames and Quake 13.5**: the 15 fps the person sees is not the
interpreter alone, it is these frames.

### Frame rates (the page, 400 MHz)

| run | MMU | low | 0.1% low | 1% low | avg | high | worst |
|---|---|---:|---:|---:|---:|---:|---:|
| Duke Atomic demo1 | on | 7.5 | 8.0 | 8.5 | 15.7 | 62.8 | 133 ms |
| | off | 10.0 | 10.0 | 10.2 | 19.5 | 75.6 | 100 ms |
| Duke Atomic demo2 | on | 12.0 | 12.0 | 13.9 | 27.2 | 62.4 | 84 ms |
| | off | 12.0 | 13.8 | 15.7 | 33.6 | 62.6 | 83 ms |
| Duke Atomic demo3 | on | 15.0 | 15.0 | 16.8 | 29.1 | 61.9 | 67 ms |
| | off | 15.0 | 17.1 | 19.6 | 36.1 | 62.6 | 67 ms |
| Duke shareware script | on | 15.0 | 15.0 | 16.3 | 28.4 | 64.6 | 67 ms |
| | off | 12.1 | 12.1 | 18.0 | 35.7 | 77.3 | 83 ms |
| Quake demo1 | on | 4.0 | 4.0 | 5.7 | 13.3 | 30.2 | 250 ms |
| | off | 6.7 | 6.7 | 7.4 | 16.9 | 60.2 | 150 ms |
| Quake demo2 | on | 5.5 | 5.5 | 6.1 | 14.0 | 54.7 | 183 ms |
| | off | 6.7 | 6.7 | 7.7 | 17.3 | 60.4 | 150 ms |
| Quake demo3 | on | 5.4 | 5.5 | 6.3 | 12.4 | 55.5 | 184 ms |
| | off | 7.5 | 7.5 | 7.9 | 15.0 | 60.4 | 133 ms |
| Doom demo3 | on | 15.0 | 16.2 | 19.3 | 32.0 | 61.5 | 67 ms |
| | off | 15.0 | 16.4 | 25.6 | 41.0 | 77.2 | 67 ms |
| Wolf3D | on | 14.9 | 17.1 | 26.6 | 58.6 | 76.8 | 67 ms |
| | off | 19.9 | 19.9 | 34.2 | 76.0 | 73.9 | 50 ms |
| ROTT | on | 8.6 | 8.6 | 13.8 | 37.2 | 61.1 | 117 ms |
| | off | 10.0 | 10.0 | 15.6 | 45.8 | 61.6 | 100 ms |

The average is the game's frames over the time; the rest are the frames the page showed, so the high
is the screen's refresh and can be below the average when the game makes more frames than are shown.

The MMU costs 20% of the frame rate in every game here (Quake demo1 16.9 to 13.3, Duke's demo1 19.5 to
15.7). The page runs about 1.4 times node's 100 M a second with the MMU off and 1.1 times with it on:
Quake's 16.9 fps with the MMU off is 8.26 M instructions a frame at 140 M a second. So the person's
"15 fps or below in heavy scenes" is this: Duke's demo1 averages 15.7 with the MMU, and its slowest 1%
run at 8.5; Quake averages 12 to 14 and its slowest 1% at 6.

## 3. Where the heavy frames go

`--profile`, the heaviest 1% of frames against the median 10%, M instructions a frame.

**Duke, the Atomic Edition's three demos** (76 heaviest frames, mean 12.91 M; 760 median, 4.73 M).
75 of the 76 are in demo1, frames 1949 to 2782: the player has died there ("PRESS SPACE TO RESTART
LEVEL" over a view tilted on its side and down at the floor, by the game's `-shot` frames).

| function | median | heavy | growth | of the growth |
|---|---:|---:|---:|---:|
| `rhlineasm4` | 0.000 | 4.130 | 4.130 | 50.5% |
| `slopevlin` (sloped floors and ceilings) | 0.568 | 3.093 | 2.525 | 30.9% |
| `mvlineasm4` (masked walls) | 0.308 | 0.785 | 0.477 | 5.8% |
| `vlineasm4` (walls) | 0.678 | 1.097 | 0.419 | 5.1% |
| `tvlineasm2` (translucent walls) | 0.011 | 0.125 | 0.114 | 1.4% |
| `dorotatesprite` | 0.064 | 0.162 | 0.098 | 1.2% |
| everything else | | | 0.4 or less each | |

- **`rhlineasm4`** is the tilted screen. While `rotscrnang` is set -- the player dead, knocked about
  (`forceplayerangle`) or looking left or right -- Duke draws the view into a tile and draws that back
  rotated (`game.c:3315`, `dorotatesprite`), the whole view through `rhlineasm4`. It is the one inner
  loop still as Chocolate Duke3D wrote it: its unsigned carries are compares that c4lc makes by
  flipping sign bits, about 70 instructions a pixel. Now written for c4 as `hlineasm4` was (games/duke3d
  622cfa9, 2026-11-15): the same 770 checksums from the host build, and on Atomic demo1 the mean frame
  went from 7.63 to 6.53 M instructions, the 99th percentile from 12.82 to 10.59 M and the worst from
  15.23 to 13.00 M; the 1% low at 250 M instructions a second from 19.1 to 23.0 fps. Demos 2 and 3,
  which never tilt, did not change.
- **`slopevlin`**: a dead player's view is mostly floor, sloped and close.
- The median frame: `vlineasm4` 14.3%, `slopevlin` 12.0%, `hlineasm4` 9.7%, `mvlineasm4` 6.5%,
  `c4_mulscale` 5.3%, `c4_udiv64` 4.7%, `wallscan` 4.1%: the four drawing loops are 43% of it.
- Game logic does not grow: the actors, `movestandables`, `inside` and the like are smaller in the
  heavy frames than in the median ones.

Demo2's heaviest frames (#4269 to #4284, 8.4 to 9.7 M, against its median of 4.4) are an explosion's
sprites, by its frames: transparent and masked columns. The shareware script's heaviest (5.5 M against
4.1) grow in `mvlineasm4` (43% of their growth), `slopevlin`, `mhlineskipmodify` and `maskwallscan`.

**Quake, demo1** (10 heaviest frames, mean 18.47 M; 98 median, 7.35 M):

| function | median | heavy | growth | of the growth |
|---|---:|---:|---:|---:|
| `R_DrawSurfaceBlock8_mip0` | 0.388 | 3.151 | 2.763 | 24.9% |
| `R_DrawSurfaceBlock8_mip1` | 0.169 | 2.826 | 2.656 | 23.9% |
| `D_SpriteDrawSpans` | 0.000 | 0.957 | 0.957 | 8.6% |
| `R_AddDynamicLights` | 0.002 | 0.663 | 0.662 | 6.0% |
| `R_DrawParticles`, `D_DrawParticle` | 0.032 | 0.893 | 0.860 | 7.7% |
| `D_DrawSpans8` | 2.276 | 2.702 | 0.426 | 3.8% |
| `R_BuildLightMap` | 0.025 | 0.320 | 0.295 | 2.7% |
| float to unsigned, long long shifts (`__c4f_to_ull`, `__c4ll_shr`, ...) | | | 0.8 | 7% |

Demo2's and demo3's are the same: the surface blocks are 60% and 45% of the growth.

- **The surface cache.** Quake lights a wall's texture once and keeps it (`R_DrawSurfaceBlock8_mip*`,
  `D_CacheSurface`). A dynamic light -- a rocket's explosion, a muzzle flash, a moving light -- makes
  every surface it touches lit again in that frame. The heavy frames are explosions: lights,
  particles (`D_DrawParticle`, 1,551 calls a frame against 69) and the explosion's sprite
  (`D_SpriteDrawSpans`).
- **Float to unsigned and back.** `R_AddDynamicLights` adds a float to an `unsigned` for each texel a
  light reaches (`blocklights[t*smax + s] += (rad - dist)*256`, `r_surf.c:119`): the unsigned goes to
  a float and the sum back through the library (`__c4f_from_ull_s`, `__c4f_to_u`, `__c4f_to_ull`,
  `__c4fp_clz32` and long long shifts), about 2,400 times a heavy frame, 0.8 M, 7% of the growth.
  `-mfpu` has float to and from `int`, not `unsigned`.
- The median frame: `D_DrawSpans8` 30.9%, `D_DrawZSpans` 11.1%, the surface blocks 7.6%,
  `D_DrawTurbulent8Span` 4.5%, `D_PolysetDrawSpans8` 4.2%; the edges and surfaces
  (`R_RecursiveWorldNode`, `R_LeadingEdge`, `R_ClipEdge`, `R_RenderFace`, `R_TrailingEdge`,
  `R_EmitEdge`, `R_GenerateSpans`) 19%.

**Doom, Wolf3D and ROTT**, the same way (their median frames carry the timedemo's checksum, section 1):
- Doom demo3: a median frame is `R_DrawColumn` 33% and `R_DrawSpan` 20%; the heaviest 1% (5.6 M
  against 3.8) grow in `R_DrawSpan`, floors, 49% of the growth, and `R_DrawFuzzColumn`, the
  spectres' shimmer, 15%.
- Wolf3D: `ScalePost` 39% and `ScaleLine` 20% of a median frame, and 58% and 23% of the heaviest
  frames' growth (2.7 M against 2.1): walls close up.
- ROTT: `DrawRow` 22% and `R_DrawWallColumn` 17% of a median frame; its heaviest 1% (7.4 M against
  4.1) are its own turned screen, `DrawRotRow` and `StartupRotateBuffer`, 3.4 and 0.6 M.

**So the low frames are drawing, and drawing of a kind the median frame hardly does.** In both games
it is pixels through a loop the median frame runs little or not at all: Duke's tilted screen and close
floors, Quake's relit surfaces and explosions. The game logic is flat.

## 4. Hand-written code against a drawing opcode, on one loop

Duke's `vlineasm4`, a wall's column, four columns a call. A pixel is
`*p = pal[tex[v >> sh]]; v += inc; p += bpl;`, and c4lc (`-O -mfuse`) makes it

    PSHL p; PSHL pal; PSHL tex; PSHL v; LDL sh; LSR; ADD; LCU; ADD; LCU; SC     the pixel, 11
    PSHL v; LDL inc; ADD; STL v                                                 v += inc, 4
    PSHL p; LDL bpl; ADD; STL p                                                 p += bpl, 4

19, and the loop's count, compare, branch and jump, 9 a turn of four rows: 21.25 a pixel. Each of
the 19 is needed by a stack machine whose every push sets `a`: the three bases go on the stack before
the index is made, each load pairs an `ADD` with its `LCU`, and there is nothing that adds one local
to another in place.

### A: by hand, in the machine's own instructions

The best found: eight rows a turn, each row's offsets from the turn's (`p[b3] = pal[tex[(v + i3) >> sh]]`,
with `b3 = 3*bpl` and `i3 = 3*inc` worked out once a column), so that `v` and `p` move once a turn, and
the last rows through a jump into the run (a `switch` that falls through, which c4lc makes a jump
table). A row is then 17 (`PSHL p; LDL b3; ADD; PSH` for its address and `PSHL v; LDL i3; ADD; PSH` for
its index replace the 8 of the two steps), the turn's first row 11, the steps 8 and the loop 9 a turn
of eight: **18.4 a pixel, −13%**. More rows a turn approach 17; no ordering of these instructions
does better. It is written in C, and c4lc's code for it is the code a person would write by hand, so
`__c4_opcode` or a hand-made object gains nothing over it:

    for (n = count; n >= 8; n -= 8)
    {
        p[0] = pal[tex[v >> sh]];
        p[b1] = pal[tex[(v + i1) >> sh]];      /* b1..b7 = bpl*k, i1..i7 = inc*k */
        ...
        p[b7] = pal[tex[(v + i7) >> sh]];
        v += i8;
        p += b8;
    }
    switch (n) { case 7: p[b6] = ...; ... case 1: p[0] = ...; }

It was measured, not committed: section 6 says why.

### B: the loop as one host instruction

`TCOL` (95), a prototype on branch `tcol-proto`: `__c4_opcode(count, v, inc, sh, tex, pal, dest,
stride, 95)` draws the column, and `a` is `v` after. `TCOLM` (96) leaves a pixel whose texel is 255
alone (`mvlineasm4`'s masked walls). The machine runs it in JavaScript (`c4m.js` `texcol`): plain
memory straight from the arena's bytes, a task under the MMU through its pages (`mmu.page`), and any
address that is neither through `ld8`/`st8`, so a fault there faults as a load would and the
instruction runs again. Only a machine made with `tcol` (`cli.js --tcol`, the page's `&tcol`,
`createMachine({ tcol })`) has it, and says so with an `INFO` bit, `0x100000`, which C4IX passes to
its tasks as it is (`task_info`). Duke asks once (`draw.c`, only c4lc's build, `__c4_inttypes__`) and
draws with the C loops on any machine that does not say so. `vlineasm1`, `vlineasm4`, `mvlineasm1`
and `mvlineasm4` use it.

### The numbers

Per pixel, `libjs/tests/src/tcolbench.c` (on `tcol-proto`): 320 columns of 200 rows of a 64-texel
texture, 200 frames, each way in turn, the same frame buffer at the end (`e49a35fc`) every way.

| | instructions a pixel | node, ns a pixel | page (bare machine), ns a pixel |
|---|---:|---:|---:|
| c4lc's loop, as Duke has it | 21.4 | 164 | 132 |
| A, by hand | 18.9 | 137 | 118 |
| B, `TCOL` | 0.1 (a call a column) | 10 | 6 |

Per frame, by instructions, the Atomic Edition's demos. Every frame is the same: Atomic `25b25fc6`
at frame 1,000 and the shareware script `96ab05bb`, A and B against the build without them. (Node's
clock, under helios's load, differed by 15% between two runs of one build; the page below is the
wall's measure.)

| | demo1 M a frame | demo2 | demo3 |
|---|---:|---:|---:|
| as it is | 7.630 | 4.166 | 3.951 |
| A | 7.518 (−1.5%) | 4.073 (−2.2%) | 3.866 (−2.2%) |
| B, the machine without `tcol` (C loops, one `INFO`) | 7.633 | 4.168 | 3.954 |
| B | 6.268 (−17.9%) | 2.989 (−28.3%) | 2.837 (−28.2%) |

And in the page, C4IX at 400 MHz, `&tcol` against the same tree without it, served together
(shareware script: two runs each, both shown):

| | MMU | avg fps | 1% low | worst |
|---|---|---:|---:|---:|
| shareware script | on | 27.4, 28.1 → **43.4, 41.6** | 17.1, 17.7 → 20.0, 20.0 | 67 → 50 ms |
| | off | 34.3, 34.6 → **54.4, 53.9** | 18.9, 18.9 → 27.2, 25.4 | 67 → 50 ms |
| Atomic demo1 | on | 15.7 → **18.4** | 8.5 → 9.8 | 133 → 117 ms |
| Atomic demo2 | on | 27.2 → **35.1** | 14.0 → 15.0 | 83 → 67 ms |
| Atomic demo3 | on | 29.1 → **37.5** | 17.1 → 19.1 | 67 → 67 ms |
| Atomic demo1 | off | 19.7 → **22.8** | 10.8 → 11.8 | 100 → 100 ms |
| Atomic demo2 | off | 34.1 → **43.9** | 16.9 → 18.9 | 67 → 67 ms |
| Atomic demo3 | off | 36.4 → **46.6** | 19.9 → 20.0 | 50 → 50 ms |

The page's frame times come in sixtieths of a second (its display draws on the screen's refresh), so
its lows and worst frames move in steps of 16.7 ms.

**A against B.** By hand, the loop is 13% shorter and the frame 1.5 to 2% shorter: c4lc already makes
the obvious code, and what is left is the stack machine's own cost. As one instruction the loop is 16
to 22 times faster a pixel, and the frame 18 to 28% shorter in instructions and up to 57% faster in
the page (the shareware level is mostly walls). Demo1 gains least because its heavy frames are floors and the
tilted screen (section 3), which `TCOL` does not draw.

**What c4lc could do.** A's one idea, offsets from the turn's `v` and `p` in an unrolled loop, is a
compiler's induction-variable splitting; done by c4lc it would give every such loop its 10 to 13%,
which in these games is 1 to 2% of a frame. A loop tested at its bottom saves its `JMP`, one
instruction a turn, everywhere. An instruction that adds a local to `a` (`PSH; LDL x; ADD` in one, the
immediate-ALU family `docs/fused-opcodes.md` leaves out) would take a row of A from 17 to 15. None of
these comes near B.

### What `TCOL` would cost to keep

As a prototype it is in libjs alone: 58 lines in `c4m.js`, a line each in `boot.js`, `worker.js`,
`page.js`, `web/app.js` and `cli.js`, and 30 in Duke's `draw.c`. No compiler changes: `__c4_opcode`
reaches it. No other machine has to change, because the program asks first and falls back to its C
loop, so `c4m`, `c4mpg`, `c4mp`, `oisc4` and the board run Duke as they do now. That is the difference
from the 16-bit opcodes and the fused ones (`docs/c4-16bit.md`, `docs/fused-opcodes.md`), which every
machine and both compilers had to learn because the compilers emit them.

To keep it:
- its number. 95 and 96 are the next free in the append-only table; an accelerator a program asks for
  should have a range of its own (or be a device's command), so that the next true opcode does not
  collide. The name tables (`c4m.c`, `c4l.c`, `c4cc.c` and its token count, `c4r.lisp`, `emit.f`,
  `devices.js`, `explore.c`, `opdoc.js`, `opscan.mjs`) need it only if it joins the instruction set;
- `mmu.js`'s own fast path, if the MMU's share matters (now each byte goes through `mmu.page`);
- `par.js`: a CPU on a thread of its own makes its machine in `ap.js`, which does not pass `tcol` on
  yet (a game draws on CPU 0, so it is not reached);
- a test: `tcolbench`'s sum with and without, and `test-duke3d` once more with `--tcol`;
- `c4m.c`, if native c4m should have it too: the same loop in C, about 25 lines. The board and oisc4
  need never have it.

The same shape covers the rest of the drawing loops (section 3): Duke's `hlineasm4`, `slopevlin` and
`rhlineasm4`, and Quake's `D_DrawSpans8` runs, `D_DrawZSpans` and the surface blocks' rows, each a few
dozen lines of JavaScript and a call in the game. Together they are 43% of Duke's median frame and
most of its heavy ones, and half of Quake's.

## 5. Input lag

`--lag 12`: Duke's E1L1 (shareware, played, sound on) and Quake's e1m1 in the page, C4IX with the
MMU and one CPU, a turn key held 12 times. The machine's clock (`mhz`) was turned down to make the
frame rate lower: at 400 and 200 MHz the Chromebook, not the clock, sets the rate.

| game | mhz | frame | key to the frame that shows it, median (min-max) | in frame times | frames shown |
|---|---:|---:|---|---:|---|
| Duke | 400 | 36 ms | 90 ms (49-127) | 2.5 | 2 to 4, mostly 2 or 3 |
| | 200 | 36 ms | 58 ms (46-86) | 1.6 | 2 to 4, mostly 2 |
| | 100 | 43 ms | 69 ms (43-110) | 1.6 | 2 or 3 |
| | 50 | 95 ms | 167 ms (118-219)* | 1.8 | 2 or 3 |
| Quake | 400 | 60 ms | 110 ms (82-135) | 1.8 | 2 or 3 |
| | 200 | 60 ms | 91 ms (74-125) | 1.5 | 2, every time |
| | 100 | 67 ms | 122 ms (71-145) | 1.8 | 2 or 3 |
| | 50 | 146 ms | 230 ms (176-279) | 1.6 | 2, every time |
| | 25 | 296 ms | 467 ms (333-664) | 1.6 | 2, every time |
| Duke, 4 CPUs on threads | 400 | 37 ms | 70 ms (67-127) | 1.9 | 2 or 3 |
| Quake, 4 CPUs on threads | 400 | 69 ms | 125 ms (92-156) | 1.8 | 2 or 3 |

\* The first four presses at 50 MHz came while the level was still loading and are left out. Duke
at 25 MHz did not finish loading in the time the run gave it.

**The lag is the frame rate.** A game that reads its input when a frame starts shows a key in the
frame after the one being made when it was pressed: half a frame's wait on average, then a whole
frame, then the page's next animation frame (8 ms on average) and the compositor's. That is 1.5
frame times and about 20 ms, and it is what Quake shows at every rate, from 60 ms frames to 296 ms
ones: 110 against 110 predicted, 230 against 239, 467 against 464. "Frames shown" counts the frames
the page drew after the key, up to the one that showed it: 2 means nothing waits in between -- not
the worker's queue, not the display's ring. With four CPUs on threads, where every present goes
through CPU 0 (the game is pinned there for drawing), the lag is the same 1.8 to 1.9 frames. Duke's
3s and 4s are its own: it moves the player by 30 Hz tics, and a frame that falls between two tics shows no turn yet. There is no
buffering or presentation delay to remove; at 15 fps a key takes about 120 ms to show because a frame
takes 67.

## 6. What to do

1. **Drawing opcodes, behind a probe, in libjs.** The one change that moves the frame rate a person
   sees: `TCOL` alone took the shareware level from 28 to 43 fps with the MMU and the Atomic demos'
   averages up 17 to 29%, with the same frames. The rest of the drawing loops in the same shape --
   Duke's floors (`hlineasm4`), slopes (`slopevlin`) and tilted screen (`rhlineasm4`), Quake's spans
   (`D_DrawSpans8`'s runs), z spans and surface blocks -- are where both games' heavy frames go
   (section 3). Give such accelerators a number range of their own, outside the instruction set, so
   no other machine or compiler has to learn them.
2. **`rhlineasm4` written for c4: done** (games/duke3d 622cfa9). The one inner loop left as
   Chocolate Duke3D had it, the whole tilted screen when the player dies, is knocked about or looks
   aside. Atomic demo1's 1% low went up by a fifth (19.1 to 23.0 fps at 250 M instructions a second),
   on every machine (section 3).
3. **Quake's dynamic lights.** `blocklights[...] += (rad - dist)*256` makes an unsigned a float and back
   through the library for every lit texel, 7% of the heavy frames' growth; float to and from
   `unsigned` as `-mfpu` operations (or an `int` there, if its frames stay gcc's) would take it.
4. **The MMU off for games**, or cheaper: it costs the page about 20% of its frame rate in every game
   here (`docs/c4ix-mmu.md` 12).
5. **Not**: hand-written loops (13% of a loop, 2% of a frame), or more compiler work on these loops
   for their own sake. A loop tested at its bottom and induction variables split in unrolled loops are
   worth having in c4lc for every program, but they will not change what a player sees.
6. **Lag** needs nothing of its own: it is 1.5 to 2 frames, as in any game that reads its input once a
   frame. It falls as the frame rate rises.
