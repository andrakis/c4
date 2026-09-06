# The DOS rung: six fixes, then four more

Tracker for six papercuts found driving C4DOS/C4KE by hand, 2026-09-05. Plan
agreed with the user before any code was written; this file is the contract, and
deviations get written back here.

**Tick a box only on green evidence** — a command that ran and what it printed.

## The six

| # | What | Where |
|---|---|---|
| F1 | c4bb has no run command; serving the board UI is an undocumented `python3 -m http.server` | `Makefile`, `src/c4bb/package.json` |
| F2 | `make run-c4dos-build` boots a C4KE with no userland — `ls` is not on the floppy and cannot be built from `c4ke-src.tar` | `Makefile` |
| F3 | The games (`mandel`/`rps`/`raycast`) ship on the 32-bit climb disk only | `Makefile` |
| F4 | C4DOS's `A>` never flushes, so you cannot tell it is ready | `src/c4dos/c4dos.c` |
| F5 | u0/libc4ix programs trap-storm and segfault under C4DOS instead of declining | `include/u0.h`, `src/c4dos/c4dos.c`, `src/c4ix/lib/libc4ix.c` |
| F6 | `type -P` hard-errors "not implemented"; `-p` is parsed but never pauses | `src/c4ke/bin/type.c`, `src/c4dos/c4dos.c` |

## Round two (2026-09-05, after the first six landed)

| # | What | Where |
|---|---|---|
| F7 | `c4m load-c4r.c -- c4ke` could not see the C4DOS RAM disk | `c4m.c`, `Makefile` |
| F8 | c4bb's board and terminal split was fixed, so a full screen of output scrolled | `src/c4bb/web/*` |
| F9 | Black scan lines between every row of coloured terminal output | `src/c4bb/web/style.css` |
| F10 | The speed slider's minimum was still too fast to watch | `src/c4bb/web/app.js` |
| F11 | c4m had no clock on the breadboard, so a guest waiting on time waited forever | `c4m.c`, `src/c4dos/c4dos.c`, `include/c4dos.h` |
| F12 | vfsload parsed past the end of its manifest into whatever the heap held | `src/c4ke/bin/vfsload.c` |

### F7 — the VM's syscalls, and the invoke that escaped them

Two bugs stacked, and the first hid the second.

**The syscalls.** A program running inside c4m reaches the world through the
`OPEN`/`READ`/`CLOS` opcodes, and those went straight to the host — so
`load-c4r.c`, running under c4m, could not see a kernel `BUILD` had just written
to DOS's RAM disk. Patching load-c4r.c would not do: it is compiled from source
at runtime and has no symbol section for `inject_api` to reach. Fixing it in the
**VM** fixes it for every program c4m will ever run, and none of them need to
know. `c4m_open`/`c4m_read`/`c4m_close` ask DOS first (DOS checks RAM before the
real disk, so host files still resolve), guarded by `#if C4M_DOS` — defined only
for the raw-`c4cc` C4DOS-rung builds, which get `include/c4dos.h` prepended.
Native `./c4m` and the C4KE `c4m.c4r` go through gcc/`gcc -E` and are byte for
byte what they were. Still base-c4: c4dos.h reaches the API through its invoke
STUB, not an indirect call.

**The invoke.** That alone changed nothing, and the reason took a while to find:
`c4r_load` routes through `c4r_load_opt_pure` whenever `__c4_info()` reports
`C4I_C4`, and that calls `__c4_invoke` — the `C4IV` opcode. `C4IV` rewrites
c4m's OWN code to jump at the target, so the loader stops being something c4m
interprets and becomes something the machine runs directly. Its `open` then
happened one level ABOVE the only code that knows what a RAM disk is. The
evidence was a probe that opened the same file by the same name and got fd 1000
from two lines away, while `c4r_load` got -1 and c4m's handler never fired at
all. Fix: when DOS is present, emulate `C4IV` as a **JSR** — push the return
address, jump, let the callee's `LEV` come back — which is what the `#else`
branch there has had a `TODO` asking for. Only when DOS is actually there, so a
c4m on bare hardware still gets the stub it always had.

### F11 — a clock for the VM, borrowed from the OS

`c4_time()` in `c4m.c` is a `/proc/uptime` reader -- a Linux fallback, and the
reason the C4KE build works is simply that C4KE is hosted on Linux. There is no
such file on a breadboard, and the failure path sets `c4_time_unavailable` and
returns **0 for the rest of the run**. C4KE's boot measures instructions per
second by watching the clock advance, so it never got past that step: not slowly,
never. The symptom was one line nobody was reading -- `c4m: unable to open uptime
file` -- printed immediately before the hang.

**The clock is borrowed from DOS, not taken from the machine.** C4DOS already has
one: `__time()`, the TIME opcode, serviced by the board's microcode or by a native
c4m. c4m could call it directly -- and must not. **TIME is opcode 53, above EXIT,
and `c4m-dos32.c4r` is pinned in `RUNG_BASE`** so the campaign can say the VM needs
no opcode the machine did not boot with. Moving it to the `dos` rung would rewrite
the ladder's dependency graph to fix a bug.

So the clock comes through the API table, which costs the caller nothing above
EXIT (the invoke stub is base-c4):

- **`src/c4dos/c4dos.c`**: `dos_api_time()` on **slot 15**, advertised only when
  `DEVICE=CLOCK.SYS` was installed and the build has the hardware -- the honesty
  rule the RAMDISK slots already keep. API version -> **3**.
- **`include/c4dos.h`**: `dos_can_time()` / `dos_time()`, gated on BOTH the version
  word and the slot, because a v1 DOS allocated sixteen words and filled nine, so
  slot 15 on one of those is uninitialised heap rather than a zero.
- **`c4m.c`**: `c4_time()` asks DOS first, under `#if C4M_DOS`, then falls through
  to the `/proc/uptime` reader exactly as before.

`opscan -rung base c4m-dos32.c4r` still passes with exactly the 39 base opcodes.

### F8/F9/F10 — the board in the browser

