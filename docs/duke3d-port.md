# Duke Nukem 3D on c4: the plan

Asked for on 2026-11-10: Duke Nukem 3D, like Doom, Wolfenstein 3D, Rise of the
Triad and Quake under `games/`. It must run in C4DOS, on the bare machine and in
C4IX; boot the shareware episode from the disk where its licence allows that; boot
the full game from the person's own files (the page's Uploads, which C4IX lists in
`/home`); take the mouse; and let the settings, the screen size among them, be
changed from inside the game.

This is the plan, written before any porting. What happened is added at the end.

## 1. The source base: Chocolate Duke3D

| candidate | C, as c4lc sees it | renderer | settings in the game | size | verdict |
|---|---|---|---|---|---|
| 3D Realms' release (2003) | Watcom C with `#pragma aux` assembly for every fixed-point helper, and Ken's `a.asm` for the inner loops | Build, DOS VESA/VGA | no: `SETUP.EXE`, which was not released | game 1.3 MB of C, the engine separately | all that assembly would have to be written again in C, which Chocolate Duke3D has done |
| **Chocolate Duke3D** (Fabien Sanglard, 2012-2019) | plain C: the assembly is C (`fixedPoint_math.h`, `draw.c`), `stdint` types throughout, one SDL file (`display.c`) | Build, 8-bit, any `xdim` x `ydim` up to 1600x1200 | **yes**: a VIDEO SETUP menu (resolution, detail, frame rate) built into the options menu (`menues.c` case 706), and `duke3d.cfg` keeps `ScreenWidth`/`ScreenHeight` | 1.5 MB repository, no submodules | **chosen** |
| JFDuke3D (Jonathon Fowler) | C, but in three submodules (jfbuild, jfaudiolib, jfmact) and with Polymost's OpenGL renderer alongside | Build classic and Polymost | a startup window (GTK or Win32), plus some in-game | 2.9 MB and the submodules | more to cut away than to gain |
| EDuke32, NBlood/Rednukem | C++ now | | | very large | no C++ compiler here |

Why Chocolate Duke3D:

- **It is the original game, made portable and nothing more** (its README:
  "as faithful as possible to the original source"). The game logic is 3D Realms'
  Atomic Edition code, so it plays like the DOS game, and it recognises every
  official GRP by its CRC: the shareware 1.3 (`0x983AD923`), the full 1.3D, 1.4
  and Atomic 1.5 (`duke3d.h`). One build plays the shareware and the person's own
  full game: Wolfenstein needed two builds (`wolf3d`, `wolf3d6`) and this does not.
- **No assembly is left.** `pragmas.h`'s Watcom functions are C in
  `fixedPoint_math.h`, using `int64_t` for the 32x32 products, which c4lc has
  (`long long`). The texture loops (`hlineasm4`, `vlineasm4`, `mvlineasm4`,
  `tvlineasm1`...) are C in `draw.c`.
- **Settings in the game already exist**, which is what modern ports do and what
  the user asked for: the resolution list is the engine's `validmode*` table,
  which `display.c` fills from SDL; here it is filled from a fixed list of sizes
  the display can show (§4).
- **It uses Apogee's audiolib for music**, the same units (`midi.c`, `al_midi.c`,
  `gmtimbre.c`, `ll_man.c`) the ROTT port already drives on the card's OPL
  (`games/rott`, `rott-snd.c`). Duke's songs are MIDI in the GRP, as ROTT's are.
- **What the renderer allocates is fixed and small.** Map arrays are static
  (`MAXSECTORS` 1024, `MAXWALLS` 8192, `MAXSPRITES` 4096); the per-column arrays
  are sized for `MAXXDIM` 1600. The art is in one cache (`cache.c`, Ken's
  `cache1d`) of a size the game chooses at start (§5); the frame is one 8-bit
  buffer of `xdim * ydim`.

**Licences.** The game code is GPL-2 (3D Realms, 2003). The engine (`Engine/src`)
is Ken Silverman's Build, under his own licence (`BUILDLIC.TXT`): free for
non-commercial use, with credit. Every Build port lives with this pairing. c4 is
not sold, so it may carry it; the game repository adds `BUILDLIC.TXT` (from
jfbuild, which has Ken's text) and `GPL.TXT`, which Chocolate's repository lacks.

## 2. The data

The shareware package is `3dduke13.zip`, Duke Nukem 3D v1.3D shareware,
5,924,374 bytes, as 3D Realms' FTP had it (the Wayback Machine keeps
`ftp.3drealms.com/share/3dduke13.zip`). Its own `LICENSE.DOC` decides whether
c4 may put it on its disks, as `VENDOR.DOC` did for ROTT: that is read before
anything ships (§8). It is never committed. `make duke3d-data` fetches it with
`games/fetch.sh`, checked by its SHA-256, or takes it from `$DUKE3D_ZIP`.

