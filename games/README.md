# games

The games c4 runs -- Wolfenstein 3D, Doom, OpenTyrian and Rise of the
Triad -- are not part of c4's repository. Each lives in one of its own,
on GitHub, on upstream's history with c4's port on top as commits, and
is an optional submodule here:

| path | repository | upstream |
|---|---|---|
| `games/wolf3d` | [andrakis/c4-Wolf3D](https://github.com/andrakis/c4-Wolf3D) | Wolf4SDL, KS-Presto's fork |
| `games/doom` | [andrakis/c4-Doom](https://github.com/andrakis/c4-Doom) | doomgeneric |
| `games/tyrian` | [andrakis/c4-Tyrian](https://github.com/andrakis/c4-Tyrian) | OpenTyrian |
| `games/rott` | [andrakis/c4-ROTT](https://github.com/andrakis/c4-ROTT) | icculus.org's Rise of the Triad 1.1.2 |

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
- `c4ix.vfs.txt`: its block of C4IX's manifest, `/usr/games/NAME`.
  `src/c4bb/tests/vfs-games.py` puts it in the disk's manifest at
  `#@games`, and its programs' names beside `/bin`'s at `#@game-names`,
  for each game on the disk: every `NAME.c4r` its block puts there
  (Wolfenstein 3D has two, `wolf3d` and the registered `wolf3d6`).

A game that is not here is not built, is not on any disk, and
`make test-NAME` says it was skipped. The bare machine's libc that the
games link there (`make libc-bare-objs`) is c4's. c4rlink drops every
function nothing reaches, so a game carries only the libc functions it
uses (docs/c4dos-design.md "Linking a bare program"); `--start-lib`
before `.libc-bare`'s objects would also leave out the data of those it
does not need at all (3 KB, libc-math's, for a game that does no math).

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