- **A draggable splitter** between board and terminal, stored as a FRACTION in
  `localStorage` (so it still means the same thing in a resized window or on
  another monitor), double-click to reset. The canvas uses `object-fit: contain`,
  so dragging SCALES the hardware map rather than cropping it or handing it a
  scrollbar. `#layout` is now exactly `100vh`.
- **The scan lines were `line-height: 1.4`.** Coloured output is inline `<span>`s
  with a background colour, and an inline box paints only its CONTENT box — the
  font's ascent+descent — not the line box. That left 0.4em unpainted between
  every row. At `line-height: 1` the content boxes meet and the colour is
  continuous.
- **The speed slider went below one microstep per frame.** Its floor was 1/frame
  = 60/s, far too quick to follow a control line. The scale is now
  `10^(0.062v - 1.2)`, which is ~3.8 microsteps/s at 0 and 10^5 at 100 — the top
  end went UP, because widening a log scale at one end must not quietly narrow
  it at the other. Fractional rates need an accumulator. The slider also shows
  the rate it is asking for, because "35" tells you nothing.

## Round three (2026-09-06)

| # | What | Where |
|---|---|---|
| F13 | the shared disk's `c4m.c4r` was built through `gcc -E`, compiling the DOS branch OUT | `src/c4bb/tests/build-images.sh` |
| F14 | `install.lst` never carried `src/c4ke/c4ke.c`, so innerbench worked on a directory disk and failed on a medium | `src/c4dos/fs/install.lst` |
| F15 | no way to change the browser's arena, and the RAM label said 32MB while the arena was 128 | `src/c4bb/web/app.js` |
| F16 | `A>` prompt but `install 1:` -- two vocabularies for one idea | `src/c4dos/c4dos.c` and 16 other sites |
| -- | THE MEMORY CENSUS: the ladder's memory axis, never measured | `src/c4bb/tools/memcensus.mjs` |

**F13 is the one worth remembering.** F7 gave c4m the DOS API behind `#if C4M_DOS`,
which raw `c4cc` switches ON by skipping `#` lines. `build-images.sh` builds the shared
disk's c4m through `$PREPROC` (gcc -E), which HONOURS the `#if` and compiles the branch
out -- so the fix worked on the build floppies I tested and not on the medium a player
boots. Two builds of the same source, one of them silently without the feature. The
climb disk already copied `c4m-dos32.c4r`; the shared disk now builds the same way.

**F16 turned out to be a naming problem, not a machinery one.** `Devices.resolveDrive`
has accepted `0:` and `A:` alike since M5 -- its own comment says *"C4DOS already thinks
in drive letters"* -- so cross-drive prefixes needed no code in `dos_open` at all, and
`install`'s `drivenum()` already took letters. What was missing was C4DOS knowing which
drive it was on. It does now, behind `DEVICE=DRIVES.SYS` (announced, never probed: those
registers are a device window on the board and ordinary memory under native c4m).

### The census, and two bugs in it

`opscan.mjs` measured the opcode axis; nothing measured memory, so nine `-m` values and
`C4IX_CELLS` were numbers arrived at by trying them until they stopped failing. Modelled
on Homeward's `scripts/opcensus.ts`. Measured, at last:

| image | data | machine stack | note |
|---|---|---|---|
| `c4ke32.c4r` | 5,931K | 420B | capped at 800M cycles |
| `c4ix32.c4r` | 17,002K | 280B | capped at 800M cycles |
| `hello32.c4r` | 28K | 228B | returned |

The tiny stack figures are correct: C4KE gives each task a `kmalloc`'d stack, so task
depth lands in **data**, which is the column that sizes `-m`. "capped" is the normal
outcome for a kernel that boots to a shell and waits.

Both of my own bugs in it are worth naming, because a measuring tool that is wrong is
worse than none: it passed `{diskDir}` to `Devices`, which wants a **drives array** and
ignored it silently, so every image ran with no disk and "returned" in a few thousand
cycles reporting an appetite of nothing; and the formatter rounded sub-kilobyte readings
to `0K`, which made a correct 236-byte measurement look like a dead instrument. Neither
threw. Both produced a confident table.

### Arena sizes, for the record

Three different defaults, and I had them wrong in one direction while the user had them
wrong in the other:

- **c4bb/web: 128 MB**, and was before any of this work. What said 32 was the label in
  `board.hwd`, stale since the arena was raised. Now generated from the real size.
- **cli.js: 32 MB.**
- **Homeward's oracle: 32 MB** (`opts.arenaBytes ?? 32 * 1024 * 1024`).

And a finding that outlives this tracker: **Homeward's vendored c4bb is 21 commits
behind** (`// ported from c4/src/c4bb/sim/machine.js @ af9f0af`), with 260 lines
differing in `devices.js`, 81 in `microcode.uc`, and no PIT at all. That file calls c4bb
"the behavioral oracle". Nothing in this tracker has reached it.

## Round four (2026-09-06, second session): was F13 a regression?

The previous session handed over one question: **F13 changed how the shared disk
builds `c4m.c4r`, and raw `c4cc` compiles both arms of every `#if` — is the
`innerbench -n 50` lockup mine?** It is not. What it is instead is worse, older,
and in the kernel.

| # | What | Where |
|---|---|---|
| F17 | C4KE dispatched a custom opcode by indexing `custom_opcodes` with **no bounds check**, then jumping to whatever word it read | `src/c4ke/c4ke.c` |
| F18 | the board's line discipline released **more than one line** per canonical read, so every command after the first in a burst was thrown away | `src/c4bb/sim/devices.js` |
| -- | F13's real (different) cost: the disk's c4m silently lost `u0.h`, `c4.h` and `c4m_float.h` | `src/c4bb/tests/build-images.sh` |

### F13 is not the regression — measured both ways

Two disks, identical but for `c4m.c4r`; `innerbench -mlqT -n 50` at `-m 128`
(`-l` runs the precompiled `c4ke.c4r` instead of recompiling `c4ke.c`, which is
the same stress in seconds rather than the 24 minutes `-n 5` took):

