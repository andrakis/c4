# c4mpg — the memory protection guard

A fork of `c4m` that checks every memory access against a table of what the
running context is allowed to touch, and a C4KE kernel extension that keeps that
table honest across task switches.

STATUS: designed, not built. This document is the contract; deviations get
written back here. Written 2026-09-06 at the user's request, to be implemented
in a fresh session.

## Why

Four bugs in one day, all of the same shape — something wrote where it should
not have, and the damage surfaced somewhere else entirely, much later:

- **`vfsload` parsed past the end of its manifest** into uninitialised heap and
  reported a hundred lines of C source as filesystem entries. Latent for months;
  it only showed when the heap beneath it happened to hold something legible
  (`docs/dos-rung-fixes.md` F12).
- **`free(t)` on an interior pointer** into the `kernel_tasks` block, on four
  spawn-failure paths. Hands the allocator a pointer it never issued, on the
  out-of-memory path, which is the moment the machine can least afford it.
- **A task jumped into a string** and executed `0x4d492c20` — the ASCII `" ,IM"` —
  as a custom opcode.
- **A trap frame whose saved bp pointed at itself**, leaving `TLEV` restoring the
  same PC forever, uninterruptibly (the fetch guard refuses to interrupt a
  `TLEV`, so nothing could break in).

The earlier `docs/task-memory.md` investigation is the same story: a compiler
overran a 64 KB task stack, wrote into the heap blocks underneath, and surfaced
as `BAD FREE` **153 blocks and thousands of allocations later**, in a different
subsystem, as an out-of-memory that wasn't one.

None of these are hard to fix once found. All of them were expensive to find,
because the machine's memory is one flat `Int32Array` and every instruction may
touch any of it. The distance between the corrupting write and the visible
failure is the whole cost.

**Why not valgrind.** The user's point, and it is the right one: valgrind asks
"is this address inside *some* live allocation". Every one of the bugs above
writes into memory that is validly allocated — to somebody else. A task stack
overrunning into the next heap block, a kernel freeing a slot that lives inside
a legitimately allocated array, a program reading past its buffer into another
task's data: all invisible to a tool that only knows the difference between
mapped and unmapped. What is needed is **ownership**, not liveness.

## What it is

`c4mpg` is `c4m` plus a permission check on every memory-touching operation.
It is a separate binary and a separate `.c4r`, never a flag on `c4m`: the check
costs time on every load and store, and `c4m` is the thing the whole ladder is
benchmarked on (`docs/compiler-speed.md`). A guard you can leave on by accident
is a guard that quietly changes every measurement in the repo.

### The region table

One table per *context*. A region is a half-open byte range plus permissions
and an owner:

    REGION_LO      inclusive start
    REGION_HI      exclusive end
    REGION_PERM    R | W | X, bitwise
    REGION_OWNER   an opaque tag: task id, or 0 for the kernel
    REGION_NAME    char *, for the message when it trips
    REGION__Sz

Checks are on the *access*, not the pointer's provenance. `LI`/`LC` need R,
`SI`/`SC` need W, an instruction fetch needs X, and the syscalls that take a
buffer (`READ`, `PRTF`, `MSET`, `MCMP`, `MCPY`, `WRIT`) need the whole
`[buf, buf+len)` range checked, not just the first byte. **The syscall buffers
are the point.** `read(fd, buf, 262144)` into a 4 KB buffer is precisely the
`vfsload` bug, and no per-instruction check would ever see it, because the
overrun happens inside the host's `read`, not in guest code.

### Lookup cost, and why it is affordable

A linear scan per access would be unusable. Two things make it cheap:

- **A one-entry cache.** Access locality in a C4 program is extreme: a tight
  loop touches its locals and one array. Remember the last region that
  satisfied a check and try it first. Expect a hit rate in the high nineties.
- **Sorted regions and a binary search on miss.** Region counts are small —
  a task has a stack, a code area, a data area, and its own allocations — so
  the miss path is four or five comparisons.

Budget: aim for under 2x on `test-c4bb` wall clock. If it lands worse than 3x,
say so in this document rather than shipping a tool nobody will turn on. The
existing `innerbench` and boot-cycle numbers are the baseline.

## The info bit

    C4I_C4MPG = 0x100

