# C4DOS — design notes

A single-tasking, trap-free disk operating system for the C4 family,
in the spirit of classic DOS: `CONFIG.SYS`, `AUTOEXEC.BAT`, a prompt,
transient programs that load, run, and return. Written as the bottom
rung of the ladder (C4DOS → C4KE → C4IX, each built by the one before:
the OS you can run BEFORE building trap machinery), but a first-class
citizen of this repo like every other subsystem.

STATUS (2026-09-30): working, and the ladder it was written for is
climbed. C4DOS boots on native c4m, under unmodified `c4` (clockless
build) and on c4bb; its build floppy compiles C4KE in-machine (`BUILD`,
`LADDER`), `dosload` hands the machine over, `install` writes a bootable
medium, and `make test-c4bb-climb` pins three power-ons, each booting a
system the one before wrote (docs/climbing-the-ladder.md). Written as the
contract on 2026-08-21; deviations have been written back here since.
API v4 (2026-10-24) made RUN re-entrant and added `HEAP.SYS`, for grin
(`docs/grin-plan.md` G0). API v7 (2026-10-26) gave the disks directories
("Directories", below), and the page's disks are laid out in them.

## Why it exists

c4lm ("CALM") attempted a microkernel on plain c4 and is on hold with a
root-caused context-switch bug (see internals.md Part 6 — `LEV`
re-derives `sp` from the *outgoing* `bp`). C4DOS deliberately wants
none of that: ONE task, no scheduler, no preemption. Everything it
needs is either plain-c4 or ordinary c4m microcode on c4bb:

- `OPEN/READ/CLOS` — the disk (read side).
- `PRTF/PUTC/PUTS` — the console.
- `MALC/FREE` — the one shared heap (c4bb firmware allocator; no
  preemption means its non-atomicity is irrelevant here).
- `TIME/C4CY` — real microcode on c4bb, NOT jsops. The clock C4 never
  had; gated behind `DEVICE=CLOCK.SYS` so a clockless build stays
  pure-c4 runnable.
- NO `ITH/C4CF/SIGH/SIGI/_TRP/DBG` — the jsop set stays untouched.

## The pieces

```
src/c4dos/
  cpp.c              # THE PREPROCESSOR (standalone pass; see below)
  c4dos.c            # the DOS: CONFIG.SYS, AUTOEXEC.BAT, prompt, builtins,
                     #   RAM disk, API table, and the .c4r loader
                     #   (c4ix/loader.c structure: v3 MEMSZ) + c4l.c
                     #   invoke-stub call mechanism (plain-c4 safe)
  dostar.c           # unpack a C4TAR1 archive onto the RAM disk
  dosload.c          # LOADLIN for C4DOS (below)
  bbsave.c           # write RAM-disk files to a c4bb drive
  install.c          # write a bootable medium from a manifest (install.lst)
  fs/CONFIG.SYS      # curated boot config, copied onto the disk
  fs/AUTOEXEC.BAT
  fs/BUILD.BAT, LADDER.BAT, IX.BAT, ...   # the in-machine builds
  tests/test-c4dos.sh
  tests/test-cpp.sh
include/c4dos.h      # transient-side API shim: exit(), dos_* services
include/c4dos_native.h  # its stub for host builds
src/c4bb/tests/build-images.sh   # also builds c4dos32.c4r into src/c4bb/images/
docs/c4dos-design.md # this file
```

(The loader was planned as a separate `c4dos_load.c`; it lives in
`c4dos.c`.)

## The preprocessor (cpp.c)

c4cc has NO preprocessor — `#` lines are skipped by its lexer; builds
lean on `gcc -E`, and c4lc's L9 is a Lisp program (far too slow under
nested interpretation). The self-hosting ladder (C4DOS builds C4KE,
C4KE builds C4IX, on the machine) therefore needs an in-machine cpp.

Decision: a STANDALONE pass, DOS-era style — `CPP FOO.C > FOO.I`,
then `C4CC FOO.I`. Batch files drive the pipeline; no compiler
integration required; native builds can use the same binary in place
of `gcc -E`.

- Dialect: the STRICT c4 subset (c4l.c's rules — no switch/break/
  for/struct, locals at top, single-word globals), so the full tower
  holds: `./c4 cpp.c file.c` works on the original interpreter.
- Features (scoped by a survey of the actual tree — nothing in
  include/ or the kernels uses more): `#include` (quoted + angle, -I
  paths), `#define` object-like and function-like (parameter
  substitution, recursive rescan with self-reference guard), `#undef`,
  `#ifdef/#ifndef/#if/#elif/#else/#endif` with a constant-expression
  evaluator (numbers, identifiers→macros→0, `defined()`, ! ~ - * / %
  + - << >> comparisons & ^ | && ||), `-D NAME[=VAL]` predefines.
  NOT implemented (unused in tree, error loudly): `##`, `#`
  stringize, backslash continuations, `#include_next`, variadic
  macros, `#pragma`.
- Comments are stripped from macro BODIES (a `//` in a body would eat
  the rest of every expansion site) and passed through everywhere
  else; expansion never fires inside strings, chars, or comments.
- Output: stdout (like `gcc -E`). Line markers: none (`-P` style);
  the verification pin is on COMPILED IMAGES, not preprocessed text.
- Verification: for each corpus file, `cpp | c4cc` vs
  `gcc -E -P | c4cc` must produce byte-identical .c4r images (the
  same pin style c4lc's L9 used).

## Program loading and return

The loader is `src/c4ix/loader.c`'s parser (v3 MEMSZ-aware — c4l.c is
NOT; it predates BSS) + `c4l.c`'s invoke stub for the actual call, so
DOS itself stays plain-c4 clean while running any image the MACHINE
can execute (on c4bb that includes JSRI/JMPA programs; DOS only scans
and refuses when the underlying machine is plain c4).

**`exit()` is the hard part**: the `EXIT` opcode HALTS the machine on
c4bb (POWER latch) and ends the VM natively; C4KE survives it only by
trapping. Trap-free DOS instead ships `include/c4dos.h` whose `exit()`
performs a non-local return using the double-`LEV` trampoline proven
in `src/tests/test_coop_switch.c`: DOS saves its frame pair before
`invoke2`, the transient's `exit()` restores it and returns THROUGH
the trampoline — control lands at the RUN call site as if main had
returned. Programs that simply `return` from main need nothing.

Transients find DOS via the systable pattern (`src/c4ix/loader.c
loader_systable`): the loader scans the image's symbol section for
`__c4dos_api` and writes the service-table address into it. One
binary can run under C4DOS, C4KE, or bare — the shim checks the slot.

### Run frames, and programs that run programs (API v4)

Until 2026-10-24 RUN was not re-entrant: what it loaded lived in the
`img_*` globals and the 4 MB scratch (the constructor and destructor
tables pointed into it), and `dos_exit` returned through the one
`run_trans` frame. A program could not run another. grin, a shell for
C4DOS (`docs/grin-plan.md`), needs exactly that, and so does anything
that wants `DIR` or a batch file from inside a program.