| c4m.c4r built by | `Custom opcode not found` | what it did instead |
|---|---|---|
| `$PREPROC c4m.c` (pre-F13) | **11**, tasks 9 through 31 | went silent and spun |
| `c4cc32 c4dos.h c4m.c` (F13) | **0** | 1708 honest `malloc failed` lines |

So the crash cluster the handoff pointed at is *pre-existing* and belongs to the
u0-linked c4m. The F13 build cannot exhibit it for a reason nobody intended:

**Raw `c4cc` skips `#include` as well as `#if`.** It is not that both arms get
compiled — it is that the headers never arrive. Comparing the two builds'
function sets, F13's `c4m.c4r` is missing every one of u0's ~55 definitions
(`vprintf`, `snprintf`, `strlen`, `memmove`, `calloc`, `rand`, the default
signal handlers, `__u0_ops_init`, the vfs calls), plus `c4_float_instruction`
from `c4m_float.h`. The image is 24KB smaller, which was the first clue.

Measured from inside the machine, with a two-line guest that prints
`__c4_info()`:

- pre-F13: **242** = `C4M | HRT | SIG | FLT | PROT`
- F13: **131** = `C4 | C4M | PROT`

F13 cost the disk's c4m its floating point, its high-resolution timer and its
signals, and turned **on** the `C4I_C4` bit — the very bit `build-images.sh`'s
own comment worries about, because `c4r_load` picks `c4r_load_opt_pure` whenever
`__c4_info()` says `C4I_C4`. Every nested load now takes a different path.

### Settled: use one of our own preprocessors

The user's answer, and it is the right one: **we have preprocessors — use one.**
`./cpp` (src/c4dos/cpp.c) is a real preprocessor, it is ours, it is the same one
`BUILD.BAT` runs inside the machine, and it is pinned byte-identical against
`gcc -E` over the corpus. "Compile it with the compiler that ignores the
question" was never an answer to the question.

And the user's other point is the one that explains how this happened at all:
**c4m.c was originally written so that no `#else` or `#elif` was ever needed.**
Every conditional ADDED something harmless, so a compiler with no preprocessor
at all — which skips every `#` line and therefore compiles every arm — still
produced a working program. That discipline had drifted. F11's DOS clock check
was written INSIDE the `#if C4_ONLY` arm, so it existed only in builds that had
no preprocessor; every build that had one evaluated `#define C4_ONLY 0`
honestly, took the other arm, and quietly had no DOS clock. Whether c4m can ask
DOS the time has nothing to do with whether it was compiled against a host libc.

So, three things:

- **`c4_time()` is now one function outside the split**, with the DOS check as
  an additive `#if C4M_DOS` block at the top and `c4_time_host()` (the old
  /proc/uptime reader, or `c4m_time()` in the native arm) underneath. The rule
  is written into the comment: an `#ifdef` may only ADD something harmless,
  never pick between two spellings of the same thing.
- **The knobs are knobs.** `C4_ONLY` is `#ifndef`-guarded so a build can ask for
  it; `C4M_NO_U0` suppresses the u0 include, because u0 declines under C4DOS by
  design (F5) and that has to be a decision; `C4M_FREESTANDING` asks for the
  no-headers build.
- **Every build of `c4m.c` now goes through a preprocessor.** The shared disk's
  (`build-images.sh`), and both `c4m-dos` rules in the Makefile.

Measured after: `__c4_info()` back to **242**, and the clock moves
(`t0=7 t1=8`) both standalone on the board and as a task under C4KE.

**`C4M_FREESTANDING` is not a tidiness flag, and the rung census is why.** Give
`c4m-dos32.c4r` a real preprocessor and it stops being a base-rung image:

    c4m-dos32.c4r: ABOVE RUNG base -- uses MCPY (42) at code+1435 and 1 more

The tree's own `include/string.h` DEFINES `memmove()` in terms of `memcpy()`
under `__c4cc__`, and `include/c4ke/opcodes.h` defines `c4ke_opcode()` in terms
of `__c4_opcode()`. Both are dead code in c4m and both are compiled in anyway,
and `MCPY` (42) and `OPCD` are above the base rung this image is pinned to — so
the DOS rung's VM would have needed a CPU the player has not extended yet. It
was inside its rung only because raw c4cc never opened a header. With
`-DC4M_FREESTANDING=1 -DC4_ONLY=1 -DNOT_NATIVE=1` it is inside again, with the
same opcode set as before and 63 bytes smaller (the dead `if (0)` arm of `C4IV`
goes). `test-c4bb-rungs` green.

That census earned its keep here. It is the only thing in the tree that would
have noticed, and it noticed immediately.

### F17 — the wild jump under every one of these lockups

`trap_handler`, on `TRAP_ILLOP`:

    handler = (int *)(*(custom_opcodes + (ins - CO_BASE)));

`custom_opcodes` holds `CO_MAX` (128) entries. `ins` is whatever the faulting
task raised. There was no range check — the one that belongs there was written
and then commented out, three lines below, where it had sat long enough to be
furniture. So an out-of-range opcode read a word from arbitrary heap, and if
that word was non-zero the kernel did `__c4_adjust(handler[-1] * -1)` and
`__c4_jmp(handler)`: a stack adjustment and a jump, both taken from data.

And the opcodes really are out of range, because they are **pointers**. A
two-line probe (`__c4_opcode(name, 9001)` for an opcode nobody installed) shows
where they come from: `__c4_opcode` leaves its last-evaluated argument in `a`
when a request is not serviced, `__u0_ops_init` stores whatever comes back, and
u0 then spends the rest of the run raising a string address as an opcode. That
is the whole of `c4ke: Custom opcode not found: 108138136` — and the reason the
values marched down by one allocation stride per task.

The probe also showed the damage was not confined to the offender:

    probe: good  name=6270592 -> 134
    probe: bad   name=6270636 -> 0
    c4ke: Custom opcode not found: 9001, executed by task 6
    c4ke: Custom opcode not found: 50276, executed by task 5   <-- c4sh

