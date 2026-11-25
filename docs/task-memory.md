# Task memory in C4KE: what a task owns, and who gives it back

## Why this document exists

`b4ke -f c4ix.b4k` builds C4IX under C4KE and stopped at eight of twelve
modules with

    ;; c4lc: src/c4ix/sched.c - 19126 bytes - sched.c4o
    c4sc-host: out of memory copying string
    b4ke: 8 ran, 0 skipped, 1 failed

and did so identically at `-m 256`, `-m 512` and `-m 1024`, to within
thirty thousand cycles out of 1.59 billion. `docs/compiler-on-the-board.md`
blamed `src/c4ke/extensions/c4ke_pm.c:106` — a protected task's untracked
`MALC`. **That was wrong**, and the correction is the first thing here:
`CONFIG_ENABLE_PM` is `0` (`include/c4ke/config.h`), so that file is
not compiled into the kernel at all and no `MALC` has ever passed through
it.

## M0 — what actually happens (measured, not reasoned)

`src/c4bb/fw/fw.c` was temporarily given three diagnostics: report the
free list when an allocation fails, validate the free list on every
malloc/free, and walk the heap block chain when a `free()` arrives with
an implausible header. The failing run then said:

    fw: BAD FREE at op 40689: ptr 0x6185c header size -1342175720
    fw:  walk stopped at 0x61450 after 153 blocks (size -1879046636)
    fw:   -4: kind 2 ptr 0x71c24
    fw:   -3: kind 2 ptr 0x71b84
    fw:   -2: kind 2 ptr 0x71bd4
    fw:   -1: kind 2 ptr 0x6185c

Three facts fall out of that:

1. **The free list was intact until this `free()`.** The per-op validator
   never fired. So nothing was racing it, and the firmware's
   interrupt-masking (`fw.c:46-62`) was doing its job.
2. **The heap itself was already corrupt**, 153 blocks in, at `0x61450` —
   a kilobyte *below* the pointer being freed, and a quarter of a
   megabyte into a heap that runs to `0x10000000`. Something had written
   over block headers in a low, early, long-lived region.
3. **It is not a leak.** A leak runs out of *far* memory; this failed
   with a quarter-gigabyte free and only ever touched the bottom of it.

The region is a **task stack**. `src/c4ke/c4ke.c`, as it was:

        TASK_STACK_SIZE = 0xFFFF,  // How much stack memory to allocate to tasks.

64 KB — 16,383 words on the 32-bit board — for every task, `ls` and a
recursive-descent compiler alike. `c4sc` is c4lc's Lisp transliterated to
C: recursion is its only control structure for tree walks, and `sched.c`
at 26,582 bytes is the largest module in C4IX. It ran off the bottom of
its stack and kept pushing, straight into the heap blocks underneath.
The `BAD FREE` is the *second-order* symptom — a block header eaten by a
stack that had already left the building.

This also explains the one fact that made no sense before: **the failure
is insensitive to machine size** because a task's stack is a fixed 64 KB
whatever `-m` says.

Valgrind on the native `c4sc-host` compiling the same module confirms the
other half: no invalid free, no overlap, 2,535 allocations and 1,515
frees. The compiler's own memory discipline is fine. It just needs a
stack.

## What this changes about the plan

Two separate pieces of work, and only the first one is a bug:

- **The stack** is a correctness problem with the worst possible failure
  mode: silent heap corruption that surfaces thousands of allocations
  later, in a different subsystem, as an out-of-memory that isn't one.
  A task must not be able to do that, and when it overruns it must say so.
- **Task allocations** are a resource problem. C4KE frees everything *it*
  allocated for a task (`kernel_clean_task`, `c4ke.c:1400` — stack, code,
  data, argv, signal handlers, the C4R, the ext data) and nothing the task
  allocated for itself. Twelve compiler runs in one session therefore
  leave twelve arenas behind. That is what stands between this build and
  32 MB.

## Milestones

- [x] **M0** This tracker, with the diagnosis above, before any code.
- [x] **M1** A task stack big enough for the compiler, and an overrun
      that is *reported* rather than silently written into somebody
      else's heap. `b4ke -f c4ix.b4k`: **13 ran, 0 skipped, 0 failed**.
- [x] **M2** The honest baseline: **128 MB**. 64 MB fails.
- [x] **M3** Our own hygiene. `gc_shutdown`, `pr_shutdown`,
      `atoms_shutdown`, `stdlib_shutdown`, and a `main` in c4sp and c4sc
      that is a wrapper so every exit path passes through them. Valgrind
      on a real compile: **0 bytes in use at exit**, 1,587 allocs against
      1,587 frees.