- **Every call DOS makes into a program's code goes through a run
  frame**: its constructors, `main`, its destructors, and whatever a
  caller hands to `catch` (slot 23). `frame_enter` records its own frame
  for `dos_exit` and makes the call; `frame_call`, around it, keeps the
  outer frame's `g_run_bp` and `g_in_trans` in its own locals, so frames
  nest and an `exit()` lands in the innermost. The status comes home in
  `g_exit_status`, not in A: the trampoline tail is whatever follows the
  `JSR` in `learn_trampoline`, and its `return 0` may put an `IMM 0`
  there.
- **A load is a handle**, a block of words that owns the image's code
  and data, its own copies of the constructor and destructor tables, its
  heap generation (below) and its parent, the image that loaded it.
  `RUN` is load, start, unload. Unload takes with it any image the
  unloaded one left loaded, and refuses (-1) an image whose code is still
  on the stack. An `exit()` in a constructor is a refusal, like returning
  `C4DOS_REFUSE`.
- **`exec(cmdline)`** (slot 19) runs one command line exactly as the
  prompt would, builtins and batch files included, in a line buffer and
  argument vector of its own, and returns its status. `EXIT` inside an
  exec ends the exec, not DOS.

All of it is plain c4: `test-c4dos`'s tower leg runs the v4 scenarios
under unmodified `c4`.

### Memory: DEVICE=HEAP.SYS

Nothing used to free what a program allocated and never gave back. DOS
freed the image's code and data, and the rest stayed: Doom left
14,853,107 bytes behind every time it ran (measured on libjs,
2026-10-24), so "play, quit, play another" ran out of memory after a few
rounds.

libjs's heap now keeps a **generation** per block (`libjs/heap.js`), and
two registers beside `HEAP_USED` reach it (`src/c4bb/sim/devices.js`):

| register | | |
|---|---|---|
| `0x1C4 HEAP_GEN` | r/w | the generation new blocks get |
| `0x1C8 HEAP_RELEASE` | w | free every block of the generation written |
| | r | bytes the last release freed |

Generation 0 is the one the machine boots in. The heap never tracks it
and refuses to release it.