Task 5 is the shell. It had raised nothing; the kernel's jump had walked
through it. **That is the user's report exactly** — a command works, the next
one does not, and the whole system stops.

With `if (ins >= CO_BASE && ins < CO_BASE + CO_MAX)` in front of the read, the
same probe kills the offender and leaves task 5 alone, and the same
`innerbench -n 50` goes from 11 wild opcodes to 2 — with a real stack trace
attached (`c4m.c4r:calloc()+0x6064 / main()+0x30`) instead of the previous
`could not find function entry`. Diagnostics that were being eaten now print:
`task 33 OVERRAN ITS STACK: 44 of 262144 bytes, guard broken` — 44 bytes used
and the guard broken is not an overrun, it is a wild write, and it was invisible
before.

**How much of that is proof, exactly.** The bug itself is not in question: an
unchecked out-of-bounds read feeding an indirect jump is wrong on inspection,
whatever it does on a given day. The run-to-run numbers above are one run each
of a chaotic system and should be read as consistent-with, not as a measurement.
The part that is not luck is qualitative: after the check, stack traces resolve
and whole classes of diagnostic (the stack-guard reports) appear that had never
printed before.

**`badop.c4r` is a smoke test, not a regression test**, and it was checked
against a kernel with the bounds check taken back out to make sure of that. On a
quiet boot the word at the out-of-range index is zero, so the unfixed kernel
behaves identically. Whether the read hurts depends on what the heap holds,
which is exactly why this only ever bit a machine full of live tasks — and
exactly why no cheap test was ever going to find it. What `badop` does pin is
the kill path: the offender dies, the trace names `badop.c4r:main()`, the shell
survives, and the session shuts down of its own accord.

The installer had the matching off-by-one: `opcode <= CO_BASE + CO_MAX` let an
extension write `custom_opcodes[CO_MAX]`, one word past the block. Now `<`.

**Still open at `-n 50` / 128 MB:** the machine exhausts its heap and spins with
no way back. That is a resource limit meeting a system that cannot report it and
stop; it is no longer memory corruption.

### F18 — the line discipline that handed out two lines at once

`kbdRead` breaks at the newline only `if (!nonblock)`. c4sh opens `/dev/stdin`
**non-blocking**, reads 99 bytes, keeps what precedes the first newline and
discards the rest of the buffer. So any input that arrives in a burst — a paste,
a scripted session, a front-end that delivers a whole line at once — loses every
line after the first, and the shell sits with an empty queue looking exactly
like a hang. The comment directly above the function already promised the
opposite: *"does not release ANY of it to a reader - blocking or non-blocking -
until Enter or EOF"*. It now breaks `if (!raw)`, so canonical readers get one
line per read and `/dev/tty` still gets whatever has arrived.

`printf 'ls\necho second\n\\q\n' | cli.js -d images/disk c4ke32.c4r` used to
run `ls` and then hang to the cycle budget; it now runs all three and shuts down
cleanly in 350ms.

**Why no test caught it:** every C4KE leg in `test-c4bb.sh` types exactly one
command and then `\q`. There was no two-command test to fail.

### Method note: an instrument, validated first

Four hours went into "lockups" last session that were a harness typing into a
void. The same trap was waiting here in a different shape — prefeeding a pipe
runs afoul of F18, so a *correct* machine looked hung. `drive.mjs` (paced writes
to `cli.js -i`, output streamed to a file as it arrives, never buffered) was
checked against a known-good four-command session **before** any conclusion was
drawn from it. Everything above rests on it.

## Decisions taken with the user

- **Guard C4DOS on both sides only.** u0 programs refuse under C4DOS, C4IX programs
  refuse under C4DOS. C4IX-app-under-C4KE is deliberately left unguarded: probing by
  name is unsafe because C4IX's own `sched_trap` forwards C4KE-range opcodes to
  `ck_dispatch` (`src/c4ix/sched.c:224-250`), so the probe would answer wrongly on the
  very system it is meant to run on. Doing it properly needs a `__c4ke_present` symbol
  injection mirroring C4DOS's `inject_api`; not in scope here.
- **Paginate in both** `type.c` and C4DOS's `TYPE`.
- **No `DOSBOOT` builtin.** `dosload c4ke.c4r` already works through `dispatch()`'s
  `prog_ext()` fall-through (`c4dos.c:904-912`).
- **The prompt is announced, not probed** — see F4.

---

## F1 — a run command for c4bb

The page must be served **from the repo root**: `web/app.js` fetches drive and image
paths that are repo-root-relative, so a server rooted at `src/c4bb/web/` breaks the
drives panel.

- [x] `serve-c4bb` target in the Makefile, `C4BB_PORT ?= 8471`, echoing the full URL
- [x] `.PHONY` updated
- [x] `src/c4bb/package.json` gains a `scripts` block delegating to make (keep
      `"type": "module"` — it is what marks `src/c4bb/**/*.js` as ESM)
- [x] `src/c4bb/README.md` + `docs/c4bb-design.md` mention it, keeping the manual
      `python3` line as the no-make fallback

---

## F2 + F3 — everything back on the `c4dos-build` floppy

**Why copying is enough:** `task_loadc4r` (`src/c4ke/c4ke.c:2905-2931`) searches
ramfs `name` → ramfs `name.c4r` → host `name` → host `name.c4r`, and
`run-c4dos-build` runs with cwd set to the floppy (`Makefile:472`). A `.c4r` dropped
in `c4dos-build/` is loadable by the booted kernel with no kernel change. That is
already how `c4sh.c4r` is found in this flow.

Modelled on the `c4dos-c4ix32` target (`Makefile:565-599`), which does all of this at
32 bits.

- [x] New 64-bit rules `c4-dos.c4r`, `c4m-dos.c4r`, `mandel-dos.c4r`, `rps-dos.c4r`
      (twins of `Makefile:418-428`, `$(C4CC)` instead of `./c4cc32`).
      `raycast-dos.c4r` already exists. These are the `u0lite`/bare builds, so they
      need no opcode above `EXIT` and run at **both** rungs