The full game is the person's own `DUKE3D.GRP` (1.3D, 1.4 or Atomic 1.5),
uploaded on the page:

- C4IX: the uploads are listed in `/home`. `duke3d -home` (and its Go > Games
  entry, "Duke Nukem 3D (your files)") looks for `DUKE3D.GRP` there.
- the bare machine and C4DOS: the uploads are on the host's drive 0, where
  Wolfenstein's `wolf3d6` finds its `*.WL6`.

Without the file it says so and stops, as `wolf3d6` does.

## 3. The shape of the port

As Quake's (`games/quake`), whose layout this follows:

- game repository `andrakis/c4-Duke3D`, on Chocolate Duke3D's history, with c4's
  work as commits on a `c4` branch; submodule `games/duke3d` in c4.
- `c4.mk`: `duke3d-objs`, `duke3d-data`, `duke3d-host` (gcc, 32-bit) and
  `test-duke3d`; `GAME_OBJS`/`GAME_TESTS`.
- `c4-images.sh`: link for C4IX against libc, and whole with `libc-bare` for the
  bare machine and C4DOS.
- `c4ix.vfs.txt`: `/usr/games/duke3d`, with `#@app Games|Duke Nukem 3D|duke3d`.
- c4's own files in `c4/`:
  - `duke-gui.c`: the display. Takes the whole display (`udisplay_take` on C4IX),
    shows the 8-bit frame with `gui_blit8`, scaled to the display, with the
    palette; keys to Duke's scancodes; the mouse locked (`GUI_PTR_LOCK`,
    `GUI_EV_REL`) to turn and aim, its buttons to fire and the rest.
  - `duke-c4ix.c`, `duke-bare.c`: whether the display has to be asked for.
  - `duke-host.c`: no display, no sound, for gcc: the reference.
  - `duke-snd.c`: `FX_` on the card's eight sample channels (the GRP's VOCs and
    WAVs, 8-bit), `MUSIC_` through audiolib's MIDI player on the OPL, as
    `rott-snd.c` does.
  - `duke-sum.c`: the clock (virtual in a test), and every frame checksummed.
  - `c4inc/`: the headers C4IX's libc lacks, as Quake's.

`display.c` (SDL) is replaced by the c4 files; `mmulti.c`, `network.c` and enet
are replaced by a one-player stub; `dsl.c`, `sdl_midi.c` are not built.

## 4. The screen size as a setting

The engine draws at any `xdim` x `ydim`, and Duke's HUD, menus and status bar
are placed in 320x200 units and scaled (`rotatesprite`), so a bigger screen is
sharper, not smaller. The display's `gui_blit8` scales the frame to the canvas,
so every size fills the screen.

- The list (`validmodexdim`/`validmodeydim`): 320x200, 320x240, 400x300,
  512x384, 640x400, 640x480, 800x600.
- **Default 320x200**, as the DOS game ran.
- Changed in the game: Options > Video Setup > Resolution, Enter to apply, as
  Chocolate has it. Kept in `duke3d.cfg` beside the game (or in `/home` for the
  full game), with the other settings: keys, mouse, sound, detail.
- On the command line too: `duke3d -res 640x480` for that run.
- The full-screen toggle (Alt-Enter) does nothing here: it is always the whole
  display.

## 5. Memory and speed

- **Memory.** The shareware GRP is 11 MB and is read through the cache, not
  loaded whole. The cache is sized at start; Duke on DOS ran in 8 MB, so a cache
  of 8 MB in a 32 MB machine is the first try, and the game runs in `-m 32` or
  `-m 64` as Doom does.