`DEVICE=HEAP.SYS` tells DOS the registers are there. Like the drives it
is announced, never probed, because under native c4m those addresses are
ordinary memory; and once announced it is checked: DOS writes 1 to
`HEAP_GEN` and must read 1 back. c4bb has the registers but no heap
behind them (its heap is the firmware's), reads 0, and DOS says
`this machine's heap keeps no generations` and carries on without.

With the line:

- each load takes a fresh generation, and the image's code and data
  are allocated in it; `start` and the image's own constructors and
  destructors run with it current;
- unload, which ends every `RUN`, releases it;
- **DOS's own memory is generation 0**, and every API entry that
  allocates on DOS's behalf switches to it and back: `create` and
  `write` on the RAM disk, the scratch, a handle and its tables, an
  exec's line. A RAM-disk file a child writes therefore outlives the
  child;
- `MEM` reports the heap: free, total, in use, and how many programs
  are loaded.

`make test-c4dos-heap` pins it on libjs: Doom run five times at the
prompt and quit with keys each time leaves `HEAP_USED` exactly where the
first round did (4,221,585 bytes, DOS's own and a test file), where
without the line it grows by 14.8 MB a round. The games disk the web
page boots (`build-images.sh`) carries the line.

## Files, writing, and the RAM disk

c4/c4m have NO file-write primitive (READ only), and c4bb's disk
controller was read-only too when this was written (it has drives with a
write head now: "Drives, and one spelling for them", above). Writing is therefore a DOS SERVICE: an
in-memory RAM disk (name → buffer table), DOS-style
`DEVICE=RAMDISK.SYS SIZE=n` (default 1 MB, 64 slots).

**Implemented.** `DIR` lists the union of the disk (read-only) and the
RAM disk (rw, marked `<ram>` with a usage line); opens check the RAM
disk FIRST, so a tool that rewrites a file shadows the read-only
original rather than failing. `ram:name` opens the RAM disk's copy and
nothing else (cpp and c4cc report what they write as `ram:<file>`, so
that spelling has to open); a bare `RAM:` at the prompt explains that
the RAM disk is not a drive. RAM files carry pseudo-descriptors above
`RAMFD`, which is why every read and close inside DOS goes through
`dos_read`/`dos_close` -- a pseudo-fd is not something the host has
ever heard of. `COPY src dst` is the builtin that writes, and `RUN`
loads out of RAM, which is the point: one stage's output is the next
stage's input. All of it is stock-c4 opcodes, so the clockless build
still runs the purity tower under unmodified `./c4`.

**Reachable from a transient too.** `include/c4dos.h` is the other half
of the ABI. DOS's loader scans each image's symbol section for
`__c4dos_api` and writes the table's address into it (`inject_api`), so
a tool built against that header calls DOS's own routines by address,
through the invoke stub. `cpp` and `c4cc` both use it -- that is how
`BUILD.BAT` gets a kernel onto the RAM disk with no write syscall
anywhere in the machine.

c4sp, and so c4lc and c4sc, use the same table when it is there: their
`file:write` goes to the RAM disk under C4DOS and to the ramfs (through
C4KE's custom opcodes) under C4KE. That is how `IX.BAT` builds C4IX with
c4sc at the DOS prompt.

### API versions

The table is 40 words (32 before v7), zeroed at boot. Slot 5 is the
version, 7 since 2026-10-26 (6 earlier that day, 5 from 2026-10-25, 4
from 2026-10-24).

| slot | v1 | | slot | v2 / v3 |
|---|---|---|---|---|
| 0 | magic `C4D` | | 9 | `count()` → RAM disk entries |
| 1 | `dos_exit(status)`, from any run frame | | 10 | `entname(i)` → `char *` |
| 2 | `create(name)` | | 11 | `entsize(i)` → bytes |
| 3 | `write(h, buf, len)` | | 12 | `entdata(i)` → `char *` |
| 4 | `close(h)` | | 13 | `trim()` → scratch bytes released |
| 5 | version | | 14 | `release()` → RAM disk bytes released |
| 6 | `open(name)` | | 15 | **v3** `time()` → ms since power-on |
| 7 | `read(h, buf, len)` | | | |
| 8 | `close(h)` | | | |

**v4** (2026-10-24, for programs that run programs and for shells):

| slot | call | returns |
|---|---|---|
| 16 | `conwrite(buf, n)` | n: console output, to the sink if one is set |
| 17 | `conread(buf, n)` | bytes, at most one line; 0 at end of input. The prompt's own buffer, so a line typed ahead of a program is the program's |
| 18 | `unlink(name)` | 0, or -1: delete from the RAM disk (v5: or from a drive, "The home drive" below) |
| 19 | `exec(cmdline)` | its status, run as the prompt runs it |
| 20 | `load(path)` | a handle, constructors run; 0 on failure or refusal |
| 21 | `start(h, argc, argv)` | `main`'s status; the image stays loaded |
| 22 | `unload(h)` | 0: destructors run, image and generation freed; -1 for a dead handle or one still running |
| 23 | `catch(fn, arg, exited)` | `fn(arg)`, or the status of an `exit()` inside it; `*exited` (when `exited` is not 0) says which |
| 24 | `sink(write, read, ctx)` | 0: the console goes to `write(ctx, buf, n)` and comes from `read(ctx, buf, n)`; 0 for either gives it back to DOS |
| 25 | *data* | the shell's word (grin's window kit table), 0 until a shell writes it |
| 26 | `gen(h)` | image `h`'s heap generation (`h` = 0: the image DOS last started, the caller from its own `main`); 0 without HEAP.SYS |
| 27 | `setgen(g)` | the previous generation, after making `g` current; -1 without HEAP.SYS |

**v5** (2026-10-25, grin's G4: drives that can be written):

| slot | call | returns |
|---|---|---|
| 28 | `home()` | the home drive (`DRIVES.SYS HOME=`), 0 = A:; -1 for none |
| 29 | `drives()` | how many drives there are; 1 without `DRIVES.SYS` |

**v6** (2026-10-26, grin's G5: redirection and DOS Prompt windows):

| slot | call | returns |
|---|---|---|
| 30 | `drive()` | the drive the prompt is on, 0 = A:. DOS has one for every caller; a DOS Prompt window keeps its own and asks after each line it runs |

and conwrite, conread (16, 17) follow `REDIR.SYS`'s redirections
("Redirection", below), which come before a sink. A sink (24) is called
as the program that installed it: a program it loads meanwhile is that
program's child, not the child of whatever DOS was running when it asked
for input. `include/c4dos.h` has `dos_drive()`.

**v7** (2026-10-26, directories; "Directories", below):

| slot | call | returns |
|---|---|---|
| 31 | `chdir(path)` | 0, or -1: the current directory of path's drive (CD) |
| 32 | `getcwd(drive)` | `char *`, DOS's own buffer: the drive's current directory as a person writes it, `\GAMES\DOOM`; drive -1 the prompt's |
| 33 | `mkdir(path)` | 0, or -1 (MD) |
| 34 | `rmdir(path)` | 0, or -1: an empty directory (RD) |

and every slot that takes a name takes a path, relative to the current
directory (`open`, `create`, `unlink`, `load`; `load` also looks along
PATH, as RUN does). `include/c4dos.h` has `dos_chdir()`, `dos_getcwd()`,
`dos_mkdir()` and `dos_rmdir()`.

v5's `create`, `write`, `close` (2-4) and `unlink` (18) reach drives:
"The home drive", below. `include/c4dos.h` has `dos_home()` and
`dos_drives()`. A handle `create` gives is no longer a RAM-disk slot
number but `WFD` (2000) plus an entry of DOS's, so nothing may treat it
as an index for `entname` and the like (nothing did).

26 and 27 are for a shell, which runs code of several images and has to
say whose memory a call allocates: `catch` runs in the caller's
generation, so grin switches to an application's before calling into it,
and back to its own in every window-kit entry point. They are always
there, answering 0 and -1 without HEAP.SYS, so a shell can call them
unconditionally.

Slot 1 was always 0 until 2026-10; since then it is `dos_exit`, which
returns from any depth to the innermost run frame (above). libc's
`bare.c` calls it directly, and `include/c4dos.h`'s `dos_exit()` reaches
it through the invoke stub, which works on plain c4 as well. An `exit()`
in a constructor is a refusal now; `C4DOS_REFUSE` is still the quiet way
to decline.

`c4dos.h` wraps every v4 slot through the invoke stub too (`dos_exec`,
`dos_load`, `dos_catch`, ...), so a c4cc program can use all of them;
`src/tests/dosexec.c` does, on every leg of `test-c4dos`. `bare.c` sends
fd 1, 2 and 0 through slots 16 and 17 when the version word says 4.

Slots 9–12 are advertised only when `DEVICE=RAMDISK.SYS` was installed;
2 when there is a RAM disk or a home drive, 18 when there is a RAM disk
or `DRIVES.SYS`; 28 to 34 always; and slot 15 only when
`DEVICE=CLOCK.SYS` was, so a caller
that checks a slot is told the truth rather than handed an always-empty
listing or a stopped clock. **Everything at 9 and above must be
gated on the version word**: a v1 DOS allocated sixteen words and
filled nine, leaving uninitialised heap where a caller would read a
function pointer. `dos_can_enum()` and `dos_can_time()` in the header do
both checks.

**Why the clock is in the table at all.** `TIME` is opcode 53, above
`EXIT`, so a transient that calls it directly leaves the base rung —
and `c4m-dos32.c4r` is pinned in `RUNG_BASE` so the campaign can say
the VM needs no opcode the machine did not boot with. Borrowing DOS's
clock through the table costs the caller nothing above `EXIT`, because
this header reaches the slots through its invoke STUB rather than an
indirect call. c4m is the first user: without it, `c4_time()` falls
back to reading `/proc/uptime`, which no breadboard has, and a guest
waiting for the clock to advance waits forever
(`docs/dos-rung-fixes.md` F11).

v1 lets a tool ask for a file BY NAME. v2 is for a *loader*: something
taking the machine over wants everything the RAM disk holds without
being told what is on it, and then wants the memory. That is what
`src/c4ke/extensions/c4ke_dos.c` does with it.

(Persistent writes arrived later as hardware: c4bb's drives, reached by
`bbsave` and `install`, which drive the disk registers directly. Since
v5 DOS's own API writes to them too, given a home drive or a name with
a drive in it: "The home drive", below.)

DECISION (user, 2026-08-21): NO `>` output redirection. Compilers and
tools write files DIRECTLY through the API (`dos_create`/`dos_write`/
`dos_close` slots) instead of a captured console — simpler, and
anything printf-shaped can be done in software where needed (the c4lm
stdio.h vsnprintf is sitting right there when a tool wants it). Batch
stays a command list, not a shell language.

**DECISION CHANGED (user, 2026-10-24), built 2026-10-26: redirection is
opt-in.** grin's DOS Prompt windows want `DIR | ...` and `> file` (its
plan, `docs/grin-plan.md` 3.7), so `DEVICE=REDIR.SYS` turns on `>`,
`>>`, `<` and `|`, and `DEVICE=REDIR.SYS TAP` says the machine has the
console tap, for c4cc programs too. **Without the line, a command line
is parsed exactly as before** — the 2026-08-21 decision stays the
default, `ECHO a > b` still says `a > b`, and the build floppies, which
do not have the line, are unaffected. Tools still write their files
through the API. Batch files are still command lists; they redirect,
and are redirected, like any other command.

## Redirection: DEVICE=REDIR.SYS

    cmd > file      cmd >> file      cmd < file      a | b | c

- **Where it happens.** The prompt, each line of a batch file and
  `exec` (slot 19) all run a command line through `line_run`. Without
  `REDIR.SYS` that is `dispatch`, as it always was. With it, a line with
  `>`, `<` or `|` in it goes through `redir_run`: split at the `|`s, each
  segment's redirections taken out of it (`seg_parse`; a name runs to the
  next space, `>` or `<`), and each segment run by `run_seg`, which opens
  the files, keeps the outer level's state in its locals, runs the
  command (a batch file it names included) and puts the state back. So
  `T > T.OUT`, where `T.BAT` redirects lines of its own, nests.
- **Targets.** Anything `create` can make: `RAM:NAME`, `B:NAME` on a
  drive that can be written, a plain name on the home drive or else the
  RAM disk. `>>` reads what the file held and writes it back first (a
  file on a drive is written whole when it is closed). `<` reads the
  whole file into a block of DOS's own.
- **Pipes** run `a`, then `b`, through a temporary file on the RAM disk
  (`RAM:PIPEn.$$$`), as MS-DOS's did, deleted when the next segment is
  done; they need `RAMDISK.SYS`, and say so without it. `DIR` leaves
  `.$$$` files out, so `DIR | x` does not list its own pipe.
- **The catch** of making it opt-in rather than universal: with the line
  in, a `>` in an `ECHO` is a redirection. The page's disk had `ECHO ...
  back to A>.` in its AUTOEXEC, which wrote a file called `.` on B: until
  the browser check showed the line missing (fixed in build-images.sh).
- **What is redirected.** DOS's own output: every builtin, its error
  messages and a batch file's echo now go through `pf`, a small
  formatter (`%d %s %c %% %.Nd`, a format and exactly three arguments,
  since plain c4 has no varargs of its own) that hands its buffer to
  conwrite. A `bare.c` program's fd 1, 2 and 0, which go through slots
  16 and 17. And, with the tap, a c4cc program's `PRTF`, `PUTC`, `PUTS`
  and its `READ` of fd 0.
- **Order of precedence** for the console: a redirect file, then a
  shell's sink (slot 24), then the console. Under a DOS Prompt window's
  sink, a line with no redirection in it still goes through `redir_run`
  when the tap is in, so that a c4cc program's output reaches the window.
- `TYPE /P`'s pause reads its line through conread, so a DOS Prompt
  window's Enter answers it; paging is off into a file. `TYPE` copies
  through a buffer of its own rather than the scratch, since a pause can
  wait in grin while grin loads a program.
- **ECHO says the rest of the line**, however many words: argv stops at
  15, and ECHO puts back the spaces `parse_line` cut. (It used to drop
  everything past fourteen words.) A redirected `ECHO hi > x` writes
  `hi` with no trailing space.

### The console tap

A c4cc program prints with `PRTF`/`PUTC`/`PUTS` and reads with `READ`
on fd 0, all straight to the machine, never near DOS. The tap is four
registers in `src/c4bb/sim/devices.js`, so c4bb and libjs both have it:

| register | | |
|---|---|---|
| `CON_TAP` 0x1d0 | w | 1: capture -- every byte the console would send (the opcodes, `UART_TX`, the block write) goes into a host buffer instead; 0: the console again (what is held stays) |
| | r | bytes held |
| `CON_ADDR` 0x1d4 | w | a buffer, for the two below |
| `CON_LEN` 0x1d8 | w | drain: up to N held bytes copied to `CON_ADDR`, oldest first; r: how many |
| `CON_IN` 0x1dc | w | fd 0's `READ`s take the N bytes at `CON_ADDR` (copied when written), then end of input; -1: the keyboard again. r: bytes left, or -1 |

DOS captures whenever the console goes anywhere but the console (a
redirect file or a sink) and drains what was caught to wherever it goes
before writing anything else there and when a level of redirection ends
(`tap_drain`, `tap_set`); `con_put` lets go of the capture for what is
meant for the console itself. Fd 0 is fed from a `<` file, with nothing
at all under a sink (a c4cc program in a DOS Prompt window reads end of
input rather than the page's keyboard), and is the keyboard otherwise.

**Announced, never probed:** `DEVICE=REDIR.SYS TAP`. Native c4m has no
device window there, so the line is left off its floppies; there a
redirected c4cc program's output stays on the console, and DOS says so,
once, on the console. Everything redirection needs in DOS itself is plain
c4 (the tap's loads and stores run only with `TAP`), and the tower leg
of `test-c4dos` runs the redirection session under unmodified `c4`.

## dosload — LOADLIN for C4DOS

`src/c4dos/dosload.c`, built as `dosload.c4r`. Loads an image, hands
DOS's memory back, and jumps:

    RUN dosload.c4r c4ke.c4r -v 50

**Be clear about what it does not do.** `RUN c4ke.c4r` already works,
and DOS already injects the API into it, so a kernel started the
ordinary way can already find the RAM disk. dosload adds two things:

- **Memory.** DOS holds a 4 MB read scratch. dosload copies the loaded
  image's constructor and destructor tables out and resolves them to
  absolute addresses, so it can call `dos_trim()` before the kernel
  starts. (When dosload was written DOS's own copy of *dosload's* tables
  pointed into that scratch too, which is why dosload declares no
  destructors; since API v4 DOS keeps copies of its own, so the scratch
  can go at any time.)
- **A command line.** `RUN` passes a transient its own arguments;
  dosload passes the image name as `argv[0]` and everything after it as
  the loaded program's, so `-v 50`, `-c N` or an alternate init reach
  the kernel's `parse_commandline`.

Strict c4, nothing above the `EXIT` opcode: `make test-dosload` pins
that with `c4l.c`, which refuses an image that uses more and names the
instruction. This is a tool the player runs *at* the C4DOS rung, and
loading a kernel must not be the thing that demands a better CPU.

## CONFIG.SYS and AUTOEXEC.BAT

- `CONFIG.SYS`: `DEVICE=CLOCK.SYS` (enables TIME/C4CY use),
  `DEVICE=RAMDISK.SYS SIZE=n`, `DEVICE=CONSOLE.SYS FLUSH`,
  `DEVICE=DRIVES.SYS` (with `HOME=B:`, "The home drive" below) and
  `DEVICE=HEAP.SYS` (each program's memory freed when it ends; "Memory:
  DEVICE=HEAP.SYS" above) and `DEVICE=REDIR.SYS` (with `TAP`;
  "Redirection" above), all implemented; `FILES=n`, `SHELL=...`
  reserved.
  Parsed with a flattened-locals descent (the vfsload.c skeleton,
  c4cc-dialect).

  `CONSOLE.SYS FLUSH` says **this console shows a partial line**, and
  the prompt is `A>` with the cursor after it, the way DOS always
  looked. Without it DOS puts the prompt on its own line instead,
  because nothing in this family can flush a buffer: natively `PRTF`
  bottoms out in the host libc, which is line-buffered on a TTY, so a
  bare `A>` sits there unseen and the machine looks hung. eshell solved
  the same problem the same way and says so at `src/c4ke/c4ke.c:90`.

  It is a config line and not a probe on purpose. The only "is this
  c4bb?" test in the tree is `include/c4bb_info.h`, and that header
  says why it is no use here: it asks `__c4_info()`, `INFO` is opcode
  57, and c4cc compiles every function in a header whether it is called
  or not — so including it would put DOS above the base-c4 rung and
  break the purity pin. *"The capability is announced, never probed."*
  The c4bb disks announce it; the native ones do not.
- `AUTOEXEC.BAT`: line-per-command batch, `ECHO`, `REM`, `@` prefix,
  run through the same reader as the interactive prompt (the c4sh
  INPUT_STDIN/INPUT_STREAM split, minus its known multi-line-read bug).
- Builtins: `DIR` (the prompt's drive's listing; `DIR B:` types B:'s),
  `TYPE`, `COPY`, `DEL`
  (or `ERASE`), `RUN` (implicit for *.C4R names),
  `ECHO`, `REM`, `VER`, `TIME` (wants CLOCK.SYS), `MEM` (the heap too,
  with HEAP.SYS), `EXIT` (halts —
  the one legitimate use of the EXIT opcode), a drive letter such
  as `B:` (below), and since v7 `CD`, `MD`, `RD`, `PATH` and `APPEND`
  ("Directories", below). A `.BAT` name runs the batch file.
  `TYPE file /P` and `DIR /P` page: 23 lines, then `-- More --` and a
  wait. The wait reads a line through conread (`con_get`, which like
  the prompt's reader understands that one `read(0, ...)` may carry
  several lines), so a pause eats exactly one and leaves the rest of a
  piped script for the prompt. Paging is off unless `/P` is given, so
  nothing that scripts `TYPE` changes. All of them print through `pf`
  (above), so they can be redirected. With `HOME=B:`, `DIR` lists the
  prompt's drive by name (`A:c4dos.dir`): the plain name opened the home
  drive's listing first, so `DIR` at A> listed B: (until 2026-10-26).
  Since v7 `DIR [path]` lists a directory -- the current one, `B:`'s
  current one, any path -- under a ` Directory of A:\GAMES` line, a
  subdirectory as `NAME  <DIR>`.

### Drives, and one spelling for them

`DEVICE=DRIVES.SYS` tells DOS the machine has more than one drive. It is
announced for the same reason the clock and the console are: the drive
registers are a device window at `0x13c`/`0x188` on the board and
ordinary memory under native c4m, so a probe would be a wild read. With
the line, the prompt names the drive it is on and `B:` moves between
them:

    A:\>B:
    B:\>DIR
    B:\>A:
    A:\>TYPE B:note.txt

(The prompt was `A>` until v7; it has said the current directory since,
`A:\>`.)

**Letters, everywhere a person can see.** The machine itself takes
either spelling -- `Devices.resolveDrive` accepts `0:` and `A:` alike --
but the prompt has said `A>` since the first boot, and `install 1:`
against an `A>` prompt was two vocabularies for one idea. So the
player-facing commands are `INSTALL B:`, `kinstall C:`,
`RUN bbsave.c4r B:`, and `install` writes its destination prefix as a
letter too. The drive NUMBER survives only where the hardware is the
subject: `cli.js -d`, the BIOS, the drives panel, and
`docs/c4bb-storage.md`'s register table.

A prefix reaches across drives anywhere a filename is taken --
`TYPE B:note.txt`, `COPY B:x.c y.c`, `RUN B:hello.c4r`. The disk
controller resolves the prefix itself (it has since M5); `dos_open`
looks the name up in that drive's own `c4dos.dir` when it does not open
as typed, so case does not matter there either.

### The home drive: DRIVES.SYS HOME=B:

`DEVICE=DRIVES.SYS HOME=B:` names the drive files are written to (2026-10-25,
`docs/grin-plan.md` 3.8). The page's DOS disk (`libjs/images/dos`) has
it, and the page keeps drive B: in the browser, so what C4DOS saves
there is there at the next visit. A home that is not there or cannot be
written (`DISK_RO`) is no home, and DOS says so at boot. The build
floppies name none, and everything they do goes to the RAM disk as
before.

- **Writing** (`create`, `COPY`): `RAM:name` goes to the RAM disk;
  `B:name` to B: if it can be written, else to the RAM disk under the
  plain name; a plain name to the home drive, else the RAM disk. A
  file on a drive goes through the write head (`DISK_WNAME`, `DISK_WLEN`,
  `DISK_WCLOSE`, as `bbsave` drives it) and is on the medium, and in its
  `c4dos.dir`, once it is closed. A plain name written home takes away
  the RAM disk's copy of that name, if there was one, so that what is
  read next is what was just written.
- **Reading** (`dos_open`): a name with a drive is that drive's; any
  other is looked for in the RAM disk, then on the home drive, then on
  the drive the prompt is on -- each in its current directory, or from
  the root for a name that starts with `\` -- and then along APPEND.
- **Listings**: DOS keeps a `c4dos.dir` on every drive it writes,
  adding a name when its file is closed and taking it out when the file
  is deleted, and makes one on a drive that had none. `DIR B:` lists it,
  and grin's File Manager reads it.
- **Deleting** (`unlink`, `DEL`): `RAM:name` off the RAM disk, `B:name`
  off B:, a plain name off the RAM disk if it is there, else off the
  home drive. The machine has a register for it, `DISK_UNLINK` (0x1cc),
  beside the write head; the page takes the file out of its store as
  well.
- **Write handles** are an indirection: `create` answers `WFD` plus an
  entry saying where the file goes (a RAM-disk slot, or a drive's
  descriptor). An unlink that compacts the RAM disk's slots moves the
  entries with them, so a file open for writing is never written into
  its neighbour (the G0 caveat this replaced). A program that ends with
  files still open for writing has them closed for it, so what it wrote
  is kept.
- `COPY x B:` (or `RAM:`) keeps the name.

On the bare machine, with no DOS, libc's `bare.c` does the same with
drive 1 itself when the info word announces the drive registers
(`C4I_DRV`, 0x40000): files are written there and read from it before
drive 0. It also renames (a copy and a delete: Doom saves by renaming
`temp.dsg`) and answers `ftell` on a file being written, which Doom pads
its saves by.

## Directories (API v7)

Until 2026-10-26 every disk was one directory: whatever its `c4dos.dir`
listed. The page's disks had outgrown that -- the Build Ladder's root
held 249 names, games beside compilers beside C4IX's userland -- and the
user asked for directories ("Having everything in one directory is
quickly becoming unmanageable"). Now:

    A:\>CD GAMES\DOOM
    A:\GAMES\DOOM>DIR
     Directory of A:\GAMES\DOOM
    c4dos.dir
    doom.c4r
    doom1.wad
    A:\GAMES\DOOM>DOOM
    A:\GAMES\DOOM>CD \
    A:\>

- **A name is a path**: `GAMES\DOOM\DOOM.C4R`, `..\X`, `\X` from the
  root, `B:X` in B:'s current directory, `B:\X` from B:'s root. `/` is as
  good as `\` everywhere but at the start of an argument to `DIR` or
  `TYPE`, where it is a switch (`/P`), as it always was. Names and
  directories are found case-insensitively, a part at a time.
- **Every drive has a current directory**, the prompt shows the prompt
  drive's (DOS's `$P$G`: `A:\>`, `A:\GAMES\DOOM>`), and `CD B:\X`
  changes B:'s without leaving A:, as in every DOS. A batch file's echoed
  lines carry the same prompt.
- **Builtins**: `CD` (or `CHDIR`; `CD` alone says where the prompt's
  drive is, `CD B:` where B: is, and `CD..` and `CD\` work without the
  space), `MD` (`MKDIR`), `RD` (`RMDIR`: only an empty directory, and not
  one that is, or holds, a current directory), `DIR [path]`, `PATH` and
  `APPEND` (below). `COPY X DIR` copies into a directory under the same
  name, as `COPY X B:` always did.
- **Everything that takes a name takes a path**: `TYPE`, `COPY`, `DEL`,
  `RUN`, a command word (`\GAMES\DOOM\DOOM`), a batch file, redirection
  targets, and every API slot that names a file -- `open`, `create`,
  `unlink` (6, 2, 18) and `load` (20) -- so a program built against
  `include/c4dos.h` or `bare.c` resolves its files relative to the
  current directory without knowing directories exist.

### How a drive describes its tree: a c4dos.dir in every directory

The question was whether a read-only drive should carry one listing of
paths (the drive's keys already hold slashes, `src/c4ix/init.c`) or a
listing per directory. Per directory, because:

- `DIR` types one small file instead of filtering a large one, and the
  name resolver reads one listing a level, not the whole tree for every
  open;
- writing a file adds one line to its own directory's listing; with one
  listing every save on B: rewrote the whole tree's (and `DIRMAX`, 16 KB,
  would have been the size of the disk rather than of a directory);
- a disk with no subdirectories is exactly what it was: the old root
  `c4dos.dir` is a valid listing of the root, so every floppy and every
  B: written before v7 works unchanged.

A directory is a line `NAME/` in its parent's listing, and has a
`c4dos.dir` of its own listing what is in it (`MD` writes both; `RD`
takes both away). The trailing slash is how `DIR` knows to say
`NAME  <DIR>` and how grin's File Manager knows a folder. A directory is
there when its parent's listing says so -- never because `NAME/c4dos.dir`
happens to open (see the machine's basename fallback, below).
`build-images.sh`'s `listings` writes one into every directory of the
page's disks.

### Paths inside DOS, and what the machine is asked for

`path_full` makes any name a drive and a path from that drive's root,
`/` between the parts and `.` and `..` gone (`..` above the root is an
invalid directory); `path_real` finds its real spelling through the
listings. DOS keeps each drive's current directory that way
(`GAMES/DOOM`), and never asks for a relative name:

- with `DRIVES.SYS` it asks the machine for `A:/GAMES/DOOM/doom.c4r` --
  the slash after the drive says from the root (`devices.js`), and for
  such a name the machine does **not** fall back to the name's last part
  the way it does for anyone else's (`/home/user/hello.c` finds
  `hello.c`): DOS means that file and no other;
- without it the path alone, `GAMES/DOOM/doom.c4r`: native c4m opens it
  in the directory it was started in, which is the disk, and so does
  plain `c4` (the tower leg of `test-c4dos` runs a directory session
  under unmodified `c4`).

### The RAM disk has paths too

Its names are paths from the root (`BUILD/c4ke.i`), and it still sits
over every drive, now at the same path: a plain name written in
`A:\BUILD` with no home drive is the RAM disk's `BUILD/c4ke.i`, which
`A:\BUILD\C4KE.I` and `TYPE \BUILD\c4ke.i` both find. It has no
listings: a directory is there while a file in it is (`CD TMP` after
`COPY X RAM:\TMP\Y`), and `DIR` shows it as `TMP  <DIR> <ram>`. `RAM:X`
is the RAM disk's `X` in the current directory, `RAM:\X` at its root.
Slots 9-12 enumerate those paths, and C4KE's dos extension seeds its
ramfs with them as they are -- which is why `BUILD.BAT`, `LADDER.BAT`
and `IX.BAT` begin with `CD \`: what they build is at the root of the
RAM disk, where C4KE's loader looks for `init.c4r` (built in `\BUILD`
it would be `BUILD/init.c4r`, and C4KE would quietly boot some other
init).

### PATH and APPEND

    PATH \BUILD;\GRIN;\BIN
    APPEND \BUILD;\C4KE;\C4IX;\GAMES\DOOM

`PATH` is where a command (a `.c4r`, a batch file, `RUN`'s argument, a
program `load` (slot 20) is given) is looked for after the current
directory. `APPEND` is MS-DOS 3.3's: where **any** file is looked for
after the current directory (and the RAM disk and the home drive) -- for
programs written before directories, which open `doom1.wad` or
`c4ix.vfs.txt` and expect it to be there. Both are kept as drives and
paths from the root, so a list set in one directory means the same in
every other; `PATH ;` and `APPEND ;` clear them; with no argument they
say what they are.

**The programs that never come to DOS** -- C4KE (loaded by `dosload`),
C4IX's `vfsload`, a c4cc program's own `open()` -- open relative names
straight on the machine. For them DOS tells the machine where to look:
`DISK_DIRS` (`0x1e0`, `src/c4bb/sim/devices.js`) holds a drive's search
list, its current directory followed by APPEND's directories on it
(`GAMES/DOOM;BUILD`), and a relative name is looked for along it, then
at the root. DOS writes it whenever a current directory or APPEND
changes (`dirs_tell`), with `DRIVES.SYS` only: like every register DOS
touches, it is announced, never probed. So `CD B:\W` then `RUN
A:\C4.C4R HELLO.C` compiles `B:\W\HELLO.C`, and on the ladder disk C4KE
finds `c4sh.c4r` in `\C4KE`, `doom.c4r` and its WAD in `\GAMES\DOOM`, and
C4IX's `vfsload` finds its tree in `\C4IX` -- none of them changed. A
name DOS's search list does not mention is still found at the root, as
before directories.

**Programs' opens keep one old courtesy.** The machine finds a name it
does not have by its last part, and Doom came to rely on that: it asks
for `./.savegame/doomsav0.dsg`, and its saves are at `B:\doomsav0.dsg`.
DOS's own lookups are now exact, so `open` (slot 6) alone tries a name
with directories in it once more by its last part when it opens
nowhere.

### grin

grin speaks v7 through `src/grin/disk.c` (`gd_getcwd`, `gd_chdir`):

- **File Manager** lists a directory, folders first with a folder icon;
  a double click opens one, and `..` goes back up. Its names are
  `A:/GAMES/DOOM.C4R` -- from the root, whatever the current directory --
  and so are the kit's (`/A:/GAMES/DOOM/` in a file box, `dospath.h`).
- **DOS Prompt** windows keep a directory each, as they keep a drive:
  the window's is made current before its line runs and read back after.
- **A DOS program named by a path runs in its own directory** (`grin.c`
  `full_where`), as Program Manager's and File Manager's did in Windows
  when an item gave no working directory: Program Manager's games are
  `\GAMES\DOOM\DOOM` and find their data beside them, and DOS's current
  directory is put back afterwards. A `PROGMAN.INI` written before v7
  names the games by bare words; reading it gives those items the new
  command lines (and writes it back).

### The page's disks

`build-images.sh` lays both out in directories:

| disk | directories |
|---|---|
| `libjs/images/dos` (`?system=c4dos`) | `GRIN` (grin and its applications), `BIN` (`ASKLINE`, `DOSFILT`, `HELLO`), `GAMES\DOOM`, `GAMES\WOLF3D`, `GAMES\ROTT`, `GAMES\TYRIAN` (each game and its data); at the root only `c4dos32.c4r`, `CONFIG.SYS`, `AUTOEXEC.BAT`. AUTOEXEC says `PATH \GRIN;\BIN` |
| `libjs/images/ladder` (`?system=c4dos-ladder`) | the same, and `BUILD` (the build kit: `BUILD`, `LADDER`, `IX`, the compilers, the archives, c4lc's Lisp, `b4ke` and what it reads), `C4KE` (C4KE's programs and their sources), `C4IX` (C4IX's userland, `c4ix.vfs.txt`, `c4ix.sizes`, its copy of Tyrian's data); `src\` as it was. AUTOEXEC says `PATH \BUILD;\GRIN;\BIN` and `APPEND \BUILD;\C4KE;\C4IX;\GAMES\...`, so `DOOM` and `BUILD` still work from the root and C4KE and C4IX find everything they did |

Which game a file belongs to comes from the games disk's assembly
(`$OUT/.gamefiles/NAME`, what each `c4-images.sh bare` put there). The
games disk itself (`src/c4bb/images/games`) stays flat: the bare-machine
games on the page (`?system=doom` and the rest) now boot from it, since a
bare game opens its files from the root.

## Building and running

Three images, because they answer three different questions. All go
through our own `cpp` -- a compiler that skips `#` lines would compile
both sides of the clock `#if` into one image -- and are then compiled by
c4lc `-O` (c4sp for the 64-bit images, c4sp32 for `c4dos32.c4r`). They
were c4cc builds when this was written; `src/c4dos/tests/test-c4dos.sh`
still builds its own copies with `./cpp | ./c4cc`, so the test pins the
c4cc route and the Makefile ships the c4lc one.

| target | what it is |
|---|---|
| `make c4dos.c4r` | clockless, 64-bit. The purity pin: runs under unmodified `./c4` via `c4l.c`, which refuses any image using an opcode above `EXIT` -- `TIME` included. |
| `make c4dos-clock.c4r` | the same plus `TIME`, gated at runtime behind `DEVICE=CLOCK.SYS`. The dev loop. |
| `make c4dos32.c4r` | 32-bit clock build: the image c4bb boots, and the one an embedder wants. Also built into `src/c4bb/images/` by `build-images.sh`. |

C4DOS opens `config.sys`, `autoexec.bat` and `c4dos.dir` relative to
the working directory (other drives are reached with `DEVICE=DRIVES.SYS`,
above), so "the disk" is
simply a directory you `cd` into (or hand to c4bb with `-d`); its
subdirectories are C4DOS's, given a `c4dos.dir` each ("Directories"). `make`
assembles two, since a 32-bit machine will not load a 64-bit transient:
`c4dos-disk/` for the native runs and `c4dos-disk32/` for c4bb. Both
carry `hello.c4r` and `raycast.c4r`.

    make run-c4dos        # native c4m, clock build -- the A> prompt
    make run-c4dos-c4     # the tower: unmodified c4 -> c4l -> DOS
    make run-c4dos-bb     # the breadboard machine, interactive
    make serve-c4bb       # the breadboard machine, in a browser

### The build floppy

`make run-c4dos-build` is the one that matters for the ladder: boot it,
type `BUILD`, then `dosload c4ke.c4r`, and you are in a kernel this
machine just compiled. As of 2026-09-05 that floppy carries a **userland**
-- `ls ps top cat echo type xxd kill spin c4le`, the three benchmarks, the
games, and `c4`/`c4m` -- plus `vfsload.c4r` and a manifest shaped for it.
Before that it carried only the toolchain, so the kernel it built came up
with a shell and nothing to run in it: `ls` answered
`lc4r: unable to open 'ls' or 'ls.c4r'`.

Nothing in the kernel had to change for this. `task_loadc4r` checks the
RAM filesystem and then the HOST, and the run target `cd`s into the
floppy, so a `.c4r` sitting there is loadable. What `ls` *lists*, though,
is the ramfs -- which is why `vfsload` has to be there too.

The games and `c4`/`c4m` are the `u0lite` (or bare) builds, so one image
runs at the `A>` prompt AND inside the kernel. `docs/dos-rung-fixes.md`
tracks the whole change.

`EXIT` halts. `DIR`, `TYPE`, `COPY`, `RUN`, `ECHO`, `REM`, `VER`,
`TIME`, `MEM` are the builtins; `RUN raycast.c4r -d` is the one transient
with something to look at.

### The tools on the floppies

| tool | what it does |
|---|---|
| `dostar x\|t archive.tar` | unpacks a C4TAR1 archive onto the RAM disk (`c4ke-src.tar`, `tools-src.tar`, `c4ix-src.tar`) |
| `dosload image.c4r [args]` | loads a kernel, gives back DOS's scratch, starts it (above) |
| `bbsave B: [file...]` | writes RAM-disk files onto a c4bb drive: no manifest, no boot record |
| `install B: [manifest]` | writes a **bootable** medium from `install.lst` (the default): fetches each name from the RAM disk or the floppy, writes nothing until every required name is found, then writes `boot.cfg` for the BIOS |

`bbsave` and `install` drive c4bb's disk registers directly, so they
are 32-bit, board-only transients (`bbsave32.c4r`, `install32.c4r`).
`make c4dos-c4ix32` carries all four; the walkthrough is
`docs/climbing-the-ladder.md`.

### Finding a file

`c4dos.dir` IS the directory service -- `DIR` lists that file, because
the raw disk cannot enumerate itself -- and it doubles as the name
resolver. Every open goes through `dos_open`, which tries the name as
typed and, failing that, finds each part of its path in its directory's
listing, case-insensitively, and opens the spelling that actually
exists. So `HELLO`, `hello.c4r`, `Hello.C4r` and `\games\DOOM\DOOM.C4R`
all reach the same files, the way DOS always let them. Since v7 there is
a listing in every directory ("Directories").

A command with no extension gets `.c4r` appended and retried, which is
the courtesy `COMMAND.COM` extended with `COM`/`EXE`/`BAT`: you type
the program, not the file. `RUN` does the same to its argument, and
both look along PATH after the current directory. A word
that resolves to nothing still reaches `bad command or file name`,
rather than being reported as a broken executable.

Two consequences worth knowing:

- **`c4dos.dir` must carry the TRUE on-disk spellings.** `dos_open`
  opens the name it finds in the listing, so an entry of `HELLO.C4R`
  beside a file called `hello.c4r` would defeat the very lookup it
  exists to serve. Both make-built floppies generate it with `ls`, and
  `DIR` therefore shows real case rather than shouting.
- **A disk with no listing keeps exact-match behaviour**, which is the
  same trade `DIR` already makes. Nothing depends on the fallback
  existing.

`src/c4bb/images/disk/` carries `config.sys`, `autoexec.bat` and a
generated `c4dos.dir` alongside the C4KE and C4IX corpus, so
`c4dos32.c4r` boots the SAME disk as `c4ke32.c4r` and `c4ix32.c4r`. An
embedder already serving that directory gets a third operating system
for the cost of one more image.

## Testing

None of these is part of `make test`.

| target | what it pins |
|---|---|
| `make test-c4dos` | four legs: native c4m, the clock build, unmodified `c4` through `c4l.c` (the purity pin: the clockless image uses no opcode above `EXIT`), and c4bb, including `c4.c4r` compiling hello inside it. Its script builds the images with `./cpp \| ./c4cc`, not the Makefile's c4lc. API v4 (`src/tests/dosexec.c`: exec three deep, a resident image, catch, a child's file, unlink, the console slots, the sink) runs on the native leg, the tower and c4bb; on c4bb it runs once more with `DRIVES.SYS HOME=B:` and a writable B: (the CLI's `-w`), with `COPY`, `DIR B:` and `DEL`. Redirection (`REDIR.SYS`: `DIR > LIST.TXT`, `>>`, `src/tests/dosfilt.c` given `<` and in a pipe of three, a redirected batch file and error message, a 17-word `ECHO`, no pipe file left) runs on the native leg and the tower, and without the line `>` and `\|` stay words; on c4bb, `REDIR.SYS TAP` with `HOME=B:`: `hello32 > OUT.TXT` (a c4cc program's printf, through the tap, onto B:), `hello32 \| dosfilt`, and `dosfilt -r` reading fd 0 from a pipe. Directories (v7) on the native leg and the tower: `CD` and `DIR` of a subdirectory, `..\`, `\` and `/` paths, a program and a batch file run there, by a path and along `PATH`, a note found along `APPEND`, the RAM disk's directories; on c4bb with `HOME=B:`, `MD`, `RD` (refusing a full directory), `COPY` into one, `CD` there and a c4cc program's own `open()` finding its file through `DISK_DIRS` |
| `make test-c4dos-heap` | on libjs, the Makefile's c4lc `-O` image with `HEAP.SYS`: the v4 scenarios with c4lc `-O` transients, a child's leftovers given back and its RAM-disk file kept, then Doom five times at the prompt with `HEAP_USED` within 1% (`libjs/tests/test-c4dos-heap.mjs`; Doom needs the games disk) |
| `make test-cpp` | `cpp \| c4cc` and `gcc -E \| c4cc` give byte-identical images over the corpus, and `cpp.c` interpreted by plain c4 matches the native build |
| `make test-dosload` | `dosload.c4r` uses no opcode above `EXIT` |
| `make test-ladder` | the ladder on the host: C4DOS builds C4KE, `dosload` hands over, both toolchains compile and link in the RAM filesystem. Minutes |
| `make test-c4dos-build32` | on c4bb: `BUILD`, then boot the kernel just built, to a clean shutdown |
| `make test-c4dos-ladder32` | on c4bb: `LADDER` rebuilds the compiler to a fixed point, then the kernel; boots it |
| `make test-c4dos-c4ix32` | on c4bb: `IX` builds C4IX with the compiled compiler and boots it. Minutes |
| `make test-libjs-ladder` | on libjs, the page's ladder disk: Doom at `A>`, `BUILD`, dosload, Doom under C4KE, b4ke, `kboot` into C4IX, Doom there, and back to `A>` with every rung's memory given back ([c4dos-ladder.md](c4dos-ladder.md)). About seventy seconds |
| `make test-c4dos-c4fc` | on c4bb: c4fc compiles a C program and C4DOS runs the result. About three minutes |

`make test-c4bb-climb` (in `src/c4bb/tests/test-climb.sh`) is the
end-to-end one: three power-ons, three systems, each booted from a
medium the previous one wrote with `install`/`kinstall`.

## The ladder

samples → C4DOS (this) → C4DOS runs cpp+c4cc to build C4KE → C4KE
(traps, preemption) hosts c4sp + c4lc → c4lc builds C4IX (protected
mode) → network coprocessor sub-board + CPU support → C4IX rebuilt
with networking → the time machine's control link (c4mp direction;
SMP is a stretch goal).

The DOS→C4KE rung is closed, and so is the rest of the software side
through C4IX (the network sub-board and after are not started): `BUILD.BAT` compiles a kernel *and* an
init onto the RAM disk, and `c4ke_dos.c` (a kernel extension, running
at `KEXT_START`) copies the whole RAM disk into the kernel's own RAM
filesystem before the init task is created. Since `task_loadc4r` checks
that filesystem before the host, the init that boots is the one the
machine just built — it never existed as a file. The DOS RAM disk is
released immediately afterwards; it was the initrd.

The plan that got it there is `docs/ladder-plan.md` (complete); the
climb by hand is `docs/climbing-the-ladder.md`. On libjs's page the whole
climb runs in one session (`?system=c4dos-ladder`, `docs/c4dos-ladder.md`):
C4KE boots the C4IX it built with `kboot`, and each rung returns to the
one below when it stops, with HEAP.SYS giving all of its memory back.

## c4fc, the compiler in the machine

`make c4dos-c4fc` builds a floppy whose compiler is **c4fc**, and
`make test-c4dos-c4fc` runs the whole loop: C4DOS boots, loads c4fc,
compiles a C program, writes the image to its RAM disk, and runs what it
just built.

The existing build floppy uses `cpp` + `c4cc`, which is the pair that
has always been able to do it. This one carries `c4th.c4r` and c4fc's
Forth sources plus `c4rlink.c4r`, so the machine can compile a unit to
an OBJECT and link objects into a program -- the shape a real toolchain
has and the one c4cc cannot do.

**The thing that had to be built first: c4fc could not write a file.**
It emitted its image to stdout, which is what a Makefile wants and what
C4DOS cannot use -- there is no `>` on this system by decision, and a
tool is expected to write its own file. The VM has no write syscall
either, so the only route is C4DOS's own API table.

`src/c4th/forth/dos.f` is that route, and it is small because the pieces
were already there. C4DOS's loader patches the address of its API table
into any image carrying the symbol `__c4dos_api`, so c4th declares one
and hands it to Forth through the `C4DOS-API` primitive. The table's
slots hold ordinary function addresses, and c4th's `INVOKE1`/`INVOKE3`
call an address with arguments -- the invoke stub `include/c4dos.h`
describes exists because C cannot call through a variable; Forth can.
`SAVE-BLOCK` then means "put this block in this file" on whichever
machine we are on: DOS if there is a DOS, the host's own `open`/`write`
if we are native, and an honest failure on a bare VM where neither
exists. c4fc grew `-o` on top of it.

Three things worth knowing before working here:

- **`ARGVMAX` is 16 tokens.** `RUN c4th.c4r <fifteen .f files>` is
  silently truncated and nothing happens. The floppy ships c4fc as ONE
  concatenated file, which is also one open instead of fifteen.
- **`IF`/`THEN` are compile-only**, so the top level of a `.f` file
  cannot branch -- it has to be a definition that is then run. Getting
  that wrong runs both arms.
- **A stale `c4dos32.c4r` is invisible and expensive.** Nothing in the
  disk rules depends on it, so an image a day older than `c4dos.c` will
  boot happily and hand out an API table with only the magic filled in.
  If a transient reports slot 0 correct and every other slot zero, that
  is what happened.

### What it costs, and what would change it

Loading c4fc on c4bb is about 19 s -- 3,200 lines of Forth read and
compiled through the threaded interpreter before a line of C is seen --
and compiling one real C4IX module took 5m29s. **The load is 6% of
that**, so an image save would not be the win it looks like, and c4th
cannot do one anyway: its dictionary holds real machine addresses
(`docs/c4th-design.md`), so a saved image is unrelocatable, and there is
no write syscall to save it with.

The lever is the other 94%. `src/c4th/forth/native.f` compiles Forth
words to real C4 code and is not switched on here; that is where the
time is.
