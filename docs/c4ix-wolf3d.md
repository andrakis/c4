# Wolfenstein 3D on C4IX

id Software's Wolfenstein 3D runs on C4IX's whole display. The desktop
gives way to it until it ends, as Windows did for a DOS game. It is
Wolf4SDL's C, compiled by c4lc against C4IX's libc. Go > Wolfenstein 3D
starts it, as does `wolf3d` at a shell prompt; `wolf3d --window` plays
in the Terminal window instead. All of it works: the
display, the keyboard, the music and sounds, and the game. The source,
and what was changed in it, is [games/wolf3d](../games/wolf3d/README.md).

It also runs without the desktop: from C4IX's console, under C4DOS, and
on the bare machine ("Without the desktop", below).

## Playing

The keys are the original's:

| key | does |
|---|---|
| arrows | move and turn |
| Ctrl | fire |
| Alt | strafe, with the arrows |
| Shift | run |
| Space | open a door, push a wall |
| 1-4 | knife, pistol, machine gun, chain gun |
| Esc | the menu |

The game is played from the keyboard. The original's mouse control is
not wired up yet (`plat.h` passes key events only).

It takes the whole display from the desktop (`include/display.h`,
docs/c4ix-desktop.md part thirteen). The desktop draws nothing until the
game ends, and then draws itself again. Every key is the game's, so Quit
on its menu is the way out. A stuck game can be killed from the page's
console, which gives the desktop the display back.

With `--window`, the game runs in its Terminal window and makes it its
canvas. Every frame then goes through the desktop, which is slower. Ctrl-C
there stops it, as it stops any program.

The music, the AdLib effects and the digitized sounds play through
libjs's sound card. In the browser they start with the first key or click,
because a browser keeps a page silent until it is used. The PC speaker's
sounds (the menu's "PC Speaker" choice) are silent.

The config file and saved games go beside the game in
`/usr/games/wolf3d`, as they did on DOS. In the browser they are kept
between visits, with the game's unpacked data, like every other change
to C4IX's files (docs/c4ix-design.md, "Persistence"), so only the first
visit unpacks the zip.

## Without the desktop

The web page's system list has three more ways to play it:

| `?system=` | what boots | the game |
|---|---|---|
| `wolf3d-c4ix` | C4IX's console, which runs `wolf3d` (init's `-e`) | on the whole display, as `raycast -G` draws; the first run unpacks the zip, as above, and it is kept |
| `c4dos` | C4DOS, on the page's own disk (`libjs/images/dos`), which starts grin (docs/grin-plan.md) | File > Exit grin in Program Manager, then type `WOLF3D` at the `A>` prompt; Quit comes back to it |
| `wolf3d` | the game itself, with no operating system | at once; Quit halts the machine |

The C4DOS and bare builds are the same game objects linked whole with
libc and with libc's `bare.c`, which puts the C library's file calls on
the machine's own opcodes. Under C4DOS it asks DOS for the files DOS has
(the API table, docs/c4dos-design.md), and its `exit` is DOS's
`dos_exit`, which returns to the prompt from however deep the game is.
Neither can write a drive, so the game's data comes unpacked on their
disk, beside Apogee's zip. Under C4DOS the config and saved games go to
DOS's RAM disk; on the bare machine nowhere. Neither keeps them between
visits.

These three entries run at 400 MHz, paced. The game times itself against
the machine's clock, so a faster clock than the page's 100 only gives it
room; `?mhz=` changes it.

## The first run

The shareware episode ships as Apogee distributed it:
`/usr/games/wolf3d/1wolf14.zip`, complete and unmodified, as its
`VENDOR.DOC` asks. The first time the game runs and finds no `vswap.wl1`
(`wolfinst.c`), it does four things:

1. It reads the zip and inflates `W3DSW14.SHR` from it, with libarc's
   `arc_inflate`, checking the CRC.
2. It walks that archive. Apogee's installer format is a header, then for
   each file a name and its compressed size.
3. It decompresses each file with `explode.c`. The format is PKWARE's
   Data Compression Library "implode", which zlib's `contrib/blast`
   documents; `explode.c` is a new implementation of that description.
4. It writes the eight data files, `vendor.doc` and `order.frm` under
   their names in lower case, which is how Wolf4SDL looks for them.

This is about 0.6G instructions, mostly the explode and the inflate: about
six seconds at the page's 100 MHz. After that the game starts at once.

## How it is put together

```
 Wolf4SDL (id_*.c, wl_*.c)
     |  SDL calls: surfaces, events, ticks, the mixer
 c4sdl.c -- SDL.h's functions over five platform calls (plat.h)
     |
 plat-c4ix.c                          plat-host.c (gcc, for the test)
     |  window.h: win_fb, win_poll        no window; frames to .ppm on request
 the Terminal (term.c): an 'f' op in its display list
     |  GUI_BLIT8: pixels and palette, by address
 libjs's display: the device turns them into 32-bit pixels when the
 desktop rings the bell, and the page draws them scaled to the window
```