- **Speed.** Duke is heavier than Doom (room-over-room portals, sloped floors,
  more sprites, mirrors) and lighter than Quake (no floats, no surface cache).
  Quake costs 10.5 M instructions a frame and runs at 10-13 fps in the page;
  Duke at 320x200 should come in under that. The likely costs, to be measured:
  - `int64_t` in `mulscale`/`divscale`, which c4lc turns into helper calls; the
    renderer calls them per column and per span. Where both operands are known to
    be small they can be 32-bit, or a scaled 32-bit sequence; measured first.
  - the texture loops in `draw.c`, as `D_DrawSpans8` was Quake's biggest.
- **What is reported**: the frame rate at 320x200 under C4IX in the page (CDP
  9222), with and without the MMU, and VM instructions a frame from the test.

## 6. The test

`test-duke3d` holds the c4lc build to the gcc build, frame by frame, on the bare
machine, under C4DOS and inside C4IX, as `test-quake` does. The game's own demos
are recorded at a version byte the Atomic code may refuse for the shareware's
demos; if `DEMO1.DMO` plays, the test plays it as a timedemo (a tic a frame on a
virtual clock, every frame checksummed with its palette). If it does not, the
test records nothing and plays a fixed, scripted input instead (`duke-sum.c`),
which needs no data beyond the GRP.

## 7. Steps

1. Fetch Chocolate Duke3D and the shareware package (with the user's OK); make
   the game repository and its `c4` branch.
2. Build it with gcc, 32-bit, with a null display: `duke3d-host` reaches the
   title loop and plays a demo. This is the reference.
3. Compile every unit with c4lc; fix what it refuses, preferring changes to the
   game's source in the game repository, and to c4lc only when a construct is
   common.
4. The display, keys and mouse; then C4IX, the bare machine and C4DOS.
5. Sound: effects, then music.
6. The settings: the mode list, `-res`, `duke3d.cfg`.
7. `test-duke3d`, the `#@app` lines, the full game from `/home`.
8. Measure; speed work only where the measurement points.

## 8. Open questions, answered

- **The shareware's licence** (`LICENSE.TXT` in the package, V.7.16.98) lets
  individuals give copies away without charge and free web and FTP sites offer
  it for download, as long as nothing is added or removed and the files are not
  modified. c4's page is free, so its disks carry the episode, as they carry
  ROTT's and Wolfenstein's (`games/duke3d/data/README.md`).
- **The shareware's demos do not play**: their version byte is 0 (1.3D's
  format), and the Atomic code refuses them. `test-duke3d` plays a fixed script
  of keys and mouse moves instead (`duke-sum.c`).
- **Build's licence** asks that `BUILDLIC.TXT` go wherever the engine goes, so
  the disks carry it beside the game.

## 9. What happened (2026-11-10/11)

The port went as planned; `games/duke3d/README.c4.md` describes it as it is.
The shareware GRP is `DUKE3DSW.GRP` on the disks, so that an uploaded
`DUKE3D.GRP` never hides it. What the plan did not foresee:

**c4lc** took Chocolate Duke3D with little: C4IX's libc has no `BYTE_ORDER` and
no group or other file modes (now in `unix_compat.h` under `C4DUKE`); no
`alloca` (`rts.c` mallocs); and one declarator, a function returning a function
pointer, which c4lc refuses (a typedef in `duke-display.c`). It read
`char confilename[128] = {"GAME.CON"};`, a string in braces, as an empty name.
`dpmi.h`'s register struct has fields named `CS`, `SP` and `SS`, which `duke3d.h`
makes macros, so `duke-snd.c` declares the four DPMI functions it stubs itself.
And Chocolate's `FixFilePath` wrote a `/` over a name's terminator when it could
not open a directory, which on the bare machine is always.

**The image was 7.8 MB**, and C4IX's loader took 4. Two things:

- **Uninitialized data was stored as zeros.** c4rlink placed each object's data
  and then its BSS, the next object after that, and only trims the image's
  trailing zeros, so every object's BSS but the last was stored: 3.3 MB of
  Duke's image, and some of every program's. Now c4lc and c4fc record how much
  of an object's data is BSS (header bytes 9-12 at 32 bits, zero filler until
  now; 0 means unknown), with a word between the initialized data and the BSS
  so that an address just past the last initialized object is never the BSS's
  first; and c4rlink lays every object's data out first, then the libraries'
  names (`add_needed`), then all the BSS (`load-c4r.c` C4R_HDR_BSS, c4rlink's
  `data_at`). Duke's images went from 5.7 and 6.6 MB to 4.7 and 4.5; Quake's
  from 2.24 to 1.56 MB; C4IX's kernel from 630 to 559 KB; the page's ladder
  disk from 216 to 202 MB.
