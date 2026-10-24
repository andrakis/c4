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
(`docs/grin-plan.md` G0).

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

The table is 32 words, zeroed at boot. Slot 5 is the version, 4 since
2026-10-24.

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
| 18 | `unlink(name)` | 0, or -1: delete from the RAM disk |
| 19 | `exec(cmdline)` | its status, run as the prompt runs it |
| 20 | `load(path)` | a handle, constructors run; 0 on failure or refusal |
| 21 | `start(h, argc, argv)` | `main`'s status; the image stays loaded |
| 22 | `unload(h)` | 0: destructors run, image and generation freed; -1 for a dead handle or one still running |
| 23 | `catch(fn, arg, exited)` | `fn(arg)`, or the status of an `exit()` inside it; `*exited` (when `exited` is not 0) says which |
| 24 | `sink(write, read, ctx)` | 0: the console goes to `write(ctx, buf, n)` and comes from `read(ctx, buf, n)`; 0 for either gives it back to DOS |
| 25 | *data* | the shell's word (grin's window kit table), 0 until a shell writes it |
| 26 | `gen(h)` | image `h`'s heap generation (`h` = 0: the image DOS last started, the caller from its own `main`); 0 without HEAP.SYS |
| 27 | `setgen(g)` | the previous generation, after making `g` current; -1 without HEAP.SYS |

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

Slots 2, 9–12 and 18 are advertised only when `DEVICE=RAMDISK.SYS` was
installed, and slot 15 only when `DEVICE=CLOCK.SYS` was, so a caller
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
`bbsave` and `install`, which drive the disk registers directly. The RAM
disk is still all a trap-free DOS writes through its API.)

DECISION (user, 2026-08-21): NO `>` output redirection. Compilers and
tools write files DIRECTLY through the API (`dos_create`/`dos_write`/
`dos_close` slots) instead of a captured console — simpler, and
anything printf-shaped can be done in software where needed (the c4lm
stdio.h vsnprintf is sitting right there when a tool wants it). Batch
stays a command list, not a shell language. (Revisited 2026-10-24 for
grin, keeping this as the default: redirection becomes opt-in with
`DEVICE=REDIR.SYS`, `docs/grin-plan.md` 3.7. Not built yet; the console
slots and the sink of API v4 are what it will stand on.)

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
  `DEVICE=DRIVES.SYS` and `DEVICE=HEAP.SYS` (each program's memory freed
  when it ends; "Memory: DEVICE=HEAP.SYS" above), all five implemented;
  `FILES=n`, `SHELL=...` reserved. grin's plan adds `REDIR.SYS` and
  `DRIVES.SYS HOME=` (`docs/grin-plan.md` 3.7, 3.8).
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
- Builtins: `DIR`, `TYPE`, `COPY`, `RUN` (implicit for *.C4R names),
  `ECHO`, `REM`, `VER`, `TIME` (wants CLOCK.SYS), `MEM` (the heap too,
  with HEAP.SYS), `EXIT` (halts —
  the one legitimate use of the EXIT opcode), and a drive letter such
  as `B:` (below). A `.BAT` name runs the batch file.
  `TYPE file /P` and `DIR /P` page: 23 lines, then `-- More --` and a
  wait. The wait is `get_line()`, which already understands that one
  `read(0, ...)` may carry several lines, so a pause eats exactly one
  and leaves the rest of a piped script for the prompt. Paging is off
  unless `/P` is given, so nothing that scripts `TYPE` changes.

### Drives, and one spelling for them

`DEVICE=DRIVES.SYS` tells DOS the machine has more than one drive. It is
announced for the same reason the clock and the console are: the drive
registers are a device window at `0x13c`/`0x188` on the board and
ordinary memory under native c4m, so a probe would be a wild read. With
the line, the prompt names the drive it is on and `B:` moves between
them:

    A>B:
    B>DIR
    B>A:
    A>TYPE B:note.txt

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
`TYPE B:note.txt`, `COPY B:x.c y.c`, `RUN B:hello.c4r` -- and that
needed no code in `dos_open`: the prefix is resolved by the disk
controller, which has understood it since M5.

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
simply a directory you `cd` into (or hand to c4bb with `-d`). `make`
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

`c4dos.dir` IS the directory service -- `DIR` types that file, because
the raw disk cannot enumerate itself -- and it doubles as the name
resolver. Every open goes through `dos_open`, which tries the name as
typed and, failing that, scans the listing for a case-insensitive match
and opens the spelling that actually exists. So `HELLO`, `hello.c4r`
and `Hello.C4r` all reach the same file, the way DOS always let them.

A command with no extension gets `.c4r` appended and retried, which is
the courtesy `COMMAND.COM` extended with `COM`/`EXE`/`BAT`: you type
the program, not the file. `RUN` does the same to its argument. A word
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
| `make test-c4dos` | four legs: native c4m, the clock build, unmodified `c4` through `c4l.c` (the purity pin: the clockless image uses no opcode above `EXIT`), and c4bb, including `c4.c4r` compiling hello inside it. Its script builds the images with `./cpp \| ./c4cc`, not the Makefile's c4lc. API v4 (`src/tests/dosexec.c`: exec three deep, a resident image, catch, a child's file, unlink, the console slots, the sink) runs on the native leg, the tower and c4bb |
| `make test-c4dos-heap` | on libjs, the Makefile's c4lc `-O` image with `HEAP.SYS`: the v4 scenarios with c4lc `-O` transients, a child's leftovers given back and its RAM-disk file kept, then Doom five times at the prompt with `HEAP_USED` within 1% (`libjs/tests/test-c4dos-heap.mjs`; Doom needs the games disk) |
| `make test-cpp` | `cpp \| c4cc` and `gcc -E \| c4cc` give byte-identical images over the corpus, and `cpp.c` interpreted by plain c4 matches the native build |
| `make test-dosload` | `dosload.c4r` uses no opcode above `EXIT` |
| `make test-ladder` | the ladder on the host: C4DOS builds C4KE, `dosload` hands over, both toolchains compile and link in the RAM filesystem. Minutes |
| `make test-c4dos-build32` | on c4bb: `BUILD`, then boot the kernel just built, to a clean shutdown |
| `make test-c4dos-ladder32` | on c4bb: `LADDER` rebuilds the compiler to a fixed point, then the kernel; boots it |
| `make test-c4dos-c4ix32` | on c4bb: `IX` builds C4IX with the compiled compiler and boots it. Minutes |
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
climb by hand is `docs/climbing-the-ladder.md`.

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