`0x100` is the free bit between `C4I_PROT` (`0x80`) and `C4I_C4KE` (`0x200`) —
confirmed unused across `include/u0.h`, `c4m.c` and `load-c4r.c`. `c4mpg`'s
`c4_info()` ORs it in; `c4m` never does. A program asks once and adapts:

    if (__c4_info() & C4I_C4MPG) { ...register regions... }

The rule the rest of the repo already follows applies here too — **announced,
never probed** (`include/c4bb_info.h`). Nothing may detect the guard by
attempting a bad access and seeing what happens.

## The opcodes

Four custom opcodes, in c4m's extended range, all no-ops under plain `c4m` so
one binary runs on both:

| opcode | meaning |
|---|---|
| `MPG_DEFINE(lo, hi, perm, name)` | add a region to the current context, returns a handle |
| `MPG_DROP(handle)` | remove one |
| `MPG_CONTEXT(id)` | switch the active context; kernel-only |
| `MPG_QUERY(addr)` | which region owns this, or 0 — for the debugger, and for tests |

Only `MPG_CONTEXT` is privileged. A task defining regions inside its own
allotment is fine and useful; a task widening its own allotment is not, so
`MPG_DEFINE` may only ever *narrow* what the current context already holds.
That single rule is what stops the guard being self-defeating.

## The violation

A violation raises `TRAP_MPG_VIOLATION`, a new trap type alongside
`TRAP_ILLOP` / `TRAP_PM_VIOLATION`, carrying the faulting address, the access
kind, and the PC. With no handler installed, `c4mpg` prints and halts:

    c4mpg: task 7 'c4m' wrote 0x0014D2A0 (4 bytes) — outside every region it owns
    c4mpg:   nearest: 'task 7 stack' [0x0014C000,0x0014D000) rw
    c4mpg:   pc 0x00037710, 928 bytes past the end of that region

**Halting on first violation is the whole value.** The stack trace at the moment
of the bad write is worth more than any amount of forensics on the wreckage
afterwards — that is the entire difference between this and what we did today.

## The C4KE extension

`src/c4ke/extensions/c4ke_mpg.c`, registered like every other:

    kext_register("mpg", &mpg_init, &mpg_start, &mpg_shutdown);

`mpg_init` returns immediately unless `__c4_info() & C4I_C4MPG`, so a kernel
built with the extension still runs unchanged on plain `c4m`. This mirrors
`c4ke_dos.c`, which is inert when `__c4dos_api` is 0.

What it enforces:

- **Kernel memory is readable but not writable by user tasks.** `ps` and `top`
  read the exported task table; nothing else should be able to change it. Two
  regions over the same range with different permissions per context, which is
  the ordinary way an MMU expresses this.
- **Tasks cannot reach each other.** Each task's stack, code, data and argv are
  regions owned by its id, absent from every other context.
- **On trap, switch to the kernel context; on return, switch back.** The trap
  microroutine and `TLEV` are the two switch points, and they already exist as
  the only places the machine changes privilege.

### Per-task storage

`TASK_EXTDATA` plus a reserved offset, exactly as `c4ke_pm.c` does
(`kernel_pm_extdata_start`, `c4ke_pm.c:180,294`). The extension asks for its
words at init and gets a stable offset into every task structure. No change to
`c4ke.c` required.

### Patching kernel functions without touching c4ke.c

The user's proposal, and it is sound — the repo already does this twice:

- `include/c4dos.h`'s `__c4dos_stub` rewrites its own first word to a `JMP` and
  re-enters, so C can call through a variable on a machine with no indirect
  call.
- `c4m.c`'s `c4_invoke_stub` does the same for `C4IV`.

The shape for an override:

1. Read the target's first two words — an `ENT n`.
2. Write those into a trampoline: `ENT n` followed by `JMP target+2`.
3. Overwrite the target's first word with `JMP override`.
4. The override does its work, then calls the trampoline to reach the original.

**Two hazards, both learned the hard way today.** `C4IV` bit us because a stub
that jumps out of the interpreter takes its callee's syscalls with it — the
callee stops being interpreted and starts running one level up, where the DOS
RAM disk did not exist (`dos-rung-fixes.md` F7). Anything patched this way must
stay inside the same interpreter. And the fetch guard refuses to interrupt a
`TLEV` (`machine.js:150-165`), so a patch anywhere near the trap return path can
produce a hang that **no interrupt can break into** — which is exactly the
lockup in the user's screenshot.

