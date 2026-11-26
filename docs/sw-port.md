# Shadow Warrior on c4

Asked for on 2026-11-26, with Blood: the two Build-engine games after Duke
Nukem 3D (docs/duke3d-port.md), ported the way Duke was. Blood was left out
(below). `games/sw/README.c4.md` describes the port as it is; this is how
it came about, and its numbers.

## 1. The source base: JFShadowWarrior

| candidate | what it is | verdict |
|---|---|---|
| 3D Realms' release (2005, GPL) | the game for Watcom and DOS, with Ken's assembly and `#pragma aux` helpers | all of that to write again in C, which jfsw has done |
| **JFShadowWarrior** (Jonathon Fowler, 2005 to now) | that release, made portable: C99 throughout, on his JFBuild (Ken's engine with `a-c.c`, C for the assembly), jfaudiolib and JFMact, all C; plays the shareware and registered 1.2 | **chosen** |
| VoidSW, Raze | C++ on EDuke32's or GZDoom's engine | no C++ compiler here |

jfsw's three submodules (JFBuild, jfaudiolib, JFMact) are copied into
the game repository as directories, at the commits jfsw names, so c4 takes
the game as one repository (andrakis/c4-SW, branch `c4`). Only the
classic renderer is built (no Polymost, whose only C++ is its texture
compression). jfaudiolib is not built at all: `c4/sw-snd.c` gives its
`FX_`, `MUSIC_` and `CD_` calls on libjs's sound card, as Duke's
`duke-snd.c` does, with Duke's copy of Apogee's MIDI player and AdLib
driver for the music.

Licences: the game is GPL-2 (3D Realms, 2005); the engine is Ken
Silverman's Build licence (`jfbuild/buildlic.txt`, on every disk the
engine is on, as `sw-buildlic.txt` and `SWBUILD.TXT`).

## 2. Blood

There is no C source for Blood. Monolith wrote it in C++, never released
it, and every source there is (NBlood/Blood-RE, a matching decompilation,
64,000 lines with 75 classes; NBlood/chocolate-blood, a work in progress
on it) is C++ whose GPL notice is the decompiler's, not the rights
holder's. Porting it would have meant converting it to C, with its licence
unsettled; the user left it out for now (2026-11-26).

## 3. The data

`3dsw12.zip`, 3D Realms' shareware package of 1.2 (13,299,147 bytes, from
the Wayback Machine's copy of 3D Realms' FTP), fetched by its SHA-256 by
`make sw-data`; `SW.GRP` in it (26,056,769 bytes, CRC `08A7FA1F`) becomes
`SWSW.GRP` on the disks. Its `LICENSE.TXT` (V.5.01.97) lets individuals
give copies away without charge, all files unmodified; unlike Duke's later
licence (V.7.16.98) it names no web or FTP sites. c4's page is free, and
the user chose to put the episode on the published site (`libjs/pages.sh`,
2026-11-26), as Duke's and ROTT's are. The full game is
the person's own `SW.GRP` (`sw -home`); Steam's Shadow Warrior Classic has
it, byte for byte the registered 1.2 by its size.

## 4. What c4lc needed

Five things, each ordinary C (c4 commit "c4lc: what Shadow Warrior's
sources need"):

- a backslash before a CR LF continues the line: jfaudiolib's files have
  DOS line ends (in c4fc too);
- `typedef T A, *B, C[N];`, several names in one typedef;
- typedefs of function types, `typedef int ANIMATOR(short);`, whose name
  then declares functions (`ANIMATOR DoBunnyMove, NullBunny;`) and with a
  `*` points to one: about 250 lines of SW;
- a static local's initializer naming static locals (the state rings,
  `static STATE s[] = { ..., &s[1] }`, and tables of them);
- `extern T a[];` in a block no longer forgets the size the unit gave `a`.

## 5. Where gcc's build and c4lc's parted

The test holds c4lc's frames to gcc's build, every pixel and the palette.
What made them differ, in the order found:

- **The test's clock.** It moved a millisecond each time it was read, and
  JFBuild samples the timer while it draws, every 16 columns of a wall
  (`faketimerhandler`): how often depends on the renderer's path, which
  differed. It now moves as the game looks for its input and with each
  frame.
- **`RAND_MAX`.** `rand` is one function in every build (sw-sum.c's,
  0 to 32767); glibc's `RAND_MAX` of 2147483647 scaled the screen shakes
  differently.
- **`SIZE_MAX`.** C4IX's `size_t` is an `int`, and an unsigned `SIZE_MAX`
  was -1 there: JFBuild's cache took every place as full.
- **The cache's start and contents.** `initcache` aligns the cache to 16
  and shortens it by the misalignment, which differs between glibc's
  malloc and c4's; and the engine draws one texel past a tile's end in
  places, from the slot's padding, which c4's malloc fills with what was
  there (a file it had read). The cache is now its full length everywhere
  and cleared when it is made.
- **No sound card.** With FX unavailable the game left its sound on, and
  an intermittent ambient sound that could not start queued itself again
  in the same pass until memory ran out.

## 6. How fast

`node tools/framebench.mjs sw` (test-sw's script on E1L1, 600 frames, on
node's bare machine), M instructions a frame, median / mean / 99th
percentile:

| build | median | mean | p99 | at 100 M instructions/s, average |
|---|---:|---:|---:|---:|
| first build | 7.41 | 7.54 | 12.97 | 13.3 fps |
| 64-bit helpers in 32-bit operations, as macros | 6.46 | 6.54 | 12.01 | 15.3 fps |
| with the accelerators (`c4/accel.mjs`) | 2.73 | 2.81 | 4.60 | 35.6 fps |

The first profile's median frame: 64-bit arithmetic 24% (`__c4ll_mul`
14.6%, `__c4ll_udivmod` 5.3%, `__c4ll_sar` 4.0%), then the loops
(`vlineasm1` 14.4%, `mspritevline` 8.2%, `hlineasm4` 5.8%); its heaviest
frames went to translucent sprites (`tspritevline`, 44%). The
accelerators run JFBuild's ten loops and five 64-bit helpers in the page
(docs/accel.md); c4pc runs the same in C (`src/c4pc/games/sw.c`, branch
`c4pc-sw`), where test-sw's script takes 8.5 s against 17.2 s without them
on helios, the frames the same.

In the page (framebench --page on the CDP 9224 Chrome, C4IX at 400 MHz with the
MMU, the script shown with -show, 2026-11-26): 31.7 frames a second on average
with the accelerators (1% low 12.3), 16.0 without them (8.2).