- [x] **M4** The kernel tracks a task's allocations and reclaims them
      when the task ends, whether it exited or was killed.
      `make test-task-mem`: 72 MB of deliberate leak completes in a
      32 MB machine, and the same test on a kernel without protected
      mode fails, which is what makes the pass mean something.
- [x] **M5** The whole C4IX build under C4KE in 32 MB — **it fits in
      13**.
- [x] **M6** Without protected mode, on libjs: each program task gets a
      heap generation of C4DOS's `HEAP.SYS`, released when the task is
      reaped. Doom three times under C4KE leaves `HEAP_USED` at `c4sh>`
      where the first round did (`make test-libjs-ladder`).

---

## M1 — a task stack the compiler fits in, and an overrun that says so

Two changes in `src/c4ke/c4ke.c`:

**The size.** `TASK_STACK_SIZE` 0xFFFF → 0x40000 (64 KB → 256 KB). 64 KB
is 16k words on the 32-bit board; `c4sc` compiling `sched.c` needs more
than that and there is nothing pathological about it — a recursive
descent parser plus a tree walker plus a peephole fixpoint, all in one
call chain.

**The guard.** Every stack is now allocated with `TASK_STACK_GUARD` = 256
poisoned bytes underneath it (`TASK_STACK_POISON`, "WASH" — what is left
when the tide goes out). `kernel_stack_check` verifies the poison on the
**outgoing** task at every context switch, because `kernel_before_switch`
runs after that task's registers are saved and before
`kernel_task_current` moves on; and again in `kernel_task_finish`, for a
task that overran and then exited without being switched out once more.
The guard is 256 bytes rather than one word because a C4 frame is entered
by a single `ENT` that moves `sp` by the whole local count at once — a
one-word canary can be jumped clean over.

An overrun now prints, once per task, and again at shutdown at any
verbosity:

    c4ke: task N (name) OVERRAN ITS STACK: U of 262144 bytes, guard broken.
    c4ke: the heap below 0xB is no longer trustworthy.
    c4ke: WARNING: 1 task(s) overran their stack this session.

Two new task fields carry it: `TASK_STACK_LOW` (the lowest `SP` seen,
sampled at each switch — not exact, but enough to size a stack by, and
printed per task at `-v 100`) and `TASK_STACK_BROKEN` (so the message
appears once, not once per switch).

**What that `U` was not (2026-10-24).** `U` was `SP` at the moment of the
check, and the check runs at a switch or at exit, so it is not how deep
the stack went. `44 of 262144 bytes, guard broken` was read as a stray
write more than once ([dos-rung-fixes.md](dos-rung-fixes.md),
[next-session.md](next-session.md)). When `make test-c4bb-climb` failed
with exactly that line, it was c4sc recursing 4,671 frames deep in c4lc's
`g:resolvecode` on `sched.c`, running through the guard and 125 KB on
into the shell's code, and coming back before any switch saw it.
`g:resolvecode` loops now. The report says what the guard and the stack
actually show:

    c4ke: task 6 (overrun.c4r) OVERRAN ITS STACK of 262144 bytes: 10 of 64 guard words broken.
    c4ke:   its stack is used to the bottom: it ran off it (a recursion deeper than the stack, most likely)
    c4ke:   found as it exited, with 44 bytes in use then -- which is not how deep it went.
    c4ke: the heap below 0x71cac is no longer trustworthy.

or, for one word written into the guard from `main`,

    c4ke: task 6 (overrun.c4r stray) OVERRAN ITS STACK of 262144 bytes: 1 of 64 guard words broken.
    c4ke:   its stack was never used below 392 bytes: a stray write from elsewhere, most likely

A stack is zeroed when its task starts, so the lowest word that is not
zero is the task's true high-water mark, between checks or not
(`kernel_stack_peak`). A stack used to within a guard's length of its
bottom ran off it. A guard broken under a stack that never got near it
was hit by something else. The `-v 100` line at exit ("used at most")
reads the same high-water mark now, not `TASK_STACK_LOW`.
`src/c4ke/bin/overrun.c` breaks its own guard both ways, and test-c4bb.sh
checks that the report tells them apart and that the shell survives both.

`src/c4bb/fw/fw.c` gained the other half of the diagnosis: an allocation
that fails now says how much heap was actually left.

    fw: malloc(3200000) failed: 260 free blocks, 2889208 bytes, largest 2883224

A full heap and a heap eaten by a stack overrun have nothing in common as
bugs, and telling them apart was previously guesswork.

