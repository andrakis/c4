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
- **C4DOS still does not return a transient's memory.** That is the same
  feature one rung down and the reason `IX.BAT` needs 128 MB
  (`docs/compiler-on-the-board.md`). Nothing here touched it.
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