- [x] Userland onto the floppy: `ls ps top cat echo type xxd kill spin c4le`, copied
      from `$(C4KE_BIN)` / `$(C4R_TOP)` — **not** the repo root, whose copies only
      exist after `pre` has run
- [x] Benchmarks: `$(BENCHS)` — `bench`, `benchtop`, `innerbench`
- [x] innerbench's inputs by the exact names it opens
      (`src/bench/innerbench.c:254,265,272`): `load-c4r.c`, `c4m.c`, `c4.c`, and
      `src/c4ke/c4ke.c` in a `src/c4ke/` subdirectory, plus `c4.c4r`/`c4m.c4r`
- [x] Games: `mandel.c4r`, `rps.c4r`, `raycast.c4r` from the `-dos` builds
- [x] `vfsload.c4r` (64-bit, see below) + `src/c4bb/fs/c4ke-build.vfs.txt` copied on
      as `c4ke.vfs.txt`, replacing the repo-root manifest that names dozens of files
      this floppy does not carry
- [x] Prerequisites updated; `c4dos.dir` regeneration stays **last**
- [x] `$(C4DOS_BUILD_DISK32)` (`Makefile:478-498`) extended the same way

### vfsload at 64 bits — the one genuinely new build

`ls` lists the kernel ramfs, not the host directory (`src/c4ke/bin/ls.c:1-6`), so
without `vfsload` the new programs would run but not appear in `ls`.

- [x] `vfsload.c4r` rule via `$(C4SPLC)` + `c4lc.lisp`, mirroring `raycast-dos.c4r`
      (`Makefile:368-370`). Needs c4lc's real block scoping (c4cc will not do) and
      must be built **without** `u0.h` (`Makefile:2875`)

**Riskiest item in the plan.** If the 64-bit c4lc build does not come out clean, fall
back to shipping the userland without `vfsload` — the programs still run via the host
fallback — and record that outcome here rather than forcing it.

---

## F4 — a prompt you can see, announced not probed

**Runtime detection of c4bb is not available to C4DOS.** The only probe in the tree is
`bb_has_clock()` in `include/c4bb_info.h`, and that header says why it cannot be used:
it asks `__c4_info()`, **`INFO` is opcode 57**, and c4cc compiles every function in a
header whether it is called or not. Including it would push `c4dos.c4r` above the
base-c4 rung and break the purity pin. That header also states the doctrine:
*"The capability is announced, never probed."*

The real distinction is not "c4bb" but **whether the console flushes a partial line**.
Natively `PRTF` bottoms out in the host libc, line-buffered on a TTY, so `"A>"` sits in
the buffer until the next newline — that is the bug. c4bb's UART emits each byte as it
is written, so `A>` shows immediately there and the classic DOS look is worth keeping.

- [x] `g_conflush` global + `DEVICE=CONSOLE.SYS` clause in `read_config`, beside the
      `CLOCK.SYS` and `RAMDISK.SYS` clauses
- [x] Prompt at `c4dos.c:1076` becomes `printf(g_conflush ? "A>" : "A>\n")`.
      `run_batch`'s echo (`:947`, already `"A>%s\n"`) untouched
- [x] Default is the newline; the tight look is an explicit opt-in
- [x] The c4bb disks opt in: `src/c4bb/images/disk/config.sys`, and the
      Makefile-built `c4dos-disk32`, `c4dos-build32`, `c4dos-c4ix32`

**This is why CONFIG.SYS is the right route: it keeps the test suite untouched.**
`test-climb.sh`'s `idle()` asks "is the prompt the last thing in the log" via
`tail -c 400 | grep -qe "$1\$"`, and its comment depends on today's behaviour: *"The
prompt is the last thing in the file and carries no newline."* It runs on c4bb against
`c4dos-c4ix32`, which opts in, so `A>` stays un-newlined and `idle()` needs no change.
A blanket `"A>\n"` would have broken that helper subtly, by letting a **stale** `A>`
line inside the 400-byte tail satisfy the match and typing into a busy machine.

---

## F5 — `C4DOS: This application requires C4KE.`

`__u0_init` (`include/u0.h:644`) fires ~38 `OPCD` (opcode **48**) symbol requests
before `main`, at a kernel that is not there. C4DOS installs no trap handler by design,
so each prints `Trap type TRAP_ILLOP ... instruction 48` / `c4m: missed a trap`,
leaves 128 in the accumulator, and the program limps on to a segfault. The intended
guard is already written and disabled at `include/u0.h:649-651` (`// TODO: this check
is failing`) — it fails because plain `__c4_info()` never sets `C4I_C4KE`.

**The probe** is the loader's own symbol patch: `c4r_load` calls `inject_api`
(`c4dos.c:545`) **before** `run_program` runs the constructors (`c4dos.c:583`), so
`__c4dos_api` is set under DOS and 0 everywhere else. Costs **no opcode above `EXIT`**,
which matters because `__c4_info()` is itself an `OPCD` call.

- [x] `include/u0.h`: `int *__c4dos_api;` global + magic `4404292`, checked at the top
      of `__u0_init` before `__u0_ops_init()`, returning non-zero
- [x] `src/c4dos/c4dos.c` `run_program` (`:579-590`): a constructor returning non-zero
      aborts the load — `main` never runs, destructors are skipped. Today the return
      value is **discarded**, and there is no escape hatch (`g_api[1]`, `dos_exit`, is
      permanently 0 at `c4dos.c:1041`), so this is what makes the guard bite
- [x] `src/c4ix/lib/libc4ix.c`: same guard in `libc4ix_init()` (`:495`), message
      `C4DOS: This application requires C4IX.`; `static void` becomes `static int`
- [x] `src/c4bb/images/c4ke-root/u0.h` re-synced (currently byte-identical)

---

## F6 — real pagination

### `src/c4ke/bin/type.c`

- [x] `-P n` implemented (consumes the next argv word; implies `-p`), replacing the
      `"-P not implemented"` bail at `:96`
