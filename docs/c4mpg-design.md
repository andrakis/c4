# c4mpg — the memory protection guard

A fork of `c4m` that checks every memory access against a table of what the
running context is allowed to touch, and a C4KE kernel extension that keeps that
table honest across task switches.

STATUS: **M1, M2, M5 and M3's trap half built and green, 2026-09-06.** `make test-mpg`. This document is
the contract; deviations get written back here, and two have been — see
"Deviations, and what M1 measured" below.

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
| `mpg_unterminated.c` | `printf("%s", buf)` where buf has no NUL — its root cause |
| `mpg_foreign.c` | (under C4KE) writes into another task's stack |
| `mpg_kernel.c` | (under C4KE) writes into the exported task table |

**Known-good second, and this half is not optional.** Every one of these must
run to completion with **zero** violations, or the guard is crying wolf and
will be turned off within a day:

    make test-c4l  test-cpp  test-link  test  test-c4dos  test-c4bb

**Pointed at real workloads so far (M1/M2):** the whole `src/tests` corpus, 73
programs, byte-identical to plain `c4m`; and `c4mpg c4.c c4.c hello.c` — the
guard hosting c4 hosting c4 hosting a program — clean. Feeding it a
preprocessed `c4m.c` fails at `540: close paren expected`, identically under
plain `c4m`, so that is c4m's own limit and not the guard's.

**Then the real prize.** Point it at the two bugs whose root causes we already
know, and confirm it names them at the moment of the write rather than
afterwards: the `vfsload` manifest overrun (revert F12 locally), and
`innerbench -n 50` at `-m 128`.

**A regression pin.** `make test-mpg` runs the known-bad set and asserts the
exact region name and offset in each message, so a later change that makes the
guard vaguer fails the build.

## Milestones

- [ ] **M0** This document, reviewed.
- [x] **M1** Region table, checks on `LI`/`LC`/`SI`/`SC`, `C4I_C4MPG`
      reported. No C4KE. `mpg_overrun`, `mpg_underrun` and `mpg_freed` caught
      and NAMED; 73 corpus programs run identically under the guard.
      `make test-mpg`, which checks the fork against `c4m.c` first.
- [x] **M2** Syscall buffer ranges (`READ`, `OPEN`, `PUTS`, `PRTF`, `MSET`,
      `MCMP`, `MCPY`), plus strings as ranges whose length nobody knows.
      `mpg_syscall` AND `mpg_unterminated` caught — both halves of the vfsload
      class.
- [~] **M3** `TRAP_MPG_VIOLATION` done, **and C4KE handles it** — see "Who says
      what" below. The four opcodes and the narrowing rule are still to do.
      (`mpg_freed` was caught back at M1, out of the allocator hook.)
- [ ] **M4** `c4ke_mpg.c`: per-task regions from `TASK_EXTDATA`, context switch
      on trap and `TLEV`. `mpg_foreign`, `mpg_kernel` caught. **No change to
      `c4ke.c`** — if one turns out to be unavoidable, write down why here
      before making it.
- [x] **M5** Measured on a real kernel: **1.23x** over `c4m` for
      `load-c4r.c -- c4ke.c4r innerbench -n 50`, 83.9 s against 103.6 s.
      Recorded below with the microbenchmark that flattered it.
- [ ] **M6** Aimed at the known bugs; `make test-mpg` pinned (the known-bad
      messages and the fork check are pinned already).

## Deviations, and what M1 measured

**A fork after all, and the argument against it was wrong in an instructive
way.** The first cut built the guard from `c4m.c` with `-DC4MPG=1`, reasoning
that a 2,300-line fork rots and that an `#ifdef` was safe because no BUILD of
`c4m.c` went through raw `c4cc` any more. The second half of that was true and
beside the point. **`c4m.c` is read as SOURCE at run time.** `innerbench`
launches `c4 c4m.c load-c4r.c -- src/c4ke/c4ke.c`, plain `c4` has no
preprocessor, and it compiles what is between the `#` lines it skips — so every
`#ifdef C4MPG` would have been unconditional code in every nested interpreter on
the board. A conditional is only a conditional to something that evaluates it,
and the exposure was at run time, not build time.

So `c4mpg.c` is a real fork. The rot argument was also real, so it is paid for
rather than argued away: **`src/tests/mpg/check-fork.sh` strips every block
between the `//>>> c4mpg` and `//<<< c4mpg` markers and demands byte-identity
with `c4m.c`**, and `make test-mpg` runs it first. `c4m.c` is derived from
`c4mpg.c` by exactly that strip, so the two are identical by construction, and
a change to one that does not reach the other fails the build with the lines
named. It caught its first drift — four stray blank lines — within a minute of
being written.

The one thing left behind in `c4m.c` is `C4I_C4MPG = 0x100` in the info enum: a
bit assignment, no code, so nobody reuses it.