**Drawing.** The game draws into one 8-bit 320x200 surface, as the
original did.
- Each frame, `plat_present` copies it, with the palette as 0x00RRGGBB
  words, into one of two buffers. It hands that buffer to the window with
  `win_fb`.
- The Terminal keeps the frame in its display list and draws it with
  `GUI_BLIT8`, scaled to its 640x400 canvas. The host reads the bytes and
  colours when the desktop next rings the bell.
- The Terminal then tells the program `f` (WIN_FRAME), and the buffer is
  free again. Until then a newer frame waits in the other buffer, and only
  the newest is sent.
- The game never waits for the window, and the window never shows a frame
  half drawn.
- The guest converts no pixels. In the guest, converting them would cost
  as much as rendering the frame.

**Keys.** The page sends key-up events as well as key-down ones. The
desktop used to drop them, and now passes them to a canvas Terminal as
`K` (WIN_KEYUP), because a game moves while a key is held.
`plat-c4ix.c` maps the browser's key codes to SDL scancodes, which
Wolf4SDL uses as its own.

**Files.** C4IX had no seek when this port was made (`lseek` came with
Busybox's M7c), and Wolf4SDL `fseek`s through its data.
`c4file.c` reads a file whole when it is opened for reading, so `fseek`
and `ftell` are arithmetic. A file opened for writing (the config, a
saved game) is gathered in memory and written at `fclose`.

**Sound.** `c4sdl.c`'s mixer calls go to libjs's sound card
(`libjs/guest/snd.h`, docs/libjs-design.md "The sound card") through
`plat_snd_*`.
- **The music** is Wolf4SDL's own player, `SDL_IMFMusicPlayer`, which
  SDL_mixer would call from its audio thread for each buffer. Here the
  game's loop calls it -- from the event pump, `SDL_Delay` and each
  frame -- until the card's stream is 100 ms ahead. Its OPL writes go to
  the card, and `YM3812UpdateOne`, which made the samples, only moves the
  stream on. So the music keeps its time however unevenly the game runs.
- **The digitized sounds** are the game's own 8-bit samples at 7,042 Hz,
  where the page file keeps them; the card plays them at that rate.
  `SD_PrepareSound` used to convert each to 44.1 kHz in floating point.
- **Channels.** Which are still playing is the card's to say, so
  `Mix_ChannelFinished`'s callback runs when one is seen to have stopped,
  and when a sound is halted or replaced, as SDL_mixer runs it.
- A timedemo is silent, so its frames stay the gcc build's, unless
  `--sound` asks for sound too.

**Time.** `SDL_GetTicks` is C4IX's machine clock (`clock()`), and
`SDL_Delay` is `SYS_SLEEP`. The game paces itself to 70 Hz tics, as
the original did.

## What c4lc needed

Porting a 36,000-line program found three things in the compilers. All
are fixed:

- **The comma operator** outside parentheses: `for (i = 0, j = n; ...;
  i++, j--)`, `a(), b();`, `return x, y;`. It is fixed in both compilers,
  which build the same bytes.
- **`&&` in `#if`** was its left operand alone in c4lc, because the
  preprocessor looked for the token under another name. c4fc's was right,
  so the two could disagree.
- **`libjs/cli.js`** could hang in Node's exit, in about one run in
  twenty. That is not the compiler, but every test of a compiled program
  runs through it.

The port first wrote around the rest, marking each change `c4sdl:`:
- 2-D arrays were flattened by hand;
- structs were passed by pointer;
- typedefs moved to file scope;
- float and `sizeof` initializers of globals moved into code;
- array parameters, braces around a scalar initializer and several
  declarators in one declaration were rewritten.

One of these was c4lc's ABI rather than a missing feature: every struct
member got a word of its own, `char` and `short` included. Data laid out
as it is in the files could not be read through a struct, as Wolf4SDL
does with fonts, sprites, the map headers, the picture table and the
huffman dictionary, so those were read a byte at a time.

All of this is c4lc's now, and c4fc's where it can follow
(docs/c4lc-design.md 14.8, "What Wolfenstein 3D needed"). A struct is
laid out by C's rule, as gcc lays it out, and `#pragma pack(1)`, which
`wl_def.h` sets for every struct after it, packs them as it does under
gcc. The game's code is Wolf4SDL's
again outside the port itself, and the timedemo's checksums did not
change.

Three things are noted for later, and none of them is in the game's way:

- c4lc counts a 32-bit double argument as two against a prototype's one,
  and warns "argument count mismatch". The code it generates is right.
- `#include`-ing `explode.c` into `wolfinst.c` made an image whose import
  table had nameless entries. Built as separate units, they link cleanly.
- A lazy file the disk's `c4ix.sizes` does not list loads wrongly. That is
  how the manifest's sizes work, but the error it gives is
  "needs ''".

Wolf4SDL had a bug of its own, fixed:

- `itoa` and `ltoa` sized their output by the `strlen` of the caller's
  uninitialized buffer, so the status bar's digits depended on whatever
  the stack held. valgrind found it, because the frames were not the same
  from run to run.

## Building it

`make wolf3d-objs` compiles each of the 28 units with c4lc into
`.wolf3d/`, and only those that changed. That takes about two minutes
with `-j2`, and `c4bb-images` depends on it. `build-images.sh` then
links the objects against C4IX's libc and puts the game and the zip on
the disk.

`make wolf3d-bare-objs` builds what the bare machine and C4DOS link
instead of C4IX's libc: `plat-bare.c`, and libc's units without `-fpic`,
with `bare.c` and `bareu.c`. `build-images.sh` links them with the same
objects, less `plat-c4ix.c`, as `images/games/wolf3d.c4r`, and makes
that directory a disk: the zip, the game's files unpacked from it (by
`tools/unshr.c` on the host), C4DOS's `CONFIG.SYS` and `AUTOEXEC.BAT`,
and the indexes the page and C4DOS read.

