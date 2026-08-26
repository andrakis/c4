# C4DOS — design notes

A single-tasking, trap-free disk operating system for the C4 family,
in the spirit of classic DOS: `CONFIG.SYS`, `AUTOEXEC.BAT`, a prompt,
transient programs that load, run, and return. Written for the
HOMEWARD game's bring-up campaign (the OS you can run BEFORE building
trap machinery), but a first-class citizen of this repo like every
other subsystem.

STATUS: in development, 2026-08-21. This doc is the contract; deviations
get written back here.

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
  c4dos.c            # shell: CONFIG.SYS, AUTOEXEC.BAT, prompt, builtins
  c4dos_load.c       # .c4r loader (c4ix/loader.c structure: v3 MEMSZ)
                     #   + c4l.c invoke-stub call mechanism (plain-c4 safe)
  include/c4dos.h    # transient-side API shim: exit(), dos_* services
  fs/CONFIG.SYS      # curated boot config, copied onto the disk
  fs/AUTOEXEC.BAT
  tests/build-images.sh
  tests/test-c4dos.sh
docs/c4dos-design.md # this file
```

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

## Files, writing, and the RAM disk

c4/c4m have NO file-write primitive (READ only), and c4bb's disk
controller is read-only too. Writing is therefore a DOS SERVICE: an
in-memory RAM disk (name → buffer table), DOS-style
`DEVICE=RAMDISK.SYS SIZE=n` (default 1 MB, 64 slots).

**Implemented.** `DIR` lists the union of the disk (read-only) and the
RAM disk (rw, marked `<ram>` with a usage line); opens check the RAM
disk FIRST, so a tool that rewrites a file shadows the read-only
original rather than failing. RAM files carry pseudo-descriptors above
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

c4sp/c4lc deliberately stay out of this: they only ever run under C4KE,
where the RAM filesystem is reached through custom opcodes instead, and
a trap-free DOS has none to offer.

### API versions

The table is 32 words, zeroed at boot. Slot 5 is the version.

| slot | v1 | | slot | v2 |
|---|---|---|---|---|
| 0 | magic `C4D` | | 9 | `count()` → RAM disk entries |
| 1 | `dos_exit` (reserved, always 0) | | 10 | `entname(i)` → `char *` |
| 2 | `create(name)` | | 11 | `entsize(i)` → bytes |
| 3 | `write(h, buf, len)` | | 12 | `entdata(i)` → `char *` |
| 4 | `close(h)` | | 13 | `trim()` → scratch bytes released |
| 5 | version | | 14 | `release()` → RAM disk bytes released |
| 6 | `open(name)` | | | |
| 7 | `read(h, buf, len)` | | | |
| 8 | `close(h)` | | | |

Slots 2 and 9–12 are advertised only when `DEVICE=RAMDISK.SYS` was
installed, so a caller that checks a slot is told the truth rather than
handed an always-empty listing. **Everything at 9 and above must be
gated on the version word**: a v1 DOS allocated sixteen words and
filled nine, leaving uninitialised heap where a caller would read a
function pointer. `dos_can_enum()` in the header does both checks.

v1 lets a tool ask for a file BY NAME. v2 is for a *loader*: something
taking the machine over wants everything the RAM disk holds without
being told what is on it, and then wants the memory. That is what
`src/c4ke/extensions/c4ke_dos.c` does with it.

(In HOMEWARD, persistent disk write arrives later as built hardware;
the RAM disk is honest about what the machine can do.)

DECISION (user, 2026-08-21): NO `>` output redirection. Compilers and
tools write files DIRECTLY through the API (`dos_create`/`dos_write`/
`dos_close` slots) instead of a captured console — simpler, and
anything printf-shaped can be done in software where needed (the c4lm
stdio.h vsnprintf is sitting right there when a tool wants it). Batch
stays a command list, not a shell language.

## dosload — LOADLIN for C4DOS

`src/c4dos/dosload.c`, built as `dosload.c4r`. Loads an image, hands
DOS's memory back, and jumps:

    RUN dosload.c4r c4ke.c4r -v 50

**Be clear about what it does not do.** `RUN c4ke.c4r` already works,
and DOS already injects the API into it, so a kernel started the
ordinary way can already find the RAM disk. dosload adds two things:

- **Memory.** DOS holds a 4 MB read scratch and cannot free it while
  running a program, because the loaded image's constructor and
  destructor tables point *into* that scratch. dosload copies those
  tables out and resolves them to absolute addresses first, so it can
  call `dos_trim()` before the kernel starts. It declares no
  destructors of its own — DOS walks *its* destructor table out of the
  freed scratch afterwards, and zero iterations never touch it. Do not
  add one.
- **A command line.** `RUN` passes a transient its own arguments;
  dosload passes the image name as `argv[0]` and everything after it as
  the loaded program's, so `-v 50`, `-c N` or an alternate init reach
  the kernel's `parse_commandline`.

Strict c4, nothing above the `EXIT` opcode: `make test-dosload` pins
that with `c4l.c`, which refuses an image that uses more and names the
instruction. This is a tool the player runs *at* the C4DOS rung, and
loading a kernel must not be the thing that demands a better CPU.

## CONFIG.SYS and AUTOEXEC.BAT

- `CONFIG.SYS`: `DEVICE=CLOCK.SYS` (enables TIME/C4CY use) and
  `DEVICE=RAMDISK.SYS SIZE=n`, both implemented; `FILES=n`,
  `SHELL=...` reserved.
  Parsed with a flattened-locals descent (the vfsload.c skeleton,
  c4cc-dialect).
- `AUTOEXEC.BAT`: line-per-command batch, `ECHO`, `REM`, `@` prefix,
  run through the same reader as the interactive prompt (the c4sh
  INPUT_STDIN/INPUT_STREAM split, minus its known multi-line-read bug).
- Builtins: `DIR`, `TYPE`, `RUN` (implicit for *.C4R names), `ECHO`,
  `VER`, `TIME` (wants CLOCK.SYS), `MEM`, `EXIT` (halts — the one
  legitimate use of the EXIT opcode).

## Building and running

Three images, because they answer three different questions. All go
through our own `cpp` -- raw `c4cc` skips `#` lines and would compile
both sides of the clock `#if` into one image.