## Testing

The guard is a measuring instrument, and the lesson from the memory census this
session is that an unvalidated instrument produces confident wrong answers —
it passed `{diskDir}` to `Devices`, which wants a drives array, so every image
ran with no disk and reported an appetite of nothing. So:

**Known-bad first.** Before it is pointed at anything real, `src/tests/mpg/`
gets programs that each commit exactly one sin, and the test asserts the guard
catches each and names the right region:

| test | what it does |
|---|---|
| `mpg_overrun.c` | writes one word past a malloc'd buffer |
| `mpg_underrun.c` | writes one word before it |
| `mpg_stackdive.c` | recurses until it passes the stack guard |
| `mpg_freed.c` | writes through a pointer after freeing it |
| `mpg_interior.c` | frees an interior pointer — the `free(t)` bug, verbatim |
| `mpg_syscall.c` | `read(fd, buf, huge)` into a small buffer — the vfsload bug |
| `mpg_foreign.c` | (under C4KE) writes into another task's stack |
| `mpg_kernel.c` | (under C4KE) writes into the exported task table |

**Known-good second, and this half is not optional.** Every one of these must
run to completion with **zero** violations, or the guard is crying wolf and
will be turned off within a day:

    make test-c4l  test-cpp  test-link  test  test-c4dos  test-c4bb

**Then the real prize.** Point it at the two bugs whose root causes we already
know, and confirm it names them at the moment of the write rather than
afterwards: the `vfsload` manifest overrun (revert F12 locally), and
`innerbench -n 50` at `-m 128`.

**A regression pin.** `make test-mpg` runs the known-bad set and asserts the
exact region name and offset in each message, so a later change that makes the
guard vaguer fails the build.

## Milestones

- [ ] **M0** This document, reviewed.
- [ ] **M1** `c4mpg.c` forked from `c4m.c`, region table, checks on `LI`/`LC`/
      `SI`/`SC` only, `C4I_C4MPG` reported. No C4KE. `mpg_overrun` and
      `mpg_underrun` caught; every existing test still green.
- [ ] **M2** Syscall buffer ranges (`READ`, `PRTF`, `MSET`, `MCMP`, `MCPY`).
      `mpg_syscall` caught — the vfsload class.
- [ ] **M3** The four opcodes, the narrowing rule, `TRAP_MPG_VIOLATION` with the
      full message. `mpg_freed`, `mpg_interior` caught.
- [ ] **M4** `c4ke_mpg.c`: per-task regions from `TASK_EXTDATA`, context switch
      on trap and `TLEV`. `mpg_foreign`, `mpg_kernel` caught. **No change to
      `c4ke.c`** — if one turns out to be unavoidable, write down why here
      before making it.
- [ ] **M5** Performance measured against the `test-c4bb` and boot-cycle
      baselines, recorded in this file whatever it says.
- [ ] **M6** Aimed at the known bugs; `make test-mpg` pinned.

## What this is not

- **Not a security boundary.** It is a debugging instrument for a machine with
  one user. A task that wants to escape it can.
- **Not on by default.** `c4m` stays exactly as fast and exactly as permissive
  as it is today. Nothing in the ladder's rung budgets moves: `c4mpg` is a host,
  not an opcode a program needs.
- **Not a replacement for the stack guard.** `kernel_stack_check`'s poisoned
  words (`c4ke.c:1789`) catch overruns at every context switch and cost nothing;
  they stay.

## Open questions

1. **Where do a task's own `malloc`s become regions?** The kernel cannot see
   them — `kernel_task_malloc` exists but is called only by itself, and its one
   real caller is `c4ke_pm.c`, which is compiled out (`CONFIG_ENABLE_PM = 0`).
   Either `MALC` grows a region automatically in `c4mpg`, or the extension has
   to intercept it. The former is simpler and does not depend on a shelved
   extension; prefer it unless it proves wrong.
2. **What owns firmware memory on c4bb?** `fw.c` allocates from the same arena
   and predates any context. Probably one region owned by 0, defined at boot.
3. **Does the region table live in guest memory or host memory?** Host is
   simpler and cannot itself be corrupted by the bug being hunted — which is a
   strong argument, given that a guard living in the memory it protects is a
   guard that fails exactly when it is needed.