**Bar: `b4ke -f c4ix.b4k` builds all twelve modules and links `c4ix.c4r`.**

    b4ke: C4IX built
    b4ke: 13 ran, 0 skipped, 0 failed
    c4bb: 3891060156 cycles in 257.26s (15125k inst/s), 0 missed traps, status 0

---

## M3 — what we allocate and do not give back

C4KE frees everything it allocated *for* a task and has no way to know
about anything the task allocated *for itself*. On a host that never
mattered: the process exits and the operating system reclaims the lot.
As a task it matters completely, and `fw`'s new diagnostic says so in one
line — at `-m 32` the eighth module dies with the heap genuinely empty:

    fw: malloc(3200000) failed: 260 free blocks, 2889208 bytes, largest 2883224

That is not corruption. That is eleven arenas that nobody asked for.

So c4sp and c4sc now hand everything back before they return:

- `gc_shutdown()` (`src/c4sp/include/gc.h`) releases every arena block,
  every string buffer a dead cell still owns, the mark and worklist
  arrays, the extra-roots list and the global-environment index.
- `pr_shutdown()` (`read.h`) the 64 KB print buffer.
- `atoms_shutdown()` (`atoms.h`) the atom table and every interned name.
- `stdlib_shutdown()` (`stdlib.h`) the path buffer.

`main` in both `src/c4sp/c4sp.c` and `src/c4sc/body.h` became a four-line
wrapper around the old body — renamed `c4sp_run` and `sc_run` — so that
all fifteen-odd `return`s pass through one place that does the releasing,
rather than each error path having to remember.

Measured with valgrind on the native compiler, same module, before and
after:

    before   in use at exit: 8,640,572 bytes in 1,020 blocks
             total heap usage: 2,535 allocs, 1,515 frees
    after    in use at exit: 0 bytes in 0 blocks
             total heap usage: 1,587 allocs, 1,587 frees

Zero. Not "small enough" — zero.

---

## M2 / M5 — the number

The whole C4IX build under C4KE (`b4ke -f c4ix.b4k`, twelve modules and
the link), against `-m`:

| machine | before | after M3 |
|---|---|---|
| 128 MB | 13 ran, 0 failed | — |
|  64 MB | fails: `malloc(292566) failed: 639 free blocks, 336048 bytes` | — |
|  48 MB | — | 13 ran, 0 failed |
|  32 MB | fails at module 8 | 13 ran, 0 failed |
|  24 MB | — | 13 ran, 0 failed |
|  20 MB | — | 13 ran, 0 failed |
|  16 MB | — | 13 ran, 0 failed |
|  14 MB | — | 13 ran, 0 failed |
|  13 MB | — | **13 ran, 0 failed** |
|  12 MB | — | fails: `malloc(1600000) failed: 419 free blocks, 2265448 bytes` |

**128 MB to 13 MB**, and the target was 32. The cycle count is identical
at every size that completes — 4,297,435,923 — so nothing about the
result depends on how much memory the machine has, which is what you want
from a build.

---

## M4 — the kernel tracks what a task allocates

M3 is a program being tidy. This is the kernel not having to trust it.