- [x] `-p` actually pauses — `type_file()` (`:20-56`) never reads `opt_page` today
- [x] Options parsed in a full pass **before** any file is typed (`:80-102` types
      files during the scan, so options after a filename apply too late). Fixes
      `endopts`, declared and initialised but never set — `--` currently ends only the
      current cluster, because the inner-loop flag `endopt` is a different variable
- [x] The wait uses the non-blocking console pattern proven under C4KE in
      `src/tests/raycast.c:328-378` (`/dev/tty`, then `/dev/stdin`, `O_NONBLOCK`) with
      a `schedule()` yield, honouring that file's rule: *"Never fd 0: a read there
      blocks the HOST, which stops the whole VM and every other task with it."*
      Blocking `read(STDIN, ...)` only as a last resort
- [x] Three adjacent defects: `show_help` (`:7`) `printf("%s: ...")` with no `argv0`;
      `argv0` used uninitialised in the malloc-failure message (`:63`, assigned `:67`);
      `if (!(fd = open(file, 0)))` (`:24`) treats fd 0 as failure and misses negatives

### C4DOS `TYPE`

- [x] `cmd_type` (`:677-681`) takes argc/argv and recognises `/P` (and `-P`)
- [x] `type_file` (`:594-605`) gains a page-size parameter; 0 means no paging.
      `DIR /P` falls out for free
- [x] **`get_line()` moved above `type_file()`.** This dialect is define-before-use and
      `get_line` is at `:608`, *after* `type_file` at `:594`, so `type_file` cannot call
      it as the file stands. The move is mechanical (only `g_inbuf`/`g_inlen`/`g_inpos`)
      but getting it wrong fails in c4cc with a confusing message
- [x] Paging off unless asked, so piped sessions in `test-c4dos.sh` are unaffected

---

## Verification

- [x] V1 `make test-c4l`, `make test-cpp`, `make test-link`,
      `bash src/c4dos/tests/test-c4dos.sh`, `make test`
- [x] V2 The reported bug end to end: `make run-c4dos-build` → `A>` visible → `c4sh`
      declines cleanly with no trap chatter → `BUILD` → `dosload c4ke.c4r` → `ls`,
      `innerbench`, and the games all work. Games at the DOS rung too (`A>mandel`,
      `A>rps`, `A>raycast`)
- [x] V3 Pagination: `type -p`, `type -P 10`, plain `type` unchanged; `TYPE x /P`,
      `DIR /P`
- [x] V4 Opcode rung not raised: `opscan.mjs -needs base` on the new `-dos` builds;
      `./c4 c4l.c c4dos.c4r` still prints no `needs XXXX`
- [x] V5 Both prompt paths (native newline, c4bb tight), then `make test-c4bb`,
      `make test-c4dos-build32`, `make test-c4dos-ladder32`,
      `bash src/c4bb/tests/test-climb.sh`
- [x] V6 `make serve-c4bb` + `npm start` from `src/c4bb/`: drives panel populates

## Green runs, 2026-09-05

| gate | result |
|---|---|
| `bash src/c4dos/tests/test-c4dos.sh` | **OK** — all 10 legs, including the ladder, the tower, and `c4-inside-DOS-inside-c4bb runs hello.c` |
| final regression pass, after the last floppy change | `test-c4dos-build32: OK` (exit 0) and `test-c4dos: OK` (exit 0) |
| `make test-c4bb` | **OK** — 24 legs incl. C4IX boot and `c4dos boots the shared disk` |
| `make test-c4dos-build32` | **OK** — 316 M cycles, 15.03s |
| `make test-c4dos-ladder32` | **OK** — `c4cc is a fixed point at 218124 bytes; both kernels are 204758 bytes` |
| `make test-c4l` / `test-cpp` / `test-link` | **OK** |
| `./c4 c4l.c c4dos.c4r` | clean — no `needs XXXX`, the base-c4 purity pin holds after the `run_program` and `TYPE` edits |

Both floppies carry 41 files. After the final pass added `tar`, `c4rlink` and `b4ke`
-- so the kernel this floppy builds can unpack its own sources and link objects, which
is the shape the rung above needs -- the booted system reports
`vfsload: 55/55 entries loaded` with **zero** "cannot open" lines, and `ls` shows 38
entries.

**`innerbench` runs on the built kernel.** `innerbench -n 1` reports
`invoking 'c4m load-c4r.c src/c4ke/c4ke.c -- -v 0 bench'` -- i.e. it found `c4m.c4r`,
`load-c4r.c` and `src/c4ke/c4ke.c` on the floppy by the exact names it opens them with
-- runs a whole nested C4KE compile, and finishes with `innerbench complete`.
That was the specific thing that could not work before.

## Docs to update alongside

- [x] `docs/compiler-on-the-board.md:631-634` — the "vfsload is missing from the
      floppy … not fixed here" note is resolved by F2
- [x] `docs/c4dos-design.md` — `DEVICE=CONSOLE.SYS FLUSH` in the CONFIG.SYS section;
      `TYPE ... /P` and `DIR /P` in the builtin list; `make serve-c4bb` in the
      building-and-running table; the build floppy now carries a userland
- [x] `src/c4bb/README.md`, `docs/c4bb-design.md` — `make serve-c4bb`
- [x] `include/u0.h` header comment and `include/u0lite.h`'s "Use ONE of these, never
      both" — u0 programs now say so under C4DOS rather than crashing

## Round two, verified

Driven over raw CDP to the **Chromebook** (192.168.1.220, `ssh -N -L 9223:127.0.0.1:9222`)
with the page served through the code-server proxy, because the desktop CDP was in
use by another session. Playwright's `connectOverCDP` times out enumerating targets
on that box even at 90s -- a raw CDP client over the websocket connects instantly and
is the way to drive it.