**The code region is writable.** `MPG_R | MPG_W | MPG_X`, not `rx`. Self
modifying code is a documented, load-bearing technique in this family:
`c4_invoke_stub` rewrites its own first instruction to a `JMP`,
`include/c4dos.h`'s `__c4dos_stub` does the same to reach the DOS API on a
machine with no indirect call, and `src/tests/c4_jailbreak.c` is a whole test
about doing it deliberately. A guard that forbids the VM's own idioms is a guard
that gets turned off. Narrowing it per-program is what `MPG_DEFINE` is for, at
M3.

### What the known-good half found, which is why it is not optional

Both findings came from the first corpus run, and neither was a bug in the
programs:

- **`test_switch` tripped on c4m's own switch jumptables.** They are plain
  `malloc`'d memory (`stmt()`), the compiled program READS them through an `LI`
  on every switch, and they belong to no pool. c4m hands the guest memory in
  places other than `MALC`, and the guard had to be told. `mpg_note()` records
  them during compilation; they become regions at arm time.
- **`c4_jailbreak` tripped on writing into the code area** — see above.

After both: **73 of 73** corpus programs produce byte-identical output to plain
`c4m`, zero violations. (Four of them print addresses of their own and so differ
run to run under ASLR; the test names them rather than loosening the
comparison.)

### What M2 added, and the correction it needed

The buffer half is the half no per-instruction check can do: the overrun in
`read(fd, buf, 262144)` happens inside the HOST's `read()`, so not one guest
`LI` or `SI` executes for it. `mpg_range` checks `[buf, buf+len)` before the
pointer is handed over, and says:

    c4mpg: read() into 0x586d10fecd40 (65536 bytes) -- outside every region this program owns
    c4mpg:   nearest below: 'malloc' [0x586d10fecd40,0x586d10fecd80) rw
    c4mpg:   it starts inside that region and runs 65472 bytes past the end

**Strings needed a second pass, and the test caught the first one being lazy.**
A string is a range whose length nobody knows until they have already read it,
so `mpg_string` walks to the end of the owning region looking for the
terminator. The first cut checked only `printf`'s FORMAT string, on the stated
grounds that "c4m cannot know which of the arguments a `%s` will reach for".
It can: the format is right there and the arguments are positional.
`mpg_unterminated.c` walked straight past that version, printing eight bytes
and whatever followed them — which is F12's root cause verbatim, *an
unterminated buffer in the one function that had not been read*. `mpg_printf`
now walks the format (counting `*` width arguments, which would otherwise
misalign every `%s` after them) and checks each `%s` argument as a string.

### Pointed at a real kernel, which is where the instrument was found wanting

`c4mpg load-c4r.c -- c4ke.c4r innerbench -n 50` — C4KE natively, fifty nested
c4m instances each compiling and running a kernel of their own. No C4KE
extension is needed for this to mean something: **C4KE's per-task stacks are
`malloc`'d**, so every one of them is already a region, courtesy of the MALC
hook.

The first attempt did not find a kernel bug. It found mine:

    c4mpg: region table full at 16384 -- the guard is now INCOMPLETE
    c4mpg: wrote 0x559e9cd0f7e0 (1 bytes) -- outside every region this program owns

The design said "region counts are small — a task has a stack, a code area, a
data area, and its own allocations". True of one program, false of fifty nested
kernels. And the "violation" on the second line is the cap, not the kernel: with
the table full a new allocation gets no region, so a legal write into it reads
as a fault. **That saturation message is the only reason a false finding was not
reported as the prize.** The table grows now (doubling, host memory, so it
cannot recurse into itself), and the message stays for the day growth itself
fails.

With that fixed: **zero violations, clean shutdown, 103 seconds.**

**That is a real negative result and it narrows the hunt.** The wild write the
board reports under the same workload —

    c4ke: task 33 () OVERRAN ITS STACK: 44 of 262144 bytes, guard broken

— 44 bytes used and the guard broken, which is a write from somewhere else
entirely — **does not reproduce natively**. Fifty nested kernels with a
permission check on every load and store, and nothing touched anything it did
not own. So the corruption belongs to the board: either to c4bb's own arena and
`fw:` allocator, or to the memory-exhaustion path, which never happens natively
against 4 GB. That is where to look next, and it is a much smaller place than
"somewhere in C4KE".

### Performance, recorded whatever it says

Budget was under 2x, say so if worse than 3x. Measured on a load/store-heavy
loop (4,000 iterations over a 256-word buffer, 16.4M checked accesses), eight
runs each:

| | wall |
|---|---|
| `c4m` | 1.59 s |
| `c4mpg` | 1.70 s |

**1.07x** — on a microbenchmark, which flatters it. The honest number is the
kernel one:

| | wall |
|---|---|
| `c4m load-c4r.c -- c4ke.c4r innerbench -n 50` | 83.9 s |
| `c4mpg` same | 103.6 s |