- **Code and relocations are still 4.2 MB** (2.3 MB of code, 154,000 patches of
  12 bytes), so C4IX's loader cap is 16 MB now, and C4DOS's RUN, which stopped
  reading at its 4 MB scratch and ran the truncated program, grows the scratch
  to fit (`c4r_more`).

**Speed.** The first build cost 14.2 M VM instructions a frame at 320x200 (the
test's script, on E1L1). By a profile sampled by function (`prof.mjs`, with the
helpers charged to their callers):

| what | share | done |
|---|---|---|
| the wall and masked-wall column loops (`vlineasm4`, `mvlineasm4`) | 42% | rewritten under `C4DUKE`: locals, a column at a time, and an arithmetic shift and a mask for each unsigned `>>`, which c4lc makes a call (`<lsr>`) |
| `<lsr>` | 13% | (the same) |
| 64-bit division, `__c4ll_udivmod` | 13%, then 20% | `divscale`, `scale` and `mulscale` work in 32 bits where the value fits, and otherwise by long division a remainder's room at a time and a product of 16-bit halves (`fixedPoint_math.c`), exactly as the `int64_t` code |
| `pixelsAllowed`, a debugging count in every inner loop | | gone under `C4DUKE` (it never runs out) |

Every change gives the same frames: gcc's build, which compiles the same C, kept
its checksums at 320x200, 640x480 and 800x600. **7.15 M instructions a frame**
after, 6.8 M without the test's own checksum; Quake is 10.5 M. What is left is
the column loops themselves (40%) and `<ld16s>`, c4's 16-bit load (11%).

**Two things a player would have met**, found by playing it in the page:

- Choosing a skill waited for the skill's sound to end, in a loop the card's
  interrupt ended on DOS. Here the sound ends when the card is polled, so the
  game hung there with a sound card. The wait polls now, and so does every
  timer sample (`getpackets`), which the game's other such waits call.
- A key tapped faster than a frame was lost, its press and release both read
  between two looks; a key let go in the frame it went down in is let go only
  after the next frame now. And with no `duke3d.cfg` the mouse's buttons did
  nothing (Chocolate gave the keys defaults and the mouse none): they are DOS's
  setup program's defaults now, with the wheel for the weapons.

**Three more, played with the Atomic Edition's files** (2026-11-12, the
person's own GRP on C4IX):

