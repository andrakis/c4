# More games for C4IX — what the compilers need

After Wolfenstein 3D (docs/c4ix-wolf3d.md), the next ports are, in this
order, **Doom** and a **Raptor-like shooter**. Rise of the Triad was added
to the list while this was written. This file sizes the work the
compilers need for each and sets the order of the fixes (§8). Nothing in
the compilers was changed for it.

The rules for these ports:
- plain C only, no C++;
- compiler gaps are fixed as they come up; this file only finds and sizes
  them;
- memory is not a concern (`?mem=64` raises libjs's arena);
- Quake is out for now.

Status (2026-10-23): **Doom is ported** (src/doom/README.md): doomgeneric,
on C4IX's whole display, under C4DOS and on the bare machine, its demos'
frames the same as gcc's. Step 6 is done (M11); doomgeneric's one
bitfield struct is written as bytes instead (step 7). **OpenTyrian is
ported** too (src/opentyrian/README.md), the same three ways, with steps
8-10 done (M12, c4lc-design.md 14.11). Its data, Tyrian 2.1, may not be
passed on, so it is on the disks only where its zip was there to copy.
**Rise of the Triad is ported** (src/rott/README.md), icculus.org's port,
the same three ways, with steps 11 and 12 done (M13, c4lc-design.md
14.12). Its sound is still to come.
- **c4lc** compiled 50 of linuxdoom's 59 units as they stood. The first
  five steps of §8, small and medium fixes, make it compile all of Doom.
  Twelve steps, none of them large, cover Doom, OpenTyrian and Rise of
  the Triad.
- **Steps 1–5 are done** (M10, c4lc-design.md 14.9): c4lc compiles all
  59 units and the platform stub as they stand, and `doomprobe.c` without
  its rewrites prints what gcc `-m32` prints, on `c4m32`, plain, `-O` and
  `-minline`. c4fc took steps 1, 2 and most of 4.
- **c4fc** compiles none of them, because it has no `typedef` (§4.1).
- **OpenTyrian** needs the C99 that c4lc lacks: declarations in `for`,
  struct values and compound literals.
- **Raptor** is C++ only in name and converts to C in a day or two. Its
  code is reverse-engineered, though, and its data is not ours to ship.
  So OpenTyrian is the recommended second target (§6).

## 1. The sources

All are under the GPL v2. Copies are in `D:\git\c4-ports` on the Windows
machine and in `~/ports` on helios, outside the repository. None is
vendored.

| game | source | commit | C | size |
|---|---|---|---|---|
| Doom | [id-Software/DOOM](https://github.com/id-Software/DOOM), `linuxdoom-1.10` | `a77dfb9` | C89 | 59 units, 52,257 lines |
| Doom, platform reference | [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric) (Chocolate Doom's code, six platform calls) | `e257a09` | C99 | 80 units, 73,140 lines with its ports |
| Tyrian | [opentyrian/opentyrian](https://github.com/opentyrian/opentyrian) | `ac5d0e7` | C99 | 52 units, 50,621 lines |
| Raptor | [skynettx/raptor](https://github.com/skynettx/raptor), nukeykt's reverse-engineered code | `746c35a` | C++ | 45 `.cpp`, 34,948 lines with headers, and 4,557 of setup |
| Rise of the Triad | Apogee's 2002 release, [videogamepreservation/rott](https://github.com/videogamepreservation/rott) | `6ab87f2` | Watcom C, asm | 53 units, 113,845 lines, 2,148 of asm |
| Rise of the Triad, portable | [icculus.org/rott](https://icculus.org/rott/) 1.1.2 tarball, md5 `d523c7c6…` as published | — | C89 | 50 units, about 116,000 lines |

## 2. How it was measured

The pipeline is the one test-c4fc's `C4FC_GEN_SWEEP` and test-c4lc use:
`gcc -E`, then c4lc and c4fc on the same preprocessed text.
- **gcc -E** runs with `-undef -nostdinc`, C4IX libc's headers
  (`src/c4ix/libc/include`) and `-DC4CC=1 -D__c4__=1 -D__C4CC__=1
  -D__c4cc__=1 -D__c4_inttypes__=1`.
- **c4lc** is `c4sp32 c4lc.lisp -fno-builtin -c`, for the 32-bit target
  that C4IX on libjs is.
- **c4fc** is `c4th $(C4FC_ALL)` with `-c`.

As src/wolf3d did, stubs stand in for what is not the game:
- SDL 2 for OpenTyrian is src/wolf3d's `sdl/SDL.h`, grown by what
  OpenTyrian calls.
- SDL 1.2 and SDL_mixer for ROTT are a new stub of the same kind.
- Doom's X11, OSS and socket files (`i_video.c`, `i_sound.c`, `i_net.c`)
  are replaced by one file in doomgeneric's shape: a framebuffer, no
  sound, single player.
- The libc functions and macros C4IX lacks are declared in small headers
  of their own (§7), so that they are not taken for compiler gaps.

Each unit was compiled until it failed. Then the preprocessed text was
rewritten past that one failure and compiled again, until it went
through. Each kind of failure was then counted in the source. The stubs,
scripts and rewritten units are in `~/ports/scan` on helios. The hand-over
notes are in the session notes (`ports-scan-2026-10-23.md`).

A compiler that accepts code can still compile it wrongly. For Doom,
`doomprobe.c` checks for that (§3.3). It is a program of Doom's own idioms,
built by gcc `-m32` and by c4lc and run on `c4m32`.

## 3. Doom

### 3.1 What c4lc rejects

50 of the 59 game units compile as they stand, and so does the platform
stub. The other nine stop on the first item of their unit in this table.
Once that item was rewritten, each went through after the rest of the
table had been rewritten too.

| gap | sites | units | fix |
|---|---|---|---|
| `register` | 13 | am_map (12), s_sound | S: drop the word, as `inline` is dropped |
| a declaration inside a block: `extern int forwardmove[2];`, `void V_MarkRect(int, int, int, int);` | 6 | d_main 3, g_game, f_wipe, wi_stuff | S: put it in the file scope's table |
| `&a[k]` as a global's initializer: `boolean *mousebuttons = &mousearray[1];` | 3 | g_game 2, r_main | S: it is `a + k`, which c4lc already sets at start-up (c4lc-design 14.8) |
| an address with a cast or an offset in an aggregate initializer: `(int *)&sndserver_filename`, `(int)"sndserver"`, `&S_sfx[sfx_pistol]`, a row `cheat_powerup_seq[0]` | 13 entries of `defaults[]`, 1, 8 | m_misc, sounds, st_stuff | M: casts fold to nothing; an address plus an offset needs a data patch that carries the offset |
| floating point in an aggregate initializer: `{ -.867*R, -.5*R }` | 16 | am_map | M: fold float arithmetic in constants and convert to the member's type |
| a switch whose cases span 4096 or more: `case AM_MSGENTERED:`, which is `('a'<<24)+('m'<<16)` plus a little | 1 | st_stuff | S–M: past a density limit, compares instead of a table |

S is under a day for one compiler, M one to three days, L a week or more.

doomgeneric meets the same items, and also these:

| gap | sites | fix |
|---|---|---|
| a typedef of an array type: `typedef byte sha1_digest_t[20];` | 1, in a header 44 units include | M (§5) |
| bitfields: `struct color { uint32_t b:8; ... }` | 1 struct, 4 members, in i_video.h, which 50 units include | M: layout, and masked loads and stores |
| a double constant initializing a float: `float mouse_acceleration = 2.0;` | 1 | S: convert it, as C does |
| another sparse switch: `case ANG45:` and the like in `G_CheckSpot` | 1 | the switch fix above |

With those rewritten, all 81 of doomgeneric's units compile too, its
platform stub among them.

c4lc's only warnings on Doom were two about a dropped `const`, in
`m_misc.c`. It did not warn about the `open` and `close` that Doom's
`p_spec.h` declares as enum constants. gcc rejects
those, because C4IX's `libc.h` declares the functions `open` and `close`
in every standard header. In c4lc the enum constants win, and the probe
gets their values right, so the program compiles correctly. The cleaner
fix is in libc: split `libc.h` so that `<stdlib.h>` does not declare
`open` (§7).

### 3.2 Floating point, and what Doom assumes about the machine

Doom is fixed-point, and its floating point is small:
- **`am_map.c`** has the 16 constants above, four `(int)(1.02*FRACUNIT)`
  in code, and `scale_mtof = .2*FRACUNIT`. All are constants.
- **`m_fixed.c`'s `FixedDiv2`** divides in `double`. doomgeneric divides
  the same thing in `int64_t` instead.
- **`r_main.c` and `p_maputl.c`** use floats only inside `#if 0`.

So the float library is spliced into two units, and could be taken out of
both.

What Doom assumes about the machine, against C4IX on libjs:

| assumption | where | on C4IX |
|---|---|---|
| a pointer fits in an `int` | `m_misc.c`'s `defaults[]` keeps 13 strings in `int` members; `p_saveg.c` keeps array indices in pointer fields across a save (9 sites) | holds: 32-bit target |
| `long` is 32 bits | `state_t`'s `frame` and `tics` are `long`, as is `MAXLONG` | holds: c4lc's `long` is the word |
| 64-bit products | `FixedMul` and `FixedDiv` in `long long` | correct; slow helper calls. Wolf3D's `FixedMul` in 32-bit pieces applies (docs/c4ix-wolf3d.md, "How fast") |
| little-endian | `SHORT()` and `LONG()` are the identity unless `__BIG_ENDIAN__`, at 147 sites | holds |
| unaligned word reads | `*(int *)name` compares of 4-character lump names (`r_things.c`, `w_wad.c`, 3 sites) | correct: libjs reads an unaligned word through `DataView`, on a slow path (`arena.js`) |
| `char` is signed | `ticcmd_t`'s `forwardmove` and `sidemove` | holds: c4lc's `char` is signed |
| `short` is 16 bits | the map lumps (`mapvertex_t`, `maplinedef_t`, ...) | holds, with C's layout (c4lc-design 14.8) |
| packed structs | doomgeneric marks 10 lump structs `__attribute__((packed))` | c4lc drops the attribute. gcc `-m32` gives all ten the same size packed or not, so it does no harm here |
| a struct offset through a null pointer | `d_net.c`: `(int)&(((doomdata_t *)0)->cmds[n])` | holds: libc's `offsetof` is the same idiom |

### 3.3 Does c4lc compile it right?

`doomprobe.c` has 15 checks, each printed:
- `FixedMul` and `FixedDiv` over 121 pairs of values, and `FixedDiv2` in
  double;
- angle arithmetic in `unsigned`, wrapping, shifting and comparing;
- right shifts, division and remainder of negative numbers;
- the thinker union of function pointers, set and called through, and
  compared with `(actionf_v)(-1)`;
- `info.c`'s states table, a union of a function in braces;
- map records read from bytes at odd offsets;
- `ticcmd_t` with signed `char` and `short` members, a struct copy, and the
  checksum over it as `int`s;
- the null-pointer offset;
- the defaults table, with strings kept in `int`s;
- `finecosine` and `mousebuttons` as offsets into arrays;
- an aggregate holding a pointer to its own element;
- the `*(int *)` name compares;
- varargs;
- the sparse switch.

Where c4lc rejects a construct, the probe has the rewrite as well
(`-DWORKAROUND`). gcc gives the same output both ways.

**c4lc's image, run on `c4m32`, prints what gcc `-m32`'s build prints,
plain, `-O` and `-minline`.** No wrong code was found. The 64-bit build
differs where it should, in the `int`-sized checksum and offsets.

### 3.4 Which Doom

Both trees compile once §8's first five steps are done. doomgeneric then
also needs steps 6 and 7 (array typedefs and bitfields), or two lines
changed by hand.

doomgeneric is the better base for the port:
- its platform layer is six calls, which map onto `plat.h` as Wolf4SDL's
  SDL did;
- its fixed-point division needs no floats;
- its savegame code does not keep pointers in `int`s.

Neither tree has music that suits C4IX:
- linuxdoom plays music through a separate sound server;
- doomgeneric plays it through SDL_mixer or Allegro.

Chocolate Doom's `i_oplmusic.c` plays the MUS tracks through an OPL,
which libjs's sound card is. It is what Raptor's `i_oplmusic.cpp` was
taken from.

## 4. c4fc

### 4.1 It has no typedef

c4fc rejects every unit of every game here at the same place. That is the
first line of C4IX's `libc.h`, `typedef int size_t;`, which says "c4fc: a
declaration was expected". c4fc's lexer knows the keyword, but nothing
parses it (docs/busybox-port.md §7 says the same). So c4fc cannot compile
any program that includes C4IX libc's headers. That holds whether the text
comes through gcc `-E` or through c4fc's own `-P`.

### 4.2 What else it lacks

Doom's probe was rewritten without typedefs: simple types as `#define`s,
structs by their tags, function-pointer types spelled out. That rewrite
was compiled with c4fc, beside c4lc at 64 bits. It found:
- **floating point**, which c4fc refuses by design (c4fc-design, "What
  this is"): `FixedDiv2`;
- **an initializer computed at start-up**, `int *finecosine = finesine +
  2048;`: "c4fc: not a constant";
- **a declaration after a statement**, C99's mixed declarations, which
  c4lc takes: `int i; i = 1; int j = 2;`. Doom never mixes them.
  OpenTyrian does throughout.

With those three taken out, **c4fc's image is byte-identical to c4lc's,
and prints the same.** So, for these games, c4fc needs:
1. typedef (L: c4fc's parser has no typedef table, and C's grammar needs
   one);
2. floats (L, docs/c4-float.md);
3. mixed declarations (S–M);
4. start-up initializers (M);
5. then every c4lc item in §8 a second time, since `test-c4fc` holds the
   two to the same bytes.

c4lc builds the games. c4fc's part is a parity track of its own, and the
ports need not wait for it.

## 5. OpenTyrian

As it stands, c4lc compiles 5 of its 52 units. With each gap below
rewritten, 41 compile. The other 11 stop on items already in the table,
in places the rewriting did not reach:
- a `for` that a macro declares;
- a sparse switch over key and modifier;
- a pointer to an array type;
- a struct value or a compound literal.

| gap | sites | fix |
|---|---|---|
| a declaration in `for`: `for (int i = 0; ...)` | 440, in 41 files | S–M: a block around the loop. Both compilers |
| a typedef of an array type: `typedef SDL_Color Palette[256];`, `typedef JE_byte *JE_MapType[300][14];`, `typedef struct {...} JE_WeaponPortType[43];` | 17 typedefs and 1 of a struct, in 7 headers; 4 `sizeof`s of one; 1 pointer to one (`Palette *`) | M: the typedef carries its dimensions; `T (*p)[N]` is needed too |
| a function returning a struct, used: `File f = dataFileOpen(...)` | 8 functions, about 45 calls | M: the call's result in a temporary, as long long's is (c4lc-design 14.7) |
| a struct initialized from a struct, even a variable: `P q = g;` ("cannot initialize a struct by value") | most of the calls above | S: a declaration, then a struct assignment |
| compound literals: `return (MemReader){ 0, 0, 1 };`, `(Uint8[3]){ 0, 0, 0 }` | 7 | M: a temporary with an aggregate initializer |
| casts in a `case` label: `case (Uint32)SDL_SCANCODE_Q \| (... ? (Uint32)KMOD_SHIFT : 0) << 16:` | 18 | S: fold casts in constant expressions |
| a switch over key and modifier (cases 2¹⁶ apart) | 2 | §3.1's switch fix |
| `sizeof` of a local array in an array size: `MenuId menuParents[sizeof(menus) / sizeof *(menus)]` | 1 | S: c4lc takes it for globals (14.8), and needs it for locals too |
| floating point in an aggregate: `static const double attackconst[4] = { (double)(1/2.82624), ... }` | opl.c's constant tables | §3.1's float-constant fix |
| a block-scope `extern` | 7 | §3.1's fix |

Floating point is real here, where Doom's was not:
- `opl.c` is the AdLib emulator. It goes, as MAME's OPL went from Wolf3D,
  because libjs's sound card is the OPL.
- `destruct.c` is the Destruct mini-game's physics.
- `mtrand`, `mainint.c`, `tyrian2.c` and `starlib.c` have floats here and
  there.

c4lc has floats, so this costs compile time, not work.

OpenTyrian also writes a lot of C99 that c4lc already takes: `bool` at
350 sites, designated initializers, 67 `static inline` functions and 33
`goto`s.

c4lc printed "argument count mismatch in call to blit_sprite2x2 - expected
5 given 6" on `mainint.c`. The parameter is a struct passed by value, two
words. A probe of the same call gave the right answer and no warning, so
this needs looking at again when the port starts. Wolf3D's note about a
double argument (c4ix-wolf3d.md) is the same kind of warning.

## 6. Raptor

Raptor is C++ only in name. Across its 45 `.cpp` files there is:
- no `class`, template, namespace, `std::`, `new` or `delete`, or operator
  overloading;
- one `nullptr`.

Compiled as C with gcc, the files give 321 errors. Three things get them
down to 51:
- a typedef for each of the 17 struct tags it uses as type names;
- `<stdbool.h>`;
- `<string.h>` for `<cstring>`, and the like.

The 51 left are a handful of lines:
- `using byte = uint8_t;`;
- one default argument, in `i_video.h`;
- one `auto`;
- `textscreen.h` for the setup program, which is Chocolate Doom's C
  library;
- the ALSA MIDI driver, which C4IX does not need.

Converting it to C is a day or two of mechanical work, and then it is
another C port.

**OpenTyrian is the recommended second target anyway.**
- **Provenance.** Raptor's code is nukeykt's reverse engineering of the
  commercial game, not a source release, though the repository carries
  the GPL v2. Its README points at the original author's remake, which is
  sold. OpenTyrian is a port of the original game's own source.
- **The data.** Tyrian 2.1 is freeware and can ship on the disk. Raptor's
  data cannot (§9).
- **What the fixes buy.** OpenTyrian's gaps are C99 that any later C
  program will meet: `for` declarations, struct values, compound
  literals, array typedefs. Converting Raptor teaches the compilers
  nothing.

Both are vertical shoot-'em-ups with AdLib music, the kind of game asked
for.

If Raptor is wanted later, convert it to C first. Its gaps should then be
a subset of OpenTyrian's and Doom's, because it is Doom-era code with
Chocolate Doom's music player.

## 7. Rise of the Triad

The DOS release (Apogee, 2002) is Watcom C for 32-bit protected mode:

| DOS construct | lines | files |
|---|---|---|
| `#pragma aux`: inline asm and register calling conventions (`FixedMul`, `VGAMAPMASK`, the column drawers) | 32 | 14 |
| `.ASM` files: column and row drawers, the ray caster, `rand` | 2,148 | 9 |
| `far`, `near`, `huge` | 71 | 14 |
| `interrupt` handlers | 24 | 10 |
| `inp` and `outp` (VGA registers, the PIT, the keyboard) | 48 | 8 |
| `int386`, `_dos_*` | 65 | 17 |
| `<dos.h>`, `<conio.h>`, `<i86.h>`, `<bios.h>`, `<graph.h>`, `<io.h>` | 79 includes | 35 |

None of this is a compiler gap. It is the platform, which icculus.org's
port (1.1.2) has already replaced with C and SDL 1.2:
- `watcom.c` does `FixedMul` and the rest in `long long`;
- the drawers are C;
- Mode X's four planes are kept in a linear buffer.

That port is the one to build on, as doomgeneric is for Doom. The DOS
release stays the reference for what the game did.

c4lc compiles 29 of icculus's 50 units as they stand. With Doom's fixes
and the five below, 48 compile. The other two stop on a sparse switch
(`rt_menu.c`, a span of 30,001) and on an opaque pointer compared with 0
(`dukemusc.c`). Both are in §8. The scan defines `__GNUC__` for this tree,
because `rt_def.h` picks the platform by compiler and stops with `#error`
otherwise. The real port says C4IX there.

| gap | sites | fix |
|---|---|---|
| casts in an aggregate initializer: `{ CP_CursorLocation, "mm_opt1\0", 'N', (menuptr)CP_NewGame }` | 111, in rt_menu.c | §3.1's initializer fix |
| `int + pointer`: `*(dest+bufferofs) = pixel;` where `dest` is an `int` | 15, in rt_game.c | S: the operands of `+` swapped when the pointer is on the right ("bad dereference" today) |
| K&R function definitions: `int CompareHeights(s1p,s2p) visobj_t **s1p,**s2p;` | 5 | S–M |
| a pointer to a pointer to a function: `movement_status (**movement_function)(objtype*,int,int,int);` | 1 | S |
| a string that fills its array exactly once its NUL is dropped: `char SaveName[13] = "rottgam?.rot\0";` | 1 | S: C drops the NUL; c4lc says "string does not fit" |
| a pointer to a never-defined struct compared with 0: `if (music_musicchunk == NULL)`, where `Mix_Music` is opaque | 1 | S: "struct used before its definition" today |
| block-scope `extern`s and prototypes; an array typedef; sparse switches | 5; 1; 1 | §3.1, §5 |

Floating point: 21 lines in 8 files, nearly all in `WinRott.h`'s and
`winrott.c`'s screen scaling.

The sound library (`audiolib`, 25,000 lines of Apogee's DOS drivers)
was not scanned: libjs's sound card replaces it, as with Wolf3D.

## 8. Order of work

Steps 1–5 build Doom. Steps 6–7 add doomgeneric, steps 8–11 OpenTyrian,
and step 12 Rise of the Triad. Each step lands as M7b's did:
- in c4lc;
- a gcc-checked test in `src/tests/c4lc_*.c`;
- c4fc where its grammar reaches.

| step | what | for | size | c4fc | done |
|---|---|---|---|---|---|
| 1 | `register`; declarations inside a block | Doom, ROTT, dg | S | S | c4lc and c4fc |
| 2 | sparse `switch`: compares, or a binary search, past the density limit | all four | S–M | S–M | c4lc and c4fc: compares |
| 3 | `&a[k]` as a global's initializer | Doom, dg | S | with start-up initializers (§4.2) | c4lc, at start-up |
| 4 | casts and address-plus-offset in aggregate initializers; casts in constant expressions (`case`) | Doom, ROTT, Tyrian | M | M | c4lc; c4fc but `a + k`, members and a string plus k |
| 5 | float arithmetic folded in constant initializers, converted to the target type (`float f = 2.0;` too) | Doom, dg, Tyrian | M | after floats | c4lc, folded exactly |
| 6 | typedefs of array types, pointers to arrays (`T (*p)[N]`) | dg, Tyrian, ROTT | M | after typedef | c4lc (M11, c4lc-design 14.10) |
| 7 | bitfields | dg (one struct) | M | M | |
| 8 | declarations in `for` | Tyrian | S–M | S–M, with mixed declarations | c4lc (M12, c4lc-design 14.11) |
| 9 | struct values: returned and used, copy-initialized, compound literals | Tyrian | M | M | c4lc (M12) |
| 10 | `sizeof` of a local array in a constant | Tyrian | S | — | c4lc (M12) |
| 11 | small ones: an exact-fit string, a pointer to a pointer to a function, an opaque struct pointer compared with 0 | ROTT | S | S | c4lc (M13, c4lc-design 14.12); the opaque pointer is the shim's (dukemusc.c is not built) |
| 12 | `int + pointer`; K&R definitions | ROTT | S, S–M | S | c4lc (M13) |

Each fix is small, and none needs a new pass. Steps 4 and 9 are the
largest:
- step 4 may need c4rlink to carry an offset in a data patch;
- step 9 follows the temporaries that long long values already use
  (c4lc-design 14.7).

Steps 1–5 landed as M10 (c4lc-design 14.9, c4fc-design "What Doom
needed"; tests `c4lc_m10.c` and `c4lc_m10lc.c`):
- `register` alone before a name is C89's int, am_map.c's `register
  outcode1 = 0;`, which the scan's rewrite had hidden.
- Step 4's data patch carries its offset; for another unit's data the
  offset is left in the data word, and c4rlink adds it.
- Step 5 folds at compile time, exactly: rationals over the lexer's big
  integers, rounded after each operation as C rounds. A scalar global
  with more than a literal still goes to start-up (14.8), as before.

c4fc's own items (§4.2) come before its column: typedef, floats, mixed
declarations and start-up initializers.

## 9. libc and the data

### 9.1 What libc lacks

Not the compiler's, but each port needs it. The scan declared these in
stub headers:
- **Doom:**
  - `fseek`, `ftell`, `setbuf`, `fscanf` and `sscanf`;
  - `R_OK` and the other `access` modes;
  - `values.h`'s `MAXINT` and the like (11 files);
  - `gettimeofday`;
  - `alloca`, called in `r_data.c` and `w_wad.c`. malloc and free will
    do there, or c4lc could make it a builtin.
- **doomgeneric:**
  - `<stdint.h>`, `<stdbool.h>` and `<inttypes.h>` (the
    `c4-stddef-inttypes` branch on helios is working on these);
  - `SHRT_MAX` and its kin;
  - `atof` in `<stdlib.h>`, where C puts it (C4IX has it in `math.h`);
  - `system`.
- **OpenTyrian:**
  - `int_fast16_t` and the other fast types;
  - `localtime` and `struct tm`;
  - `sscanf`.
- **ROTT:**
  - `S_IREAD`, `S_IWRITE` and `S_IRWXU`;
  - `alloca`.
- **All of them:** `libc.h` declares everything in every standard
  header, so a game's own `open` or `close` collides with it (§3.1).
  Splitting it per header is the cure.

libc's `va_arg(ap, double)` steps one word where a double is two. No port
here calls it with a double.

### 9.2 The data

- **Doom.** The shareware `doom1.wad` is freely redistributable,
  unmodified. It can go on the disk as `1wolf14.zip` does, and the scan
  did not fetch it.
- **OpenTyrian.** Tyrian 2.1's data, which its author released as freeware.
  OpenTyrian's README points at `tyrian21.zip` (camanis.net), and its
  release builds bundle it. Read the licence text inside the zip before it
  goes on the disk, as Wolf3D's `VENDOR.DOC` was read.
- **Raptor.** It needs `FILE0000.GLB` and `FILE0001.GLB` from the
  shareware version 1.2 or later; the full game adds `FILE0002–0004.GLB`.
  The repository ships none and asks the player to supply them. Whether
  the shareware files may be passed on was not checked. Until it is, a
  C4IX disk cannot carry them.
- **Rise of the Triad.** The shareware v1.3 `1rott13.zip`, Apogee's
  package, which icculus.org mirrors. Ship it unmodified, as Wolf3D's
  zip is shipped. The 2002 source release (GPL) has no data.

The music of OpenTyrian, Raptor and Rise of the Triad is AdLib or MIDI
played on an OPL. Doom's is MUS, played through Chocolate Doom's OPL
player. All of them go to libjs's sound card, as Wolf3D's music does.