| check | result |
|---|---|
| F7 native | `c4m load-c4r.c -- c4ke` -> `Kernel ready`, `C4SH - The C4 SHell` |
| F7 on c4bb | `c4m load-c4r.c -- c4ke` reaches `c4ke: Kernel ready in 790ms`, init and the VFS -- see F11, which is what made that possible |
| F7 rung | `./c4 c4l.c c4m-dos.c4r` clean -- no `needs`, still base-c4 |
| F8 layout | page does not scroll; drag 518 -> 330px board, terminal 535px, canvas scaled to 328px, fraction `0.378` stored and restored across reload; double-click resets |
| F9 scan lines | mandel renders 631 coloured spans with `line-height: 15px` at `font-size: 15px`, `scrollHeight === clientHeight` (635) -- the picture is continuous and needs no scrollbar |
| regressions | `test-c4dos` OK (exit 0) · `make test` exit 0 · `test-c4dos-build32` OK · `test-c4bb` OK (41 legs, exit 0) — the c4m.c change is invisible to the native VM and the C4KE build, which go through gcc/`gcc -E` where `C4M_DOS` is undefined |
| F10 speed | slider 0/10/25/48/75/100 -> 3.8/s, 16/s, 134/s, 3.6k/s, 169.1k/s, 6.0M/s. Measured: 6 seconds at minimum advanced **3 instructions** (cycle 86 -> 89) |

### F12 — vfsload parsed past the end of its manifest

`read_file()` in `src/c4ke/bin/vfsload.c` never NUL-terminated its buffer, and its
only consumer walks it with `while (*p)` and never looks at the length it was
handed:

    if (!(buf = malloc(READ_CAP))) { close(fd); return 0; }
    n = read(fd, buf, READ_CAP);
    *plen = n;                                  /* no terminator */
    ...
    void split_lines (char *raw) { ... while (*p) { ... } }   /* ignores *plen */

So the parser ran off the end of the manifest into uninitialised heap. Booting C4KE
nested under c4m on c4bb -- right after BUILD had unpacked `c4ke-src.tar` and
compiled a kernel -- the bytes past the manifest were **C source**, and vfsload
reported `too many entries, dropping free(custom_opcodes);` about a hundred times.
Natively the next byte happened to be zero, so the same bug read as a clean
`55/55 entries loaded`. **Pre-existing and latent on both**; the nested path only
made it reachable and gave it a dirtier heap to run into.

Fixed with `malloc(READ_CAP + 1)` and `buf[n] = 0`.

**How I got this wrong first.** I had ruled out DOS name resolution (a transient on
the same board opens the manifest correctly), a stale RAM disk (`dos_api_release`
zeroes `g_ram_n`) and the ordinary `dosload` route (zero such lines) -- and then
reached for a mechanism I had not verified: that C4KE's preemption raced two tasks
over the invoke stub's single `__c4dos_slot`. That is impossible, and the user said
so: **C4KE's preemption is implemented BY c4m's dispatch loop**, so a guest task
cannot interleave inside a c4m C function like `c4m_open`. Three eliminations do not
license a guess about the fourth; the answer was in the one function in the middle
that I had not read.

Verified on c4bb, nested: `too many entries` count **0**, `Kernel ready in 790ms`,
`vfsload: 55/55 entries loaded from c4ke.vfs.txt`, `C4SH`.

## Found on the way

- **I MISREAD A STOPPED CLOCK AS A SLOW MACHINE.** When the nested boot on c4bb did not
  finish, I measured it at 40 billion cycles, concluded "four levels of interpretation
  is a speed wall", and wrote that down as a limitation. It was not. The user read the
  one line I had dismissed as a harmless warning -- `c4m: unable to open uptime file`
  -- and identified it: c4m's clock is a `/proc/uptime` reader, there is no such file
  on a breadboard, and on failure it latches `c4_time_unavailable` and returns **0
  forever**. A guest waiting for the clock to move waits forever, so no cycle budget
  would ever have been enough. That is F11. The lesson is the cheap one: a warning
  printed at the exact moment a thing stops working is not incidental, and "it is just
  slow" is a conclusion that needs a mechanism before it is written down.

- **`make test-c4bb` was not self-sufficient.** `build-images.sh:56` copies
  `c4th32.c4r` onto the fused-opcode disk, but `c4bb-images` never named it as a
  prerequisite -- so the target only worked when some earlier build had happened to
  leave that artifact behind. `*.c4r` is gitignored, so on a fresh tree, or any tree
  where it has since been cleaned away, you get `cp: cannot stat 'c4th32.c4r'` and a
  failed suite with **zero** OK lines. It bit here mid-session; I could not establish
  what removed the file (nothing in the Makefile deletes it), which is itself the
  point -- the target should not depend on luck. Fixed by adding `c4th32.c4r` to
  `c4bb-images`.

- **Self-inflicted, worth remembering: do not rebuild a disk directory under a running
  machine.** A `rm -rf c4dos-build && make c4dos-build` while a PTY session was mid-run
  left that session's cwd pointing at an unlinked inode, so every later relative `open()`
  in it would fail. The run had to be thrown away and repeated. Same lesson as
  "never edit served files during a shot run" in the sibling repos.

- **The guard found a genuinely mis-built image**, which is the interesting kind of test
  failure. `src/c4bb/tests/build-images.sh` built the shared disk's `c4.c4r` **with
  u0.h** (`$CC -o $DISK/c4.c4r $U0 c4.c`) even though **C4DOS boots that disk too** —
  shipping `c4dos32.c4r` there is the stated point of it. It only ever "worked" because
  a missed trap on the board is *silent* (`machine.js missedTrap` counts and says
  nothing) and `c4.c` never calls a C4KE service anyway: the 38 failed opcode requests
  were invisible, not absent. With the guard in place it declined outright and
  `test-c4dos.sh` leg 4 caught it — `FAIL c4bb: no evidence c4.c4r ran hello.c`.
  Fixed at the source: that image is now built from unadorned `c4.c`, matching
  `c4m.c4r` on the very next line (already bare) and the Makefile's own
  `c4-dos32.c4r`. `RUN c4.c4r hello.c` now prints `yello` and `exit(0) cycle = 22`
  inside 500 M cycles. This is exactly what `include/u0lite.h` exists to prevent, and
  the wrong build is now loud instead of quiet.