**1.23x**, still well inside the 2x budget, and that is the figure to quote.
Under that load the cache hit rate falls to about half (746M hits against 703M
misses in the saturated run), because a kernel switching between fifty tasks has
no locality of the kind one program has, and every miss is a binary search over
tens of thousands of regions. If the guard ever needs to be faster, that is the
place: the lookup, not the inline test.

The microbenchmark's own hit rate is 75%, not the "high nineties" the design
expected, and the reason is worth keeping: the commonest statement in that loop
is `sum = sum + buf[i]`, whose READS alternate between the locals on the stack
and the array on the heap, so a one-entry cache thrashes every iteration.

A second read slot was tried. It took the hit rate to 87.5% and made the program
**slower** (1.11x), because the extra inline comparison costs more than the
calls it saves. Reverted, and recorded here rather than kept as a plausible
optimisation nobody timed. Splitting the cache into a read slot and a write slot
is kept — it costs nothing and the permission check falls out of it.

## Who says what: the trap, and why C4KE answers it

The user's design note, and it is the right shape: **c4mpg should not be the one
talking about tasks, because it has never heard of one.** So a violation is now
a trap, `TRAP_MPG_VIOLATION`, and the two halves of the report come from the two
places that actually hold the facts:

    c4mpg: wrote 0x61c9ee0ac7e0 (8 bytes) -- outside every region this program owns
    c4mpg:   nearest below: 'malloc' [0x61c9ee0ac7c0,0x61c9ee0ac7e0) rw
    c4mpg:   this access ends 8 bytes past the end of it
    c4ke: task 2 'badmem.c4r' touched 0x61c9ee0ac7e0, which it does not own
    c4ke: task 'badmem.c4r' at 0x61c9ee0129c0:
      Id: 2 State: 0x3 R
      Registers: A=0x63  BP=0x61c9ee0a31d8  SP=0x61c9ee0a31c0  PC=0x61c9ee0a84b0
      badmem.c4r:main()+0x2f8
        c4ke.c4r:__loadc4r_execute_entry()+0x60
          c4ke.c4r:loadc4r_execute()+0x850
            c4ke.c4r:task_loadc4r()+0x1398

**c4mpg holds the region table**, so it alone can say which region was nearest,
what its permissions were, how far past the end the access ran, and whether the
address used to be a region and was freed. **C4KE holds the task table**, so it
alone can say who, what they were called, where they were in their own code, and
— the part that matters most — that this is one task's problem and not the
machine's. It kills that task and schedules the next one.

Before this, a violation called `exit(11)` and took the whole VM with it. Now
the board carries on and shuts down of its own accord, which is the difference
between a debugging session and a halted machine.

### How it is wired, and what it costs when off

- `mpg_fault` prints the region facts and RETURNS rather than halting. The call
  site raises the trap. Only the miss path grew a return-value test; the inline
  cache check in the dispatch loop is untouched.
- `mpg_handler` mirrors the interpreter's `trap_handler`, which is a local of
  `c4m_main` and invisible from the guard. It decides one thing: whether there
  is a kernel above that can say more than we can. With no handler — a bare
  program, `src/tests/mpg/*` — c4mpg still prints its own stack trace and exits
  11, exactly as before.
- `TRAP_MPG_VIOLATION` is appended to the trap enum in `c4m.c`, `c4mpg.c` AND
  `src/c4ke/c4ke.c`, so the number is reserved across the family and the enums
  stay in step. Plain c4m never raises it.
- C4KE's arm is one `else if` near the bottom of `trap_handler`, alongside
  `TRAP_SEGV` and `TRAP_OPV` and built the same way. A kernel running under
  plain `c4m` never reaches it, so it costs nothing anywhere else.

`src/c4ke/bin/badmem.c` is the pin: one word past a `malloc`, as a C4KE task.
`make test-mpg` asserts both halves of the message, the resolved trace, and the
clean shutdown — plus a control that plain `c4m` lets the same task run to
completion, so the leg is testing the guard and not something else.

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

1. ~~**Where do a task's own `malloc`s become regions?**~~ **Answered at M1:
   in `MALC`.** `c4_malloc` defines the region and `c4_free` drops it, which is
   also the only place that knows the LENGTH — and the length is the whole
   point, since without it an overrun by one word is indistinguishable from a
   legal access. It came with a free bonus: use-after-free falls out of the same
   hook, so `mpg_freed` is caught at M1 rather than M3. A small ring of the last
   64 dropped regions lets the report say "THIS WAS A REGION ... freed" instead
   of "nothing owns this", which is true and useless.
2. **What owns firmware memory on c4bb?** `fw.c` allocates from the same arena
   and predates any context. Probably one region owned by 0, defined at boot.
3. ~~**Does the region table live in guest memory or host memory?**~~
   **Answered at M1: host.** It is c4mpg's own `malloc`'d array, in none of the
   pools and in no region, so nothing the guard is hunting can reach it. A guard
   living in the memory it protects fails exactly when it is needed.
