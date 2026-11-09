# games

The games c4 runs -- Wolfenstein 3D, Doom, OpenTyrian, Rise of the Triad
and the BSD games -- are not part of c4's repository. Each lives in one of its own,
on GitHub, on upstream's history with c4's port on top as commits, and
is an optional submodule here:

| path | repository | upstream |
|---|---|---|
| `games/wolf3d` | [andrakis/c4-Wolf3D](https://github.com/andrakis/c4-Wolf3D) | Wolf4SDL, KS-Presto's fork |
| `games/doom` | [andrakis/c4-Doom](https://github.com/andrakis/c4-Doom) | doomgeneric |
| `games/tyrian` | [andrakis/c4-Tyrian](https://github.com/andrakis/c4-Tyrian) | OpenTyrian |
| `games/rott` | [andrakis/c4-ROTT](https://github.com/andrakis/c4-ROTT) | icculus.org's Rise of the Triad 1.1.2 |
| `games/pdcurses` | [andrakis/c4-PDCurses](https://github.com/andrakis/c4-PDCurses) | PDCurses, and a curses for C4's machines |
| `games/bsdgames` | [andrakis/c4-BSD-Games](https://github.com/andrakis/c4-BSD-Games) | bsd-games 2.17, Debian's copy of it |

A clone of c4 has none of them (`update = none` in `.gitmodules`).
To have one:

```
git submodule update --init --checkout games/doom
make doom-data
make c4bb-images
```

## What a game brings

Its repository's branch `c4` carries, beside the game:

- `c4.mk`: its rules, which c4's Makefile includes when it is there:
  `NAME-objs`, its data's `NAME-data`, the gcc build `NAME-host` and
  `test-NAME`. It runs from c4's root, with c4's tools. It adds its
  objects to `GAME_OBJS`, which `make c4bb-images` builds, and its test to
  `GAME_TESTS`.
- `c4-images.sh`: its part of `src/c4bb/tests/build-images.sh`, which
  sources it twice. The first time it links the game for C4IX and puts its
  data on the disk; the second, for the bare machine and C4DOS, on the
  games disk.
- `c4ix.vfs.txt`: its block of C4IX's manifest, its own directory,
  `/usr/games/NAME`. `src/c4bb/tests/vfs-games.py` puts it in the disk's
  manifest at `#@games`, and its programs' names in `/usr/games/bin`, on
  the shell's `PATH`, at `#@game-names`, for each game on the disk: every
  `NAME.c4r` its block puts there (Wolfenstein 3D has two, `wolf3d` and
  the registered `wolf3d6`). So `wolf3d` runs from any directory.

Its tracked text files, but not `data/`, go on the disk too, at their
paths in c4 (`games/NAME/...`), so C4IX has them in `/usr/games/NAME`,
where the IDE's Code tab finds what the game's source maps name. They
are lazy files: nothing reads one before it is opened, and the web page
keeps what it has read (docs/c4ix-desktop.md, part twenty-one).

A game that is not here is not built, is not on any disk, and
`make test-NAME` says it was skipped. The bare machine's libc that the
games link there (`make libc-bare-objs`) is c4's. c4rlink drops every
function nothing reaches, so a game carries only the libc functions it
uses (docs/c4dos-design.md "Linking a bare program"); `--start-lib`
before `.libc-bare`'s objects would also leave out the data of those it
does not need at all (3 KB, libc-math's, for a game that does no math).

## Curses

`games/pdcurses` is not a game but what the terminal games stand on, as one
submodule among the others. Its `c4/pdcc4.c` draws an 80x25 grid of 8x16 cells
as 8-bit pixels, in a window of C4IX's desktop or on libjs's display (or grin's
window, under C4DOS). A Terminal can't have it: its input is edited a line at a
time, and curses wants each key. A game links `.pdcurses/lib`'s objects with
`--start-lib`, so it carries only what it calls. `games/bsdgames` builds its
curses games only when PDCurses is here.

With no display (libjs/cli.js), or with a file called `pdcscrip` in the current
directory, a curses program is scripted: keys from stdin, and each frame's changed
rows on stdout, as text and a checksum of their pixels. gcc's build is always
that. So `test-pdcurses` and `test-bsdgames` compare c4lc's build to gcc's frame by
frame, and a file called `bsdseed` makes the BSD games' clock and random numbers
repeatable. Both tests are `c4/tests/run.sh` in the game's repository.

## The BSD games

`games/bsdgames` is 24 programs in one repository: fish, wump, pig, quiz,
adventure, backgammon, trek and monop in a Terminal, and hangman, rain, worm,
gomoku, snake, canfield, robots, tetris, cribbage, mille, boggle, battlestar,
atc, sail, phantasia and hack on curses. Each is its own name on the shell's
`PATH`, at `/usr/games/bsdgames` in C4IX. On the games disk they have 8.3 names
(`adventur`, `backgamm`, `battlest`), and so does their data.

`c4/c4bsd.h` has what they want from a Unix, for both builds: anything that a game
can tell apart between gcc's build and c4lc's is the same code in each. Scores are
kept in the current directory, where it can be written. Monop can't save its game:
it does that by writing out its heap.

Phantasia and hack write files as they play: phantasia its characters, its
scoreboard and the rest of its world, and hack every level the player has left, a
lock and its scores. On their first run they make what their install would have
in the current directory, which must be one they can write, as C4IX's `/tmp` is.
The bare machine's disk and C4DOS's can't be written, so these two are only in
C4IX, and their test (`c4/tests/writes`) only runs there. Phantasia makes its
monsters' file from their text on that first run, so the file is laid out as
the build that reads it lays out its structs. Hack drew on a terminal with
termcap; here what it writes to stdout goes onto curses's screen as a terminal
would show it (`c4/hack/c4hack.c`).

Sail's computer ships are moved by a driver, which upstream is a second process
that it forks, the two keeping each other up to date through a file. Here sail
runs the driver's turn itself, every seven seconds as the driver did, with no file:
one program, so one game at a time and no other players.

Not here:

- **hunt**, a game for several players on a network: the program each player runs
  and the server they all play in, `huntd`, talk over sockets, and C4's systems
  have none. Its robot player, `otto`, plays in the same server.
- **dm**, which isn't a game: it decides whether games may be played at all, by
  the time of day, the load and who is logged in, and then starts the one asked
  for. Nothing here keeps anyone from them.

The rest of the repository wasn't looked at: dab, which is C++; arithmetic, a
drill of sums; worms, which draws and can't be played; wargames, which only starts
another game; and small programs, not games (banner, bcd, caesar, factor,
fortune, morse, number, pom, ppt, primes, random, wtf and the like).

## Data

No game's data is committed, in c4 or in the game's repository.
`make NAME-data` puts it in `games/NAME/data/` (ignored there) with
`games/fetch.sh`, which takes it from the variable the game names, or
from the URLs in its `data/MIRRORS`, and refuses a file whose SHA-256 is
not the one the game's `c4.mk` gives:

| game | file | variable |
|---|---|---|
| Wolfenstein 3D | `1wolf14.zip`, Apogee's shareware package | `WOLF3D_ZIP` |
| Doom | `doom1.wad`, id's shareware WAD (1.9) | `DOOM_WAD` |
| Rise of the Triad | `1rott13.zip`, Apogee's shareware package | `ROTT_ZIP` |
| OpenTyrian | `tyrian21.zip`, Tyrian 2.1, the user's own copy: it may not be passed on | `TYRIAN_ZIP` |

```
DOOM_WAD=~/doom1.wad make doom-data
```
