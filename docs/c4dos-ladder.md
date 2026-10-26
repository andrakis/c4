# The Build Ladder on the page

`?system=c4dos-ladder` ("C4DOS Build Ladder" in the page's System menu) is
the ladder of [climbing-the-ladder.md](climbing-the-ladder.md) in one page
session on libjs: C4DOS boots and plays games, compiles C4KE and boots
it; C4KE plays games, compiles C4IX and boots it; C4IX plays games. Nothing
reboots: each system is started by the one below it, and when it stops,
the one below carries on.

It is a disk of its own, `libjs/images/ladder`, because it carries far
more than the plain C4DOS disk: the build kit, both kernels' sources, the
toolchain, and C4IX's userland.

## Using it

At each prompt (the disk's `LADDER.TXT` says the same; `TYPE LADDER.TXT`):

| at | type | what happens |
|---|---|---|
| `A>` | `DOOM`, `WOLF3D`, `TYRIAN`, `ROTT`, `GRIN` | play; HEAP.SYS gives a game's memory back when it quits |
| `A>` | `BUILD` | C4KE and its init compiled onto the RAM disk (`LADDER` rebuilds the compiler first) |
| `A>` | `RUN dosload.c4r c4ke.c4r` | boots the kernel just compiled, its RAM filesystem seeded from the RAM disk |
| `c4sh>` | `doom`, `wolf3d`, `rps`, `raycast` | play, as C4KE tasks |
| `c4sh>` | `grin` | grin as a C4KE task; its DOS Prompt is a C4KE Prompt (below) |
| `c4sh>` | `tar x c4ix-src.tar` | C4IX's sources into the RAM filesystem |
| `c4sh>` | `b4ke -f c4ix.b4k` | twelve modules and a link: `c4ix.c4r` |
| `c4sh>` | `kboot c4ix.c4r` | C4KE shuts down, and the C4IX it built boots (`kboot c4ix.c4r --desktop` for the desktop) |
| `c4ix:/$` | `ls`, `ps`, `top`, `doom`, `desktop` | the system C4KE built |
| `c4ix:/$` | `exit` | C4IX shuts down; C4KE returns, then dosload, to `A>` |
| `c4sh>` | `exit` | C4KE shuts down, back to `A>` |

The system's defaults are 128 MB and 400 MHz (paced). Drive 1 is kept in
the browser under the store `ladder`, apart from every other system's
([grin-plan.md](grin-plan.md) 3.8): `RUN bbsave.c4r B:` at `A>`, `save B:`
in C4KE and C4IX's `persist` write there, and Reset Files forgets it.

## What each step costs

`libjs/tests/test-c4dos-ladder.mjs` on helios, 2026-10-26, unpaced (the
page at 400 MHz paces the machine's clock, so a step there takes at least
its cycles / 400 M seconds; b4ke is then about 30 s of machine time).
The heap is libjs's `HEAP_USED`: what is allocated, of the 128 MB arena.

| step | wall | cycles | heap after | heap at most |
|---|---|---|---|---|
| C4DOS boots | 0.0 s | | 4.0 MB | |
| Doom at `A>`, 40 frames, quit | 0.8 s | 157 M | 4.0 MB | 23.7 MB |
| `BUILD` (c4ke.c4r 240,228 bytes) | 1.6 s | 364 M | 5.3 MB | 12.7 MB |
| `RUN dosload.c4r c4ke.c4r` | 0.1 s | 20 M | 4.9 MB | 6.5 MB |
| Doom under C4KE, quit | 0.7 s | 136 M | 19.1 MB | 25.2 MB |
| `tar x c4ix-src.tar` | 0.1 s | 13 M | | |
| `b4ke -f c4ix.b4k` (c4ix.c4r 261,841 bytes) | 59.3 s | 12,113 M | 20.9 MB | 39.1 MB |
| `kboot c4ix.c4r` | 0.3 s | 28 M | 35.7 MB | 35.7 MB |
| Doom under C4IX, quit | 3.5 s | 636 M | 47.1 MB | 54.6 MB |
| `exit` from C4IX, through C4KE, to `A>` | 3.5 s | 795 M | 0.0 MB | 60.7 MB |

The whole run is 14.3 G cycles and 70 s. The RAM disk is 32 MB
(`CONFIG.SYS`), and the most any step held was 61 MB of the arena.

On the page (`web-c4dos-ladder.mjs`, Chrome 151 on the user's desktop,
paced at 400 MHz, 2026-10-26; each time includes typing the line over
CDP, a second or two):

| step | on the page | machine time |
|---|---|---|
| `BUILD` | 3.9 s | 3.3 s |
| `RUN dosload.c4r c4ke.c4r` | 6.0 s | 4.0 s |
| `tar x c4ix-src.tar` | 2.1 s | 2.0 s |
| `b4ke -f c4ix.b4k` | 89 s | 89 s |
| `kboot c4ix.c4r` | 1.9 s | 1.9 s |
| `exit`, C4IX to `A>` | 5.6 s | 5.4 s |

b4ke ran at about 136 M instructions a second there, below the 400 MHz
the page paces to, so it took 89 s rather than 30; the machine's clock
keeps the wall's (`?realtime`), so it says 89 too.

## How it works

### The disk

`build-images.sh` lays three disks over each other into
`libjs/images/ladder` (hard links where it can):

1. **The shared disk** (`src/c4bb/images/disk`), for C4IX's userland and
   its tree, `c4ix.vfs.txt`. So the C4IX the machine builds comes up as
   `?system=c4ix` does: the desktop, the games, the libraries.
2. **The climb floppy** (`c4dos-c4ix32`, `make c4dos-c4ix32`): the build
   kit (`dostar`, `cpp`, `c4cc`, `c4ke-src.tar`, `tools-src.tar`), `dosload`,
   C4KE's userland and its tree (`c4ke.vfs.txt`), `c4ix-src.tar`, `c4sc`,
   `c4rlink`, `b4ke`, `c4ix.b4k` and `kboot`.
3. **The page's C4DOS disk** (`libjs/images/dos`): the games and their
   data, grin, and the newest `c4dos32.c4r`.

Its own `CONFIG.SYS` (CLOCK, a 32 MB RAMDISK, CONSOLE FLUSH, DRIVES, HEAP,
REDIR TAP; no `HOME=B:`, which would send what cpp and c4cc write to B:
instead of the RAM disk C4KE is seeded from),
`AUTOEXEC.BAT`, `BUILD.BAT`, `LADDER.TXT`, `c4dos.dir`, `index.json` and
`manifest.json`. The prebuilt C4IX kernel is left out, so the one that
boots is the one b4ke linked. Like the climb disk beside it, the ladder
disk is copied from the climb floppy, not built from it: `make
c4dos-c4ix32` first, then `make c4bb-images` (build-images.sh says when
it is skipped). `make test-libjs-ladder` does both.

### C4DOS to C4KE

Unchanged from the breadboard: `BUILD.BAT` compiles the kernel, dosload
loads it, gives DOS's scratch back and jumps, and the kernel's `dos`
extension (`c4ke_dos.c`) copies the RAM disk into its RAM filesystem.
`HEAP.SYS` matters more here than anywhere: dosload runs in a heap
generation of its own, and so does everything C4KE, and C4IX after it,
allocate. When dosload returns to `A>`, DOS releases that generation and
the whole climb's memory comes back at once.

### Games under C4KE

The games are built for the bare machine and C4DOS, against libc's
`bare.c`. On the bare machine `exit()` is the EXIT opcode, which stops the
machine; a C4KE task that did that stopped all of it. So C4KE now tells
such a program it is a task, the way C4DOS writes `__c4dos_api`: before
it starts init, the kernel sets `c4r_inject_name` to `__c4ke_exit`, and
`load-c4r.c` writes the number of `OP_TASK_EXIT` into that global of
every task it loads that has one. `bare.c`'s `exit()` raises that opcode
when it is set. Files, the console, the display and the keyboard need
nothing: a C4KE task without protected mode reaches the machine's own.

C4KE does not take back what a task allocated (there is no protected mode
here to see it), so each Doom run under C4KE keeps about 14 MB until C4KE
stops, when HEAP.SYS gives it all back. That is the reason for 128 MB.

### grin under C4KE

grin is a bare-libc program like the games, so it runs as a C4KE task too.
Two things made it a good one:

- **Its naps.** grin sleeps when it has nothing to do. It used the machine's
  USLP, which on libjs stops the whole machine, every task with it. libc's
  `usleep` (`bare.c`'s `umsleep`) now uses C4KE's `OP_USER_SLEEP` when the
  program is a task (`__c4ke_exit` set), and grin naps through it.
- **Its DOS Prompt.** With no DOS under it, the window is a **C4KE Prompt**
  (`src/grin/dosprmpt.c`, `ke_run`): a line's first word is a program,
  started as a task (`OP_USER_START_C4R`, found as c4sh finds one), the
  rest its arguments. grin's loop carries on while it runs; what it prints
  comes from the console tap into the window, and a line typed in the
  window is fed to its fd 0 (Ctrl+Z ends its input). A last word `&` leaves
  it running in the background, printing to the console; `kill PID [SIGNAL]`
  is a builtin, as in C4KE's shells (SIGTERM unless told, 9 is SIGKILL);
  `EXIT` closes the window.

**Preemption.** Under C4KE the timer had almost never fired (`c4ke: 1
timer ticks` at shutdown in these runs), so a task that never slept had the
machine to itself. A background `top` or a busy `spin x` stalled the shell
and grin. libjs and c4bb hold the PIT while a trap handler runs, and C4KE's
`await_pid` blocked *inside* its handler (it called `schedule()` there):
the kernel waiting on init, init on the shell and the shell on what it ran
each left a handler open for good. `await_pid` now switches away from the
trap as `sleep` does (`SCHEDULE_IN_TRAP`), and `kernel_task_finish` puts the
exit code in the waiter's saved A. The PIT now preempts. Note that `spin`
and `top` without `-b` take C4KE's input focus when they start, so the
shell that started them gets no keys until it asks for them back.

### C4KE to C4IX: kboot

On the breadboard the way up from C4KE is a medium and a reset (`kinstall
C:`, `reboot 1`). libjs has no BIOS to reset into, so C4KE boots the next
kernel itself, as dosload does one rung down:

- `kboot` (`src/c4ke/bin/kboot.c`) raises `OP_KBOOT`, which the `boot`
  extension (`src/c4ke/extensions/c4ke_boot.c`) adds. The handler checks
  the image is there (the RAM filesystem, then the disk), keeps its
  command line, and ends init.
- C4KE then shuts down exactly as when init exits: every task is sent
  TERM, the extensions' shutdowns run, the timer stops, the trap and
  signal handlers go back to what they were.
- Last in `main`, `kernel_handoff` (declared in `c4ke.c`, set by the
  extension) loads the image, frees the RAM filesystem, and runs the
  image's constructors, `main` and destructors.
- When C4IX shuts down, its `main` returns to C4KE's, C4KE's to dosload,
  and dosload's to DOS.

C4IX needs nothing for this: it boots as it does from dosload
(`make test-c4dos-c4ix32`) and finds its userland on drive 0.

## Tests

| target | what |
|---|---|
| `make test-libjs-ladder` | `test-c4dos-ladder.mjs`, the whole climb above with 27 checks, about 90 s; also in `make test-libjs` (which skips it without the disk). Under C4KE it also runs grin, opens a C4KE Prompt, starts `spin x &` in it, checks with `ps` that the spinning task ran on while grin waited (more than 1,000 of 2,000 ms), and kills it. Under C4IX it checks `/lib` has the shared libraries and starts the desktop (which loads `/lib/libgui.c4s`) |
| `node libjs/tests/test-c4dos-ladder.mjs --quick` | the same to C4KE and back, without b4ke |
| `node libjs/tests/web-c4dos-ladder.mjs` | the page in the user's Chrome over CDP: the same climb typed into the terminal, with screenshots |

## Not done

- **b4ke builds C4IX's kernel only.** The userland it boots into is the
  shared disk's, prebuilt. C4IX's own `BUILD.B4K` files (`src/c4ix/lib`,
  `libc`, `user`) are B4KE2 scripts for C4IX's b4ke; C4KE's b4ke reads
  B4KE1, and has no `MKDIR` or `DEPS`.
- **C4KE keeps a task's memory** until it stops (above).
- C4IX's first line counts cycles from the machine's start, with `%d`:
  after b4ke's 12 G cycles it says a negative number.
- Wolfenstein 3D starts and draws under C4KE; the test plays Doom at
  every rung, not Wolf3D, whose menu it does not drive.
- `make run-c4dos-c4` (the plain-c4 tower) is reported to abort with a
  corrupted top size. Not looked at here; the ladder does not use it.