- Quitting (F10, or Quit in the menu, then Y) left a black screen and the game
  running at full speed. On its way out `gameexit` showed the TEN advert (the
  shareware's ordering screens), faded in by the palette alone, and waited for
  a key in a loop that never slept; `duke-display.c` showed a palette only with
  a frame, so the screen stayed black. The game goes straight back to the
  desktop or the prompt now, and a palette is shown as soon as it is set, as
  SDL's 8-bit screen showed it (the end of an episode fades in so too). Checked
  in node: C4IX with the GRP in `/home`, `duke3d -home` on the whole display,
  F10 and Y. Before, the game kept the display and drew nothing more; after,
  it lets the display go, the prompt comes back, and `ps` shows no game.
- Low detail (Video Setup's Detail) scrambled the view, which it draws into a
  tile: Chocolate's `setviewtotile` stepped `ylookup` by the tile's width,
  where a row of the view is a column of the tile, its height long (Ken's
  stepped by `ysiz`).
- `duke3d-full`, the bare machine, could not open a 44 MB GRP in its 128 MB:
  the bare libc reads a file whole, into a buffer that doubles as it fills, so
  32 MB and 64 MB at once. It has 192 MB now (160 MB was enough at 320x200).

**A profile of a demo.** The Atomic Edition's `demo1.dmo` on the bare machine,
frames 400 to 1000, every call counted by its target and by its call site (a
copy of `c4m.js` that counts at `JSR`, `JSRI` and `JSRS`): 8.36 M instructions
and 127,000 calls a frame. By calls, `<ld16s>` 65,700 (c4lc's 16-bit load, a
call to 19 instructions), `<lsr>` 17,200 (18,400 of them `slopevlin`'s),
`<st16>` 12,700, `__c4ll_mul` 3,000, `mulscale` 2,900, `__c4ll_and` 2,600,
`c4_umul64` 2,500, `c4_mulscale` 2,200, `mulscale16` 1,500 and `klabs` 900. By
instructions, `<ld16s>` 11.8%, `slopevlin` 11.2%, `hlineasm4` 11.0%,
`vlineasm4` 10.3%, `mvlineasm4` 6.7%, `__c4ll_mul` 4.1%, `<lsr>` 3.3%,
`c4_udiv64` 3.2% and `c4_umul64` 3.1%. Done:

- `slopevlin`, the sloped floors and ceilings, written again as the other
  inner loops were: no `<lsr>`, no 64-bit product, no unsigned division, and
  its two register tricks kept (the x step's low byte is the count of pixels
  left, the y step's the last pixel drawn), so the same pixels.
- The `mulscaleN`, `dmulscaleN`, `tmulscaleN` and `divscaleN` wrappers are
  macros: c4lc calls a `static inline` function as any other, so each was two
  calls. Each uses each argument once, as the function it names does.
- `CheckAnimStarted` makes its test where it is needed. It ran 322 times in
  the run, five a frame of the opening `.ANM`, and never in the demo.

After: **7.12 M instructions a frame on the demo (-14.8%), 95,500 calls**;
6.98 M on the test's script, from 7.05 M (few slopes on E1L1). Every frame the
same, by the checksums of both runs.

Not done, with the reason:

- `<ld16s>` is the biggest single cost now, 13% and 58,700 calls a frame:
  `short` fields and arrays, `drawalls`, the scans, the actors. The VM has no
  16-bit load, so it is c4lc's to open-code (or the VM's to add), and then
  every program gains. (Done: section 10.)
- `klabs` as a macro: Duke passes it expressions (`klabs(ps[j].oposx-s->x)`),
  which a macro works out twice. c4lc's `-minline` inlines the function
  safely, but only where an argument of a parameter used twice is a variable
  or a constant, and Duke has no such call: built with `-minline`, the image
  is the one built without it, byte for byte. A `-minline` that bound such an
  argument to a temporary first would take them. They are 900 calls a frame,
  about 0.2%.
- `mulscale`'s 32-bit path takes operands within 46,340, and 70% of the
  demo's calls are outside it (`c4_mulscale`, `c4_umul64`: about 6%).

**In the page**, C4IX, 320x200, the test's 600 frames shown (`-show`, sound and
music on), on the Chrome of CDP 9222 (2026-11-11): 12.9 fps as the page starts
(paced at 100 MHz, with the MMU); at `mhz=400&cpus=1`, 16.2 fps with the MMU and
20.3 without. At 640x480, as the page starts, 4.1 fps. (Quake, on another
machine's Chrome: 10.4 and 13.1.)

**Tests.** `test-duke3d` plays E1L1 from the script (`-script`, 600 frames)
under gcc and c4lc on the bare machine, under C4DOS and inside C4IX, and then
changes the screen size in the game's own menu (`-menu`: Options, Video Setup,
320x200 to 400x300) inside C4IX, sets the detail low there, and quits from the
game (F10, Y), which must end it; all match. `-home` was played with an upload
laid over C4IX's disk as the page lays it (the GRP in `/home`) and on the bare
machine; sound and music were recorded from the card (`cli.js --wav`).

## 10. Performance, round 2 (2026-11-13)

Measured as section 9's profile was: the Atomic Edition's `demo1.dmo` on
node's bare machine, frames 400 to 1000, every instruction counted and every
call counted by its target. Each step ends on the same checksum,
`25b25fc6`, and `test-duke3d` holds every build to gcc's frames.

| step | M instructions a frame | change | calls a frame |
|---|---:|---:|---:|
| before (section 9) | 7.119 | | 95,555 |
| 16-bit loads and stores, LSR, as opcodes (docs/c4-16bit.md) | 5.861 | −17.7% | 23,365 |
| `LCU`, an unsigned byte's load (docs/c4-16bit.md) | 5.595 | −4.5% | 23,367 |

**16-bit loads and stores.** `<ld16s>`, `<ld16u>` and `<st16>` were 72,200
calls a frame between them, each a call and 19 to 22 instructions. They are
the opcodes `LH`, `LHU` and `SH` now, one instruction each, run by every
machine c4 has; and `<lsr>` is `LSR`. docs/c4-16bit.md has the reasons for
opcodes rather than open-coded loads (that would have saved about 7%, not
17.7%) and what each machine does. The image is 11% smaller: 4,680,784
bytes to 4,157,388.

