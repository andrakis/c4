# libjs — c4m in JavaScript

A direct interpreter for the c4m virtual machine, written the way `c4m.c`'s
dispatch loop is: one `switch` over an `Int32Array`. It boots the 32-bit
C4DOS, C4KE and C4IX images, and games on the bare machine, in Node or in a
browser Web Worker, with a terminal, a display device that a guest program
draws into, and a sound card (an AdLib OPL2/OPL3, sixteen channels of 8-bit
samples, the PC speaker and a MIDI port).

It shares its memory map, device registers, `.c4r` loader, disk controller,
keyboard, clocks and mailbox with the c4bb breadboard computer
([../src/c4bb/](../src/c4bb/README.md)) by importing them. The difference is
the CPU: c4bb runs microcode and keeps malloc and printf in guest firmware;
libjs runs opcodes, and MALC/FREE/RALC, PRTF and STRC happen on the host
side, as they do in native c4m.

## Build

Nothing to build in libjs itself. The images it runs are the ones c4bb
builds:

    make c4bb-images

## Run

From the repo root:

    node libjs/cli.js [-m MB] [-c cycles] [-s] [-d dir] [-w dir] [-i] [-hz MHz] [--fast] [--mbox BYTES] [--wav FILE] [--mmu] program.c4r [args...]