- **PRE-EXISTING TEST ROT, fixed.** `test-c4dos.sh`'s ladder leg asserted
  `dostar: extracted 39 files`, but `c4ke-src.tar` has held **42** since
  `include/c4bb.h`, `include/c4bb_info.h` and `include/u0lite.h` landed —
  `git merge-base --is-ancestor` confirms the assertion was written before those
  headers were added, and nothing in this change touches `C4KE_KIT` or `include/*.h`.
  It had rotted once before (38 -> 39). Replaced with a **floor** (`>= 39`) in the same
  idiom as the `seeded N/N` check just below it, so it still catches "the unpack
  quietly stopped working" without breaking every time the kernel grows a header.
- **F2/F3 green 2026-09-05**, driven over a real PTY (a pipe does not work: C4DOS reads
  stdin 4096 bytes at a time, so anything typed after `dosload` is swallowed by DOS's
  buffer before the kernel takes over). Boot `c4dos-build`, `BUILD`, `dosload c4ke.c4r`:
  `c4cc: wrote 390514 bytes to ram:c4ke.c4r`, `c4ke: seeded 45/45 file(s)`,
  `vfsload: 49/49 entries loaded from c4ke.vfs.txt` — **no "cannot open" lines**, which
  is what the floppy-shaped manifest is for. Then `ls` -> 35 entries, `ls /bin` -> 23
  programs, `ls /home/user` -> mandel, raycast, rps. Clean shutdown.
- `make test-c4dos-build32: OK` (316 M cycles, 15.03s) with the extended 32-bit floppy.

- **REGRESSION I INTRODUCED, then fixed.** The first cut of the F5 guard made *any*
  non-zero constructor return abort the program. `c4cc.c4r` has one constructor, and a
  c4cc-compiled constructor with no return statement returns **whatever was left in the
  accumulator** — so c4cc silently stopped running. It showed up two steps later as
  `BUILD` printing "Compiling the kernel (this is the slow part)..." and then producing
  no kernel, and `dosload: file not found: c4ke.c4r`. Fixed by making the refusal a
  **distinguished value**, `C4DOS_REFUSE = 1127037010` (`'C','4','D','R'`), defined in
  `src/c4dos/c4dos.c` and mirrored in `include/u0.h` and `src/c4ix/lib/libc4ix.c`.
  Ordinary constructors are unaffected again.
- **A rebuilt library needs its dependents rebuilt.** After changing the sentinel,
  `c4ix-uhello.c4r` still carried the old `return 1` from `libc4ix.c4l` and trap-stormed
  even though the library was correct. `make c4ix-uhello.c4r` fixed it — worth knowing,
  because the failure looks exactly like the guard not working.
- **`atoi_check` is an error code, not a boolean.** `ATOI_OK` is **0**, so
  `if (!atoi_check(...))` is inverted and rejects every valid number. Cost one build
  cycle in `type.c`.
- **`page_fd` had to be initialised to -1 explicitly.** Globals start at 0 and 0 is
  STDIN, so `if (page_fd >= 0) close(page_fd)` on a non-paging run would have closed
  the shell's own stdin.
- **The 64-bit `vfsload.c4r` built cleanly** — the plan's riskiest item. Same recipe as
  `build-images.sh` uses at 32 bits (gcc -E, then c4lc, because c4lc's own preprocessor
  hangs on u0.h). Proven working: booting C4KE from the repo root now reports
  `vfsload: 49/88 entries loaded from c4ke.vfs.txt` and `ls` lists 21 entries.
- **opscan cannot check the 64-bit `-dos` images** — it refuses anything but 32-bit
  (`c4bb is a 32-bit machine`). The 64-bit equivalent is `./c4 c4l.c <img>`, which
  refuses any opcode above EXIT and names it. Result: `c4-dos`, `c4m-dos`, `rps-dos`
  clean; `mandel-dos` and `raycast-dos` need **TIME**, which matches the existing ix-disk
  comment ("raycast and mandel want TIME, to time themselves") and is fine because the
  build floppy boots the clock build.

- **F5 green 2026-09-05.** `A>c4sh` now prints `C4DOS: This application requires C4KE.`
  and returns to the prompt — no `Trap type TRAP_ILLOP`, no `missed a trap`, no
  segfault, session exits 0. `A>c4ix-uhello` prints
  `C4DOS: This application requires C4IX.` the same way. `c4rdump -s` confirms
  `__c4dos_api` exports as **class 131 (Glo)** in both a c4cc build (`c4sh.c4r`) and a
  c4rlink'd one (`c4ix-uhello.c4r`), which is what `inject_api` requires.
  Inert under C4KE: the kernel boots, init runs, `C4SH - The C4 SHell` comes up.
- `printf` is safe in the libc4ix guard: libc4ix defines only `uprintf`/`ufprintf`, so a
  bare `printf` compiles to the **PRTF opcode (33)**, base-c4, not the write()-over-
  syscall formatter. Had it been the latter the guard would have trapped on its own
  error message.
- Booting C4KE from the repo root reproduces the F2 gap independently:
  `lc4r: unable to open 'vfsload' or 'vfsload.c4r'`.

- **F4 green 2026-09-05.** Native (no `CONSOLE.SYS` line), `cat -A` shows `A>$` — the
  prompt on its own line. With `DEVICE=CONSOLE.SYS FLUSH` appended, the same image
  prints `A>C4DOS version 0.1$` — tight, exactly as today. Purity pin intact:
  `./c4 c4l.c c4dos.c4r` runs clean, no `needs XXXX`.
- `c4dos-c4fc` turned out to be a c4bb disk too (`run-c4dos-c4fc` drives it through
  `sim/cli.js`), so it opts in as well — four Makefile disks, not three.
  `c4dos-c4th` is native and does not.

- **F1 green 2026-09-05.** `make serve-c4bb` and `npm start` (from `src/c4bb/`) both
  return 200 for `web/index.html`, `web/app.js` and `images/c4ke32.c4r` — the last of
  those is the one that proves the repo-root serving root, since it is fetched by a
  repo-root-relative path.