**Where it hooks.** A task's `malloc` is a `MALC` opcode the VM executes
directly; the kernel never sees it — unless the task runs *protected*, in
which case `MALC` and `FREE` trap and land in `pm_dispatch_run`
(`src/c4ke/extensions/c4ke_pm.c`), where the TODO has been sitting since
2025:

        } else if (ins == MALC) {
                // TODO: record allocation details somewhere...

That is now `kernel_task_malloc` / `kernel_task_free` /
`kernel_task_realloc` / `kernel_task_release` in `c4ke.c` — in the kernel
proper rather than the extension, because *what a task owns* is kernel
policy and `kernel_clean_task` is what has to empty the list.

**The layout.** Four words in front of the pointer the task gets: a
magic, the size, and the task's doubly-linked list. `kernel_clean_task`
walks the chain and frees it, so a task gets its memory back whether it
exited tidily, exited badly, or was killed.

**Three things had to be true first**, and none of them were:

1. **`FREE` cannot assume there is a header.** A protected task can
   perfectly well free something it did not get from `MALC` — u0's atexit
   table is allocated by a *constructor*, which the loader runs, and
   released by a *destructor*, which the task runs. Peeking four words
   back at such a pointer is an out-of-bounds read and then a guess. So
   the kernel keeps a set of the pointers it handed out, the same
   open-addressed shape as c4m's own `c4_mt_` table (`c4m.c:1150`),
   tombstones included. A pointer that is not in it is freed exactly as
   given.

2. **A task is the loader before it is the program.** `task_loadc4r` runs
   as the task, at the task's privilege, and allocates the image, its
   relocated code and data, and the task's name — all of which
   `kernel_clean_task` already frees. Tracked as well, they were freed
   twice, which is the `free(): invalid size` this work spent an
   afternoon on. A task now runs **unprotected until it enters the
   program's main**, at the new `OP_TASK_EXEC` opcode: the loader is the
   kernel, the program is the user, and only the program's allocations
   are the program's. The mode change takes effect immediately because
   `mode` is part of the trap frame `TLEV` restores.

3. **c4lc could never compile a protected-mode kernel.** `c4ke_pm.c`
   declared `static int OPEN, READ, CLOS, ... MALC, FREE ...` over the
   names load-c4r.c's opcode enum already owns. c4cc allowed it; c4lc
   said `bad lvalue: num` — assigning to a number — and refused the
   file. Since the board's kernel is built by c4lc, protected mode was
   unreachable there entirely. The variables are now `pm_OPEN` and so on.

Also: `RALC` is guarded in protected mode by `c4m.c:1922` and was missing
from the board's `PM_GATED` set, so a protected task's `realloc` trapped
on one host and not the other. It is in both now, and `pm_dispatch_run`
handles it — c4m's own `c4_realloc` refuses a pointer it never handed
out, and so does this.

**Bar: `make test-task-mem`.** `src/tests/test_leak.c` allocates 6 MB in
each of twelve tasks and frees none of it, on purpose, and each round
reports how much it got:

    leak: round allocated 24 of 24 blocks      (x12)
    test_leak: 12 rounds done

on a **32 MB** breadboard — 72 MB of leak in a machine a third that size.
The control is part of the test, because otherwise the pass proves
nothing: the same test on a kernel built without protected mode must
fail, and does.

    fw: malloc(262144) failed: 5 free blocks, 269792 bytes, largest 206256
    leak: round allocated 23 of 24 blocks
    test_leak: round 5 only got some of its memory

Valgrind on the native run: **0 errors from 0 contexts**, 2,220 allocs
against 2,208 frees, 82 MB allocated, 405 KB in use at exit — all of it
c4m's own.

## M6 — without protected mode: heap generations (libjs, 2026-10-26)

M4 needs protected mode, because without it a task's `MALC` never
reaches the kernel. On libjs the heap can do the bookkeeping instead:
it tags every block with the **generation** current when it was
allocated, and one store to `HEAP_RELEASE` frees a whole generation
(`libjs/heap.js`, `HEAP_GEN` 0x1C4 and `HEAP_RELEASE` 0x1C8 in
`src/c4bb/sim/devices.js`). C4DOS already gives each program one
(`DEVICE=HEAP.SYS`, [c4dos-design.md](c4dos-design.md)); C4KE now does
the same for its tasks. On the Build Ladder each Doom run under C4KE
used to keep about 14 MB until C4KE stopped.

**Announced, then checked, never probed.** Under native c4m 0x1C4 is
ordinary memory. The `dos` extension asks C4DOS's API (v4, slot 26,
`gen(0)`): it answers non-zero only when `HEAP.SYS` was installed and
answered, and the image running then (dosload, or C4KE itself under
`RUN`) has a generation of its own. `kernel_heapgen_start` then reads
`HEAP_GEN` — the kernel's own generation, which must not be 0 — and
checks a value written reads back, as C4DOS does. c4bb has the
registers but no heap behind them, and its DOS never announces them.

**The kernel's memory stays the kernel's.** The kernel's generation is
the one it was started in; nothing in C4KE releases it (C4DOS does, when
dosload returns). `HEAP_GEN` holds a task's generation only while that
task's own code runs:

- every way into the kernel switches to the kernel's generation —
  `trap_enter` (every opcode handler and `signal_forwarder`) and
  `ih_cycle`;
- `trap_exit`, the way out, and the end of `ih_cycle` switch to the
  generation of whichever task they resume, the kernel's for a task
  without one.

So a RAM-filesystem file a task writes (`ramfs_put`, in a trap), its
inbox, its name, `kboot`'s command line: all the kernel's, and they
outlive the task.