| target | what it is |
|---|---|
| `make c4dos.c4r` | clockless, 64-bit. The purity pin: runs under unmodified `./c4` via `c4l.c`, which refuses any image using an opcode above `EXIT` -- `TIME` included. |
| `make c4dos-clock.c4r` | the same plus `TIME`, gated at runtime behind `DEVICE=CLOCK.SYS`. The dev loop. |
| `make c4dos32.c4r` | 32-bit clock build: the image c4bb boots, and the one an embedder wants. Also built into `src/c4bb/images/` by `build-images.sh`. |

C4DOS has no notion of a drive. It opens `config.sys`, `autoexec.bat`
and `c4dos.dir` relative to the working directory, so "the disk" is
simply a directory you `cd` into (or hand to c4bb with `-d`). `make`
assembles two, since a 32-bit machine will not load a 64-bit transient:
`c4dos-disk/` for the native runs and `c4dos-disk32/` for c4bb. Both
carry `hello.c4r` and `raycast.c4r`.

    make run-c4dos        # native c4m, clock build -- the A> prompt
    make run-c4dos-c4     # the tower: unmodified c4 -> c4l -> DOS
    make run-c4dos-bb     # the breadboard machine, interactive

`EXIT` halts. `DIR`, `TYPE`, `RUN`, `ECHO`, `VER`, `TIME`, `MEM` are the
builtins; `RUN raycast.c4r -d` is the one transient with something to
look at.

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

- `make test-c4dos`: build 32-bit images, boot under c4bb
  (`sim/cli.js`), pipe a scripted session, grep-assert (the
  test-c4bb.sh pattern); plus native `./c4m c4dos-boot` parity where
  output interleaving allows.
- Purity pin: `./c4 c4l.c c4dos.c4r` runs the CLOCKLESS build (the
  test-c4l pattern) — regression guard against extended opcodes
  creeping into DOS core.
- cpp pin: image byte-equality vs the gcc -E path over the corpus.

## The ladder (for HOMEWARD)

samples → C4DOS (this) → C4DOS runs cpp+c4cc to build C4KE → C4KE
(traps, preemption) hosts c4sp + c4lc → c4lc builds C4IX (protected
mode) → network coprocessor sub-board + CPU support → C4IX rebuilt
with networking → the time machine's control link (c4mp direction;
SMP is a stretch goal).

The DOS→C4KE rung is closed: `BUILD.BAT` compiles a kernel *and* an
init onto the RAM disk, and `c4ke_dos.c` (a kernel extension, running
at `KEXT_START`) copies the whole RAM disk into the kernel's own RAM
filesystem before the init task is created. Since `task_loadc4r` checks
that filesystem before the host, the init that boots is the one the
machine just built — it never existed as a file. The DOS RAM disk is
released immediately afterwards; it was the initrd.

Progress against the rest of the ladder is tracked in
`docs/homeward-ladder.md`.

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