The sources stay out of C4IX's root archive, which every boot unpacks.
Rebuilding the game inside the machine would take hours, and they would
make that unpacking longer for nothing.

## Testing it

`--timedemo [N]` plays the first N of the four demos in virtual time:
each `SDL_GetTicks` is one millisecond later and `SDL_Delay` adds its
wait. A demo therefore plays the same frames on any machine. Every frame
shown, with its palette, is folded into a checksum. `make test-wolf3d`
plays demo 0 twice:
- built by gcc on the host (`make wolf3d-host`);
- built by c4lc, inside C4IX on libjs, after unpacking the zip.

Then it plays it on the bare machine and under C4DOS, from the
`images/games` disk, the two at once; C4DOS must then be back at its
prompt. All four must print the same 1,214 frames and checksum. The
host prints:

```
timedemo: demo 0: 1214 frames, checksum d7358845
timedemo: demo 1: 1346 frames, checksum 1867c52a
timedemo: demo 2: 733 frames, checksum 1c7166d3
timedemo: demo 3: 695 frames, checksum 73562491
```

`wolf3d --sound --timedemo 1` plays the demo with its sound, and
`node libjs/cli.js --wav FILE` writes it out. The music then keeps the
machine's time, not the demo's virtual time, so the two drift apart.

## How fast

The timedemo's machine time inside C4IX, at the page's 100 MHz. The
level load and the checksum of every frame are included.

| demo | frames | first c4lc build | now | fps now |
|---|---|---|---|---|
| 0 | 1214 | 104.3 s (11.6 fps) | 36.9 s | 32.9 |
| 1 | 1346 | 115.0 s | 41.1 s | 32.7 |
| 2 | 733 | 64.1 s | 22.4 s | 32.7 |
| 3 | 695 | 57.0 s | 20.3 s | 34.2 |

That is about 3M VM instructions a frame. The page runs libjs at about
110M a second unpaced, so the game is held to its machine's 100 MHz, not
to the host.

Five changes made it 2.8 times faster. The profile came from
`libjs/tests/prof-c4ix.mjs`, with `PROG="wolf3d --timedemo 1"` and
`USERIMGS=c4ix-wolf3d.c4r`.

1. **`-mfuse`**, c4lc's fused opcodes, which libjs runs.
2. **`FixedMul` in 32-bit pieces.** The original multiplies in `int64_t`,
   which c4lc does at 32 bits through helper calls. The two give the same
   bits on 200M random and edge cases.
3. **No sound conversion.** At start-up, `id_sd.c` converted every
   digitized sound to 44.1 kHz in floating point. That was about 1.1G
   instructions. The sound card plays them as they are.
4. **The timedemo's checksum** reads the frame a word at a time, not a
   byte.
5. **`int`s in the scalers** (`ScalePost`, `ScaleLine`, the shape
   functions), not `int16_t`. A short is two byte loads and a sign
   extension in c4lc. Their locals hold the globals they read for every
   pixel. One `int16_t` stays: a sprite's column offsets may be negative,
   and that one says so.

The same checksum on every demo shows that none of this changed a pixel.

Next, from the same profile:
- the wall caster's `short` globals (`xtile`, `xtilestep`, …) on every
  step of every ray;
- `FixedDiv`'s 64-bit divide, once a sprite;
- the profile attributes static functions and c4lc's spliced helpers to
  the global function before them, which hides where some cycles really
  go.