**A task is the loader before it is the program** (M4's point 2 again).
The generation is given at `OP_TASK_EXEC`, so the image, its code and
data, the stack and the name, which the loader allocates, are the
kernel's, freed by `kernel_clean_task` as before. A task that protected
mode runs gets none: the kernel already tracks what it allocates.

**Released last.** `kernel_clean_task` writes `HEAP_RELEASE` after its
own frees, so nothing it frees explicitly is freed twice. At shutdown,
once every task is cleaned, `HEAP_GEN` goes back to the kernel's and the
feature off: what runs after (`kboot`'s C4IX) is the kernel's.

**Numbers.** Task generations count up from `'K','E' << 16`
(1,262,813,184), clear of C4DOS's, which count from 1.

**What it turned up.** With the program's memory gone, what was left was
the kernel's own, and it still grew by 364 bytes, six blocks, a run. Two
leaks, both on every program start, both older than any of this:

- `c4r_load_opt_real` (`load-c4r.c`) returned 0 on a failed `open()`
  with its read buffer, the C4R structure, the header and the file name
  still allocated: 349 bytes. `task_loadc4r` tries the bare name before
  the one with `.c4r`, so this was the usual path, not a rare one.
- `task_loadc4r` freed its copy of the command line and the `.c4r` name
  after `main` returned. A program that calls `exit()` never comes back
  there, and Doom, like every bare-machine game, does
  (`__c4ke_exit`). Both are freed before the program starts now.

**And one in the way.** c4cc read at most 256 KB of source into its
512 KB source area, and the preprocessed kernel was already at 259,027
bytes: these changes took it past, and c4cc reported a missing
parenthesis wherever the read had stopped. It reads to the area's size
now, and says so when sources do not fit.

**Bar.** `test-c4dos-ladder.mjs` plays Doom under C4KE three times and
checks `HEAP_USED` at `c4sh>` is the same after each round:
4,848,431 bytes after each of the three, where each round used to add
about 14 MB. The whole climb (`make test-libjs-ladder`), C4KE's b4ke
build of C4IX included, now holds at most 45 MB of the arena, not 61.

## What protected mode costs, and why it is still not the default

The dispatcher thread in `c4ke_pm.c` exists because C4KE has no streams:
IO from several tasks at once would interleave into nonsense, so a
syscall sets a wait state, switches to `kernel/io`, is serviced there,
and switches back. That is the right shape for a `printf`. For a
`malloc` it is two context switches and a scan of the task table for an
operation that is a few dozen instructions — and a compiler allocates
thousands of times per module. One C4IX module, compiled on the board:

| kernel | one module | all twelve and the link |
|---|---|---|
| no protected mode | 125,323,714 | 4,297,435,923 |
| protected, memory syscalls through the dispatcher | did not finish in eight minutes | — |
| protected, memory syscalls serviced immediately | 142,487,206 — **+13.7%** | 4,869,355,456 — **+13.3%** |

The whole-build figure is `b4ke -f c4ix.b4k` at `-m 32` on a kernel built
with `CONFIG_ENABLE_PM=1`: **13 ran, 0 skipped, 0 failed**. Protected
mode builds C4IX, tracking every allocation every task makes, for one
part in eight.

So `MALC`, `FREE` and `RALC` are now always serviced in the trap
handler, whatever the task's exclusivity: they neither print nor block,
which is the only reason the dispatcher was ever involved.

**`CONFIG_ENABLE_PM` still defaults to 0.** Not because the tracking
does not work — `make test-task-mem` says it does — but because turning
it on changes the protection model for every task and every syscall, not
just the two this needed, and that is a bigger decision than this piece
of work. It is now one flag, it is measured at +13.7% on a compile, and
`make test-task-mem` builds its own kernels with it on so the feature
stays honest whether or not it is switched on.

The thing that actually got the build into 32 MB was M3: **we were the
ones not freeing what we allocated**, exactly as suspected.

## What is deliberately not here

- **`ps` still reports `MEM` as gross allocation when protected mode is
  off**, because with it off nothing tells the kernel about a `free`.
  With it on the figure is now real: `kernel_task_free` subtracts.
- **C4DOS returns a transient's memory only on libjs** (`HEAP.SYS`).
  On the board it still does not, which is the reason `IX.BAT` needs
  128 MB (`docs/compiler-on-the-board.md`); and the same goes for M6.
- **The stack size is one number for every task.** A compiler needs
  256 KB; `ls` needs a few hundred bytes. `TASK_STACK_LOW` is the
  measurement that a per-task stack size would be chosen from, and it is
  now recorded, but nothing reads it except the `-v 100` report.
- **The dispatcher thread still scans the whole task table per syscall.**
  Moving the memory syscalls off it was enough to make protected mode
  usable; the IO path was left alone.
- **There are two copies of `include/c4ke/config.h`** — the other is
  `src/c4ke/include/config.h`, which nothing on the include path
  reaches. Both were changed so they stay identical, which is not the
  same as fixing it.