| Flag | Meaning |
|---|---|
| `-m MB` | Arena size (default 32). |
| `-c cycles` | Cycle budget (default unlimited); running out exits with status 9. |
| `-s` | Print machine stats to stderr on exit. |
| `-d dir` | A read-only drive: every file under `dir`, by relative path. |
| `-w dir` | A writable drive: files the guest writes land in `dir`. |
| `-i` | Interactive: stdin streams to the keyboard. Without it, piped stdin is fed up front with end-of-file set. |
| `-hz MHz` | How fast the machine claims to be (default 20). |
| `--fast` | Interactive runs go as fast as the host can instead of pacing to the clock. |
| `--mbox N` | Fit a mailbox of N bytes (c4bb's `MBOX_*` registers). |
| `--wav FILE` | Fit a sound card and write what it played to FILE, 44.1 kHz 16-bit stereo, on the machine's own clock, so the same session makes the same file. Without it there is no card and programs run silent. |
| `--midi FILE` | Give the sound card a General MIDI synth on its MIDI port, so a game plays its music there, and write each message with its time (microseconds, then the bytes in hex) to FILE. |
| `--sf2 FILE` | ... and have `--wav` play the messages with this SoundFont (`snd-gm.js`). |
| `--mmu` | Fit an MMU (docs/c4ix-mmu.md): C4IX turns it on and gives each task an address space of its own. Off by default, for its cost; the tests fit one. |
| `--cpus N` | N processors, simulated (`smp.js`): each runs `-q` instructions in turn (default 1000). |
| `-q N` | Instructions a CPU runs before the next one's turn. |
| `--parallel` | ... each CPU on a thread of its own (`par.js`). |
| `--no-accel` | No accelerators (`accel.js`): a game draws with its own loops. |
| `--no-osaccel` | Nor C4IX's own (`src/c4ix/accel.mjs`): its gunzip, tar and boot unpack the root archive in its own C. |
| `--no-fpu` | No FPU opcode: floats in software. |

The flags both runners have mean the same in each, so a test can swap one
runner for the other. Examples:

    node libjs/cli.js src/c4bb/images/hello32.c4r
    node libjs/cli.js -i -d src/c4bb/images/disk src/c4bb/images/c4ix32.c4r
    printf 'ps\nexit\n' | node libjs/cli.js -d src/c4bb/images/disk src/c4bb/images/c4ix32.c4r

In a browser:

    make serve-libjs       # node libjs/serve.mjs 8472 --isolate

then open http://localhost:8472/libjs/web/. The server serves the repo root,
because the Worker imports `src/c4bb/sim/` and the page loads images from
`src/c4bb/images/`. `--isolate` adds the COOP/COEP headers, which make the
page cross-origin isolated so the display can use shared memory
(`shared.js`); without them it falls back to posting messages. The page
has the sound card unless `?snd=0`; a browser lets it play only after the
first key or click.
`LIBJS_PORT=NNNN` moves it.

On GitHub Pages, at https://andrakis.github.io/c4/libjs/web/: `make pages`
(`pages.sh`) builds the images in a tree of its own (`~/git/c4-pages`)
with every game that may be published: all but Super Blit Kart (the
licence on its art is not settled), Quake's shareware as its zip
(`QUAKE_AS_ZIP=1`). Tyrian is there as OpenTyrian, the engine, without
Tyrian 2.1's data, which may not be passed on: a visitor adds their own
`tyrian21.zip` with the page's Uploads, which takes it apart into
`/home/tyrian` (`uploads.js`), and plays it as Tyrian (your files) in
C4IX's Go > Games, Full Games (Uploads) > Tyrian in grin, or
`?system=tyrian-full`. `pages.sh` refuses to run with `TYRIAN_ZIP` set
or with anything in the tree's `games/tyrian/data` but its README, and
`site.sh` refuses a site with any of Tyrian 2.1's files in it, or in a
zip, by the SHA-256s in `tyrian21.sha256`. `site.sh` lays the site out as the repo is,
with the images in it, in the site repository's `c4/`
(`~/git/andrakis.github.io`), and `pages.sh` commits it there. Pushing
that repository publishes it. Pages
cannot send COOP/COEP headers, so `coi.js` does. It is a service worker
for `libjs/` that adds them to every response, and the page registers it
and reloads once on the first visit. A server that sends them, such as
`serve.mjs --isolate`, leaves it nothing to do.

URL parameters (from `web/app.js`). The page's Configuration button shows them as a form, and OK reloads the page with the ones changed. Its Machine tab has the system, speed, processors (and a thread each), the MMU, the accelerators, C4IX's own accelerators and memory, with presets: Single Core, and Dual, Quad and Octo CPU (2, 4 and 8 processors, a thread each). The Other tab has the rest:

| Parameter | Meaning |
|---|---|
| `?system=c4ix\|c4ke\|c4dos\|c4dos-ladder\|wolf3d\|wolf3d6\|wolf3d-c4ix\|doom\|doom-full\|tyrian\|rott\|rottreg\|quake\|quake-full\|duke3d\|duke3d-full\|sw\|sw-full\|gui-demo\|fb-demo\|snd-demo\|raycast` | Which machine to boot (default `c4ix`). `c4dos` is C4DOS on a disk of its own with every game, booting into grin, its graphical shell (docs/grin.md; 256 MB, for the games that read their files whole, and the full games from the Uploads); `c4dos-ladder` is the Build Ladder, C4DOS on a disk of its own that also builds and boots C4KE, from which C4KE builds and boots C4IX, all in one page session (docs/c4dos-ladder.md; 128 MB, and its own store for drive 1); `wolf3d`, `doom`, `tyrian`, `rott`, `quake`, `duke3d` and `sw` are those games on the bare machine, `wolf3d6` Wolfenstein 3D's registered game there, from its files in Uploads (below), `doom-full` (or `doom1-full`, `doom2-full`, `tnt-full`, `plutonia-full` for one IWAD of several), `quake-full`, `duke3d-full`, `sw-full` and `rottreg` the others that play the person's own files (the Uploads' folders, below), and `wolf3d-c4ix` Wolfenstein 3D on C4IX's whole display (docs/c4ix-wolf3d.md, games/doom/README.md, games/tyrian/README.md, games/rott/README.md, games/quake/README.c4.md, games/duke3d/README.c4.md, games/sw/README.c4.md; Tyrian's data only where it was there to copy). The last four are bare programs on the display or the sound card. |
| `?image=URL&disk=URL` | Any other image and disk. |
| `?args=A+B` | More arguments for the system's program, after its own: `?system=wolf3d&args=--secrets+--nodeath` (games/wolf3d/README.md). |
| `?uploads=0` | The Uploads are not put on the disk. |
| `?persist=0\|reset` | The system's saved files are not kept between visits / are forgotten first. |
| `?music=gm\|midi` | The games' music in General MIDI instead of on the AdLib (the default), as DOSBox's General MIDI is: Doom, Duke Nukem 3D and Rise of the Triad. `gm` plays it with a SoundFont from the Uploads, `?soundfont=NAME` naming which (else the first in `/home/soundfonts`, else the first `.sf2`, `.sf3` or `.dls`); `midi` sends it to the computer's own synth by Web MIDI (on Windows the Microsoft GS Wavetable Synth; `?midiout=NAME` picks the output), and the browser asks first. Configuration's Music and SoundFont set them. Without a SoundFont, or without Web MIDI, it is the AdLib, and the page says why beside the status. |
| `?mhz=N` | Paced speed (default the system's own: 100, or 400 for C4DOS, the ladder and the games). |
| `?fast` | Unpaced. |
| `?realtime=0` | When the host cannot keep up with `?mhz`, the machine's clock slows with it. By default it keeps the wall's time (`worker.js`), so a game drops frames rather than playing in slow motion. |
| `?mem=MB` | Arena size (default the system's own: 64, or C4DOS's 256, the ladder's 128, `quake` and `doom-full` 128, `duke3d-full` and `sw-full` 192, `sw` 128, `quake-full` 256). |
| `?snd=0` | No sound card. |
| `?cpus=N` | N processors, simulated (`smp.js`, docs/c4ix-smp.md). |
| `?parallel` | ... each on a worker of its own (`par.js`), when the page is cross-origin isolated. |
| `?mmu=1\|0` | Fit an MMU or not (`mmu.js`, docs/c4ix-mmu.md): C4IX turns it on. Off unless `?mmu=1`, for its cost (`web/site.js` can make it the default). |
| `?fpu=0` | No FPU opcode: floats in software. |
| `?accel=0` | No accelerators (docs/accel.md). |
| `?osaccel=0` | Not C4IX's own (its gzip and CRC-32). |
| `?shared=0` | Use the message path even when isolated. |
| `?coi=0` | No service worker isolating the page (`coi.js`): unregisters it. |

**Uploads** are files of the person's own: added with the Uploads button,
or dropped on the page, kept in this browser (IndexedDB, as C4IX's
drive 1 is) and laid over drive 0 at every boot (`page.js`'s
`opts.uploads`, `worker.js`), so a program opens them by name as if the
disk had them; on C4IX that is a file its own tree does not have
(`sys_open`). Under the page's C4DOS (`?system=c4dos`, `uploadsHome` in
`app.js`) they are laid over drive 1 instead, B:, its home drive, and A:
keeps only the system (below). Names are made lower case. Boot with them
reloads the page.

Each upload goes in a **folder**, picked from the dialog's list before the
files are added (or dropped): `/home`, as before, or one for each game that
plays the person's own files (`FOLDERS` in `uploads.js`). The list shows
each file's folder, and Forget takes it out of that folder alone. In the
store an upload in a folder is named `FOLDER/NAME`, and `uploads.js`
(`layUploads`) puts it where each system finds it: on drive 0 as
`FOLDER/NAME` (the bare games open that name), and in C4IX as
`/home/FOLDER/NAME`; under the page's C4DOS on B: as `B:\FOLDER\NAME`
(`HomeFiles`), and under the Build Ladder's, which has no home drive, on
A: as `A:\FOLDER\NAME`. Each game looks
in its own folder first, then in `/home`, so uploads from before there
were folders still play:

| folder | the game, and what it plays | Go > Games (C4IX) | `?system=` (bare machine) |
|---|---|---|---|
| `/home/wolf3d` | `wolf3d6`: Wolfenstein 3D's registered `*.WL6` (games/wolf3d/README.md) | Wolfenstein 3D (your files) | `wolf3d6` |
| `/home/duke3d` | `duke3d -home`: `DUKE3D.GRP` (games/duke3d/README.c4.md) | Duke Nukem 3D (your files) | `duke3d-full` |
| `/home/sw` | `sw -home`: `SW.GRP`, the registered game (games/sw/README.c4.md) | Shadow Warrior (your files) | `sw-full` |
| `/home/quake` | `quake -home`: `pak0.pak` and `pak1.pak`, the registered game (games/quake/README.c4.md) | Quake (your files) | `quake-full` (256 MB) |
| `/home/doom` | `doom -home`: `doom.wad`, `doom2.wad`, `plutonia.wad`, `tnt.wad` (games/doom/README.md) | Doom (your files) > each IWAD | `doom-full`, or `doom1-full`, `doom2-full`, `tnt-full`, `plutonia-full` (128 MB) |
| `/home/rott` | `rottreg`: `DARKWAR.WAD`, `.RTL`, `.RTC`, and `REMOTE1.RTS` if you have it (games/rott/README.md) | Rise of the Triad (your files) | `rottreg` |

On the bare machine a game looks in `FOLDER/` at drive 0's root, then at
the root itself; not under `/home`, which the disk would answer by a
path's last part, from the root (devices.js `diskFind`). Under C4DOS the
same names, which have no drive, are looked for in the home drive's
current directory first (`c4dos.c`, `dos_open`): `B:\FOLDER`, then `B:\`.

B: under C4DOS is what the page keeps of it (the `c4dos` store) with the
uploads over it (`HomeFiles` in `uploads.js`): DIR and grin's File Manager
list both, since B:'s listings (`c4dos.dir`) are the person's with the
uploads added. An upload hides a file of the person's of the same name
while it is uploaded, and DEL cannot take one off (devices.js
`diskUnlink`, `locked`): Forget in the Uploads dialog does, from the next
boot. Nothing of an upload is ever kept in the store -- a listing C4DOS
writes back, for a file saved there, is kept without the uploads' names
-- so a forgotten one leaves nothing behind. With `?persist=0` B: is there
for the uploads alone, kept for that boot.
Under C4DOS, grin's Program Manager has the same games in its Full Games
(Uploads) group (docs/grin.md 3.7), or at `A>`: `\GAMES\WOLF3D\WOLF3D6`,
`\GAMES\DUKE3D\DUKE3D -home`, `\GAMES\SW\SW -home`, `\GAMES\QUAKE\QUAKE -home`,
`\GAMES\DOOM\DOOM -home` (`-iwad NAME` for one of several) and
`\GAMES\ROTT\ROTTREG`; and README.TXT there says how to add each game's
files (`A:\GAMES\README.TXT`, written from `NEEDS` at build time by
`libjs/uploads-readme.mjs`). The dialog lists what each game wants
(`NEEDS` in `uploads.js`): each of its files, found among the uploads or
not, and where to start it on each system.

**Downloads.** What a system keeps can be taken away as files, two ways:

- **Saved Files** (the page's button, beside Reset Files, wherever the
  system has a store) lists the store -- drive 1, kept in this browser --
  without the machine, and downloads a file, a folder as a tar, or all of
  it as `STORE.tar`. Every system's: the bare games' saves and settings
  by their names, C4DOS's B:, and C4IX's changes by their paths, from
  the tar persist last wrote (`kept.js`; `c4ix-persist.cur` says which of
  its two). For C4IX that is the files as the last save kept them: the
  desktop saves a minute after a change, and at Shut Down.
- **In C4IX**, `download PATH...` in a shell, or **Files' File >
  Download**, hands a file over as it is now, and a folder as `NAME.tar`
  (`download` is persist by another name: `src/c4ix/user/persist.c`). It
  writes `download:NAME` through the drive's write registers, which
  `devices.js` gives to `onDownload` rather than to a drive; `worker.js`
  posts it to the page (`download`), and the page saves it. On the
  command line nothing takes it, and `download` says so.

The folder tar is ustar, as C4IX's persist writes, read by any tar.

A SoundFont for General MIDI music (`?music=gm`) is an upload too. None
ships with c4: the free ones are tens of megabytes. GeneralUser GS
(https://schristiancollins.com/generaluser.php, free to use and to pass
on) is a good one.

The terminal pastes and copies as a terminal does, on every system:
- **Paste**: Ctrl+Shift+V, Shift+Insert, Ctrl+V while no program has the
  keyboard raw (a full-screen program gets its Ctrl+V), the right-click
  menu's Paste, or the Paste button. The text goes in as typed keys, a
  line at a time: the next line waits until the machine has read the last
  one and asks the keyboard for more (`keys.js`'s `Paste`, fed by the
  worker), so a list of commands runs as if typed at each prompt, and a
  long paste waits its turn. It is made ASCII first: CRLF is Enter,
  typographic quotes and dashes are plain, accents come off, other
  characters are `?`. Ctrl-C drops what has not gone in yet. Where the
  browser will not let the page read the clipboard (the menu and the
  button need its permission; the keys do not), a box opens to paste into.
- **Copy**: select text, then Ctrl+Shift+C, Ctrl+Insert, Ctrl+C (with
  nothing selected Ctrl+C is still SIGINT) or the menu's Copy.
- **The cursor** is drawn where the guest's is, as an underline that blinks
  while the terminal has the focus. A program that hides it (`ESC[?25l`)
  hides it here too.

The running machine is `window.c4m` on the page, so the console or a test
can drive it (`c4m.write('ps\n')`, or `c4m.paste('ps\nls\n')` a line at a
time).

## The Code Explorer

`/libjs/web/explore.html`, from the same server: any image, object or
library in the repository (the list is every one under
`src/c4bb/images`; a path, or a file dropped on the page or opened from
this computer, does as well).

- **Functions, globals and strings**, filtered by a regex. A body with
  no symbol (a static, a helper the compiler made) is listed by where it
  starts, `<code@0x…>`. A string is any the code takes the address of.
- **The code** of the one chosen, with every operand named: calls and
  `&function` link to the function, branches to labels, data to the
  global (and offset) or the string literal, `[bp+n]` to the parameter
  when the source is found, and anything still to link (in a `.c4o` or
  `.c4l`) by its symbol. Fused opcodes are green, the machine's
  syscalls pink.
- **What an instruction does**, on hovering its mnemonic, as Compiler
  Explorer explains an x86 one (`web/opdoc.js`). The tooltip names its
  operand, if it has one, and says whether memory is read or written
  through a pointer. A fused opcode is explained as the instructions it
  stands for. `tests/test-opdoc.mjs` checks every opcode against the
  machine's table.
- **Its source, line by line**, when the image has a source map (c4lc
  `-g`; docs/c4lc-design.md 15). The rows say which file and line each
  run of code came from:
  - the code pane heads each run with its line, as objdump `-S` does,
    and colours it;
  - the source pane marks every line that has code;
  - clicking a line marks its instructions, in this function or the one
    they are in, and clicking an instruction marks its line;
  - the file comes from the server by the path the compiler was given,
    and is checked against the map's size and CRC, so a source changed
    since is said to be.
- **Its source, by name**, when there is no map: each `.c` in the
  directories named (guessed from the image; the guess is kept per
  image) is scanned for function definitions, and the function's lines
  are marked. Names in the source that are functions in the image link
  back to them.
- **Refs**: who calls it, or takes its address, and from where; what it
  calls; the globals it uses; its instructions by opcode.
- **Data**: a global's bytes, the pointers in it (patched at load), and
  who uses it.
- **Go to** a name, a code word (`0x1f4`), or a machine address
  (`@0x1a3f0`, against the image's base: 0x1000 on the bare machine).
  The URL keeps the image and function (`#img=…&fn=…`), and Alt+Left and
  Alt+Right go back and forward.

Only c4lc and c4sc write maps (so C4IX's `cc -g` does too), and only
with `-g`. c4rlink merges the objects' maps into the program's (S2), and
`-s` drops them with the symbols.

## Test

None is in `make test`; `make suite` runs `test-libjs` and `test-libjs-ladder`.

| Target | What it checks |
|---|---|
| `make test-libjs` | Unit tests (printf, heap, display, shared memory, desktop, the explorer's `c4r.js` over every image), then every corpus image byte for byte against native `c4m32` (or 64-bit `c4m` where `c4m32` cannot be trusted), the fused-opcode differential, STRC with symbol names, exit status, the PIT, and C4KE and C4IX booting with grep assertions. Builds `c4bb-images` first. |
| `make test-libjs-ladder` | The Build Ladder headless (`tests/test-c4dos-ladder.mjs`, docs/c4dos-ladder.md): Doom at each rung, BUILD, dosload, b4ke, kboot, and back to `A:\>`, about a minute and a half (99-103 s on helios, 2026-10-31). `make c4bb-images`, which it runs first, makes the climb floppy and the ladder disk; `make test-libjs` runs it too when the disk is there. `tests/web-c4dos-ladder.mjs` is its browser check. |
| `make test-libjs-web` | The page in the user's own Chrome over raw CDP (`LIBJS_CDP`, else ports 9224 then 9222), then the display legs with `--gui`. It never launches a browser; if none answers it says so. |
| `make test-quick` | The quick tier (`tests/test-quick.sh`): only the parts of `test-libjs` that the paths changed since `c4ix_and_toppings` can reach (`QUICK_BASE=` another branch), `test-c4ix` if C4IX changed, `test-c4ix-rebuild` if b4ke, cc, a `BUILD.B4K` or a compiler did. A desktop-only change runs the desktop part alone; docs alone run nothing. `QUICK_DRY=1` says what it would run. For the edits before a merge: the merge still has the full gate. |

### The gate's time

The full gate is `make c4bb-images test-c4ix test-c4ix-rebuild
test-libjs`. `test-libjs.sh` is in parts, a function each that depends on
no other: `PARTS="desktop grin"` runs only those, and `JOBS` sets how many
run at once (2 by default; `JOBS=1` runs them in turn and prints as it
goes). It ends with each part's time. `test-rebuild.sh` builds on two
processors with `b4ke -j 2` (`REBUILD_CPUS=1` for one).

On helios (six cores, 3 GB), with the images already built, before
(2026-10-30, every suite in turn) and after (2026-10-31). A part's time
after is its own, run beside another:

| | Before | After |
|---|---:|---:|
| `test-libjs` part `grin` (test-grin.mjs) | 1262 s | 69 s |
| `swap`: test-c4ix-swap.mjs, at `CPUS=2`, test-c4ix-par.mjs | 13 + 213 + 12 s | 46 s |
| `desktop`: test-desktop.mjs, test-boot-desktop.mjs | 139 s | 115 s |
| `ladder` (test-c4dos-ladder.mjs) | 99 s | 103 s |
| `c4ix`: zombie, cpu, sigh, mmu, guard | 90 s | 91 s |
| `c4m` (test-c4ix-c4m.mjs) | 74 s | 76 s |
| `ixbuild`: b4ke and cc inside C4IX | 68 s | 71 s |
| the other ten parts | 94 s | 93 s |
| **`test-libjs`** | **2063 s** | **332 s** (664 s of parts, two at a time) |
| **`test-c4ix-rebuild`** | **723 s** | **442 s** |
| `c4bb-images` (up to date), `test-c4ix` | | 47 + 23 s |
| **the gate** | **about 47 min** | **15 min** |

The same 823 checks pass either way; memory in use on the whole machine
peaked at 2.3 GB of 3.4.

What it was, and why:

- `test-grin.mjs` spent twenty of its twenty-one minutes in its own frame
  reader, not in the machine. While Doom's timedemo played under the
  maximised Program Manager, grin's redraws under a clip each drew the
  window's whole frame again, which the reader kept, as a clip leaves the
  new rectangle covering nothing: a quarter of a million by the end, each
  filtered at every rectangle drawn. A rectangle drawn again at the same
  place and size now stands for the old one. The machine ran the same
  10,985 M cycles and 2134 frames either way.
- The `CPUS=2` swap test's spinning holder spun for the 300 machine-seconds
  it was given (3 x 10^10 cycles at 100 MHz), where one CPU's holders
  sleep them away for nothing. The holders now stop when the test makes
  `/tmp/swapgo`, once it has looked at the pages that are out.
- The rebuild runs two compiles at once on two processors.
- `JOBS=2`: two parts at a time. A part peaks at 330 MB (grin; the
  desktop 280 MB, the ladder 200 MB), so two fit helios beside its other
  services.
- The ladder (b4ke building C4IX inside C4KE is 75 s of its 97), the
  desktop and `test-c4ix-c4m` (cc compiling C4KE's kernel inside C4IX,
  31 s) were looked at too: their time is the work they check.

### Harnesses for looking

`tests/try-*.mjs` boot C4IX headless, as the tests do, and print what a
question needs rather than pass or fail. They are for chasing a hang, a
kill or the memory a run leaves behind, and are in no suite. Each says at
its top what it runs and which environment variables change it (`MB`,
`CPUS`, `N`, `IMAGE`, …). Some read the kernel's `struct task` and MMU
regions by word offset, as `c4ix.h` has them; those say so.

| harness | what |
|---|---|
| `try-inner.mjs` | the shell runs `CMDS` (split at `\|`), each with its machine time; `PIN`, `HEAP=1`, `LIVE=1` |
| `try-bench.mjs` | `/bin/bench` on `CPUS` CPUs, the MMU on or off |
| `try-free.mjs` | `free` and `/proc/meminfo` beside the JS heap's own numbers |
| `try-tiny.mjs` | the desktop's boot in machines of a few MB (`SIZES`) |
| `try-hog.mjs` | one program bigger than memory, walked: machine time and swap |
| `try-build.mjs`, `try-stall.mjs` | the in-C4IX build, with `free` before and after, or `ps` and `free` as it goes |
| `try-hang.mjs`, `try50.mjs` | the build or `innerbench -Tln N` until its output stops; then each CPU's place, the tasks and the kernel's log |
| `try-fault.mjs` | the build until a task is killed; then the kernel's books on that address |
| `try-heapwho.mjs` | which kernel call chain allocated each heap block left after `innerbench` |
| `try-par.mjs`, `try-parw.mjs` | `innerbench -Tln N` on real threads (`NS` for several N), or with the swap and scheduler counters as it goes |
| `try-osprof.mjs` | where C4IX and its desktop spend their instructions, by scenario (docs/accel.md); `MMU=1` gives totals only |
| `try-quake3.mjs` | three windowed Quakes at once in the desktop's Terminal, with a sound card |
| `try-scroll.mjs` | what a turn of the wheel costs in the IDE and Text Editor, and whether it reaches both ends |
| `repro-save.mjs` | `persist save` while other tasks read big files, `ROUNDS` times |
| `heapdump.mjs` | `heapDump(vm)`, the heap's map, for the others |

## Layout

| File | What it is |
|---|---|
| `c4m.js` | `Machine`, the interpreter: registers, the dispatch loop, traps, syscalls. No DOM, no Node. |
| `bus.js` | Addresses below 0x1000: 0x100–0x1FF go to c4bb's `Devices`, 0x400–0x47F to the display, 0x480–0x4FF to the sound card. |
| `heap.js` | The host-side allocator behind MALC/FREE/RALC, with a size side table as native c4m keeps. |
| `smp.js` | Several processors, simulated: c4mp's instruction set, each CPU taking turns. |
| `par.js`, `ap.js` | Several processors for real: a thread per CPU sharing the arena; `ap.js` is one CPU other than 0. |
| `sheap.js` | `heap.js`'s heap with its books in shared memory, for the parallel engine. |
| `mmu.js` | Paged virtual memory with protection, for C4IX (docs/c4ix-mmu.md). |
| `printf.js` | PRTF: glibc's format rules over 32-bit words. |
| `accel.js` | The accelerators' loader and kit: each game that ships `games/NAME/c4/accel.mjs` (the build lists them in `src/c4bb/images/accel.json`) has its loops run by the host, opcodes 0x1000-0x1fff a program finds by name through OPCD once INFO says `C4I_ACCEL` (0x100000) (docs/accel.md). `--no-accel` turns them off. C4IX's own (`src/c4ix/accel.mjs`: libarc's CRC-32 and inflater) load the same way; `--no-osaccel` turns those off alone. |
| `boot.js` | Builds the machine, loads an image with c4bb's loader, and starts `main` through a c4mp-style trampoline. |
| `gui-device.js` | The display registers and its two rings (c4bb's mailbox format), host side. |
| `shared.js` | The display's SharedArrayBuffer path, used when the page is cross-origin isolated. |
| `snd-device.js` | The sound card's registers: each write becomes an event with a time on it. |
| `snd-mix.js`, `snd-worklet.js` | The mixer that turns those events into samples, in the page's AudioWorklet or for `cli.js --wav`. |
| `snd-gm.js` | The General MIDI synth on the card's MIDI port: SpessaSynth (`vendor/`, Apache-2.0) playing the person's SoundFont inside the mixer. |
| `vendor/` | SpessaSynth's core and the two small libraries it needs, as published ([vendor/README.md](vendor/README.md)). |
| `opl.js`, `opl-tables.js` | The AdLib's OPL2/OPL3: DOSBox's dbopl, ported line by line (GPL v2 or later). |
| `keys.js` | What a keypress means to the machine, cooked or raw. Copied from `src/c4bb/web/app.js`, which cannot be imported. |
| `worker.js` | The Web Worker: arena, devices, machine, run loop, messages. |
| `page.js` | The main-thread API, `C4M.boot(...)`. |
| `lazy.js` | A disk read on demand, and the page's cache of what it read. |
| `unpack.js` | A file a disk carries as the package it came in (`NAME.unpack` beside it), made when the machine first opens it: Quake's `pak0.pak` from `quake106.zip` (`QUAKE_AS_ZIP=1`, games/quake/data/README.md). |
| `lh5.js`, `sha256.js` | For it: an LHA `-lh5-` member decoded, and SHA-256, both synchronous. |
| `uploads.js` | The Uploads: their folders, what each game needs, and where each system finds them (`layUploads`, `HomeFiles`). |
| `uploads-readme.mjs` | Writes `GAMES\README.TXT` on the C4DOS disks from `NEEDS`. |
| `kept.js` | Saved Files: a store's files as the person sees them, for listing and download. |
| `display.js` | Replays command frames onto the canvas; turns DOM events into event frames. |
| `cli.js` | The Node runner. |
| `c4r.js` | The Code Explorer's reader: a C4R image, object or library at either word size, every segment and symbol, decoded with the machine's own opcode table, operands resolved, cross-references both ways. |
| `serve.mjs` | A static server for the repo root, with optional COOP/COEP headers, and a directory's listing as JSON with `?ls`. |
| `coi.js` | The COOP/COEP headers from a service worker, where the server cannot send them (GitHub Pages). |
| `site.sh` | The page, the sources and the images as a static site, for GitHub Pages. |
| `pages.sh` | `make pages`: builds the site and commits it to the GitHub Pages repository. |
| `hostlock.mjs` | On a host that asks for it, runs only while holding a build slot. |
| `web/` | The page: terminal on the left, display on the right. `explore.html`, the Code Explorer. |
| `images/` | The page's own C4DOS and ladder disks, made by `make c4bb-images`. |
| [guest/](guest/README.md) | `gui.h` and `snd.h`, the display and sound card APIs for guest C, and their demos. |
| `tests/` | `test-libjs.sh`, the unit tests, the CDP gate (`cdp.mjs`, `test-web.mjs`, `test-web-gui.mjs`), `src/strc.c`, and the `try-*.mjs` harnesses (Test, above). |
| `package.json` | Marks the directory as ES modules. |

Reused from c4bb: `src/c4bb/sim/arena.js`, `devices.js`, `loader.js`,
`mbox.js`, `fpu.js`, and `src/c4bb/web/panels.js` (`Terminal`). libjs imports these
and never edits them.

## Hosts

libjs is itself the host: Node for `cli.js`, a Web Worker for the page. It
runs 32-bit `.c4r` images only; the 64-bit images at the repo root do not
load. The display demos (`gui-demo`, `fb-demo`) print "not fitted" and exit
on native c4m and c4bb, where 0x400 is ordinary memory; that is how
`test-libjs` compares them against `c4m32` (the CLI fits no display).

## Limits and known issues

- 32-bit only.
- c4mp's processor opcodes 66–75 run when the machine has CPUs (`--cpus N`,
  `--parallel`; `smp.js`, `par.js`); its `LXI`, `SXI` and `TRAW` (76–78)
  are named but not implemented. All ten fused opcodes (79–88) are.
- C4IX's vfsload manifest race carries over from c4bb, so the C4IX leg pins
  "most entries loaded", not all.

## Docs

[docs/libjs-design.md](../docs/libjs-design.md): decisions, the interpreter,
the Worker protocol, the display device, and the milestone tracker.
