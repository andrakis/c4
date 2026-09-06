# What is still open — start here

Written 2026-09-06 at the end of a long session, for whoever picks this up.
`docs/dos-rung-fixes.md` is the tracker for what shipped; this file is only what
did NOT finish, and what is known about it.

---

## 1. The `innerbench -n 50` lockup — NOT FIXED

### What the user reported

On c4bb in the browser: `innerbench -n 50`, then `ls` (works), then `ls`
(fails, whole system locks). The `pc` register sits on a `TLEV` that appears to
point at itself and never advances. Their screenshot showed `mode: unprotected,
ITH set`, 8 tasks, 183 free slots, and the cycle counter still climbing.

### What is established, by measurement

- **The heap really does exhaust.** `fw: malloc(262144) failed: 2 free blocks,
  56 bytes, largest 56`. The user's instinct was right.
- **A hang at a `TLEV` is uninterruptible by construction.** `machine.js`
  refuses both the PIT and the cycle interrupt when PC is at a `TLEV`
  (it restores five registers atomically). So a frame that restores its own PC
  hangs with nothing able to break in — that is why the WHOLE system stops
  rather than one task dying.
- In the trap frame, **`bp+0` pointing at itself is BY DESIGN** (the microcode
  comments the bp-link slot as self-referencing). The user's screenshot showed
  `MAR = BP+0x10` = `bp+4`, the SAVED-BP slot, holding the current bp. That one
  is not by design.
- **`innerbench -n 2` (the default) is healthy at 32 MB** — two nested c4m tasks
  progressing, output flowing. Only the stress case exhausts.
- **`innerbench -n 5` at 64 MB was still healthy at 12 minutes.** The fault needs
  many tasks.

### What was fixed, and why it was not enough

Two real bugs on the out-of-memory path, both in `start_task_builtin`
(`src/c4ke/c4ke.c`), both now fixed and **uncommitted at time of writing**:

- Four error paths called `free(t)` where `t` is an INTERIOR pointer into the
  single `kernel_tasks` block. That hands the allocator a pointer it never
  issued — heap corruption, on the path taken when memory is short. It was also
  unnecessary: the slot is claimed by writing `TASK_STATE` further down, so on
  every failure path it is still `STATE_UNLOADED`.
- `start_errno` was set in five places and read in NONE. A failed spawn was
  silent. Now `start_fail()` reports:
  `c4ke: cannot start 'inner-kernel': out of memory for a 262144 byte stack`

**Verified: the message fires, and the lockup still happens.** `ls` afterwards
still returns nothing. Do not assume these fixes address the reported bug; they
are correct on their own merits and that is all that is proven.

### The live lead

In the post-fix run at `-m 128 -n 50`, five SPAWNED c4m tasks crashed like this,
**before any malloc failure**:

    c4ke: Custom opcode not found: 108138136, executed by task 19
    c4ke: Custom opcode not found: 107377816, executed by task 21
    c4ke: Custom opcode not found: 106617496, executed by task 23
    c4ke: Custom opcode not found: 105857176, executed by task 25
    c4ke: Custom opcode not found: 105096856, executed by task 27

The values decrease by exactly **760,320** each time — one task-allocation
stride. They are ADDRESSES being executed as opcodes, one per task, marching
down the heap. Whatever computes them is off by a whole allocation per task.

**This may be a regression introduced in this session.** F13 changed how the
shared disk builds `c4m.c4r`:

    # before (src/c4bb/tests/build-images.sh)
    $PREPROC c4m.c | $CC -o $DISK/c4m.c4r -
    # after
    $CC -o $DISK/c4m.c4r include/c4dos.h c4m.c

That was needed so `c4m` gets the DOS branch (raw `c4cc` skips `#` lines, which
is what switches `#if C4M_DOS` on). But raw `c4cc` compiles **both arms of every
`#if` in c4m.c**, where `gcc -E` picks one. `make test-c4bb` passes 41 legs
after the change, but nothing in that suite runs fifty nested c4m instances.

### The next experiment, in order

1. Build `$DISK/c4m.c4r` the OLD way (`$PREPROC`), leave everything else, and
   run `innerbench -n 50` at `-m 128`. If the five crashes vanish, F13 is the
   cause and the fix is to get the DOS branch in without compiling both arms —
   e.g. `$PREPROC -DC4M_DOS=1 c4m.c` with `c4dos.h` prepended separately, so the
   preprocessor still picks arms.
2. If they persist, bisect on task count (`-n 10`, `-n 20`) to find where the
   crashes start, then dump the offending address against the task table to see
   what the stride corresponds to.
3. Only then look at the `TLEV` frame. The self-referencing saved-bp is very
   likely downstream of whatever produces those addresses.

### How to reproduce (the harness matters)

    python3 <scratch>/lock2.py 128 50      # arena MB, -n count

**`cli.js` MUST be given `-i`.** It feeds the keyboard only when `-i` is passed,
or by prefeeding when stdin is a PIPE. A PTY without `-i` hits neither branch
and the machine receives nothing — which looks EXACTLY like a hang and cost
several hours of this session. **Always confirm a plain `ls` responds before
concluding anything about a hang.**

The harness must also stream its captured output to a file as it arrives.
Buffering it and writing at the end loses everything when `timeout` sends
SIGTERM.

---

## 2. `docs/c4mpg-design.md` — designed, not built

A complete design for a memory-protection fork of `c4m` plus a C4KE extension:
region table with owner+permissions, `C4I_C4MPG = 0x100` (verified free), four
custom opcodes with a narrowing-only rule, `TRAP_MPG_VIOLATION` that halts at
the bad write, and `c4ke_mpg.c` using `TASK_EXTDATA` and the `JMP`+`ENT`
trampoline patch. Six milestones, M0 through M6.

The user asked for this specifically because four bugs in one day were all
"something wrote where it should not, and it surfaced somewhere else much
later", and valgrind cannot help when the memory written IS validly allocated —
to somebody else. Ownership, not liveness.

Start at M1 (region table, `LI`/`LC`/`SI`/`SC` only, no C4KE). The known-bad
test corpus in that document is the first deliverable, not the last.

---

## 3. Smaller things left undone

- The Makefile's nine hand-picked `-m` values and `C4IX_CELLS` are still
  unmeasured guesses. `src/c4bb/tools/memcensus.mjs` now exists to derive them;
  nobody has. Measured so far: `c4ke32` 5.9 MB data / 420 B machine stack,
  `c4ix32` 17 MB / 280 B.
- Homeward's vendored c4bb is ~21 commits behind (`@ af9f0af`), 260 lines adrift
  in `devices.js`, no PIT — while its own header calls c4bb "the behavioral
  oracle". None of this session's fixes have reached it.
- C4IX programs detect C4DOS but not C4KE. Doing it safely needs C4KE's loader
  to patch a `__c4ke_present` symbol; probing by name is unsafe because C4IX's
  `sched_trap` forwards C4KE-range opcodes to `ck_dispatch`.
- `innerbench`'s default mode is `ONLY_C4M | ONLY_DIRECT`, and `ONLY_DIRECT` is
  marked `// No longer valid` in `src/bench/innerbench.c:15`. It is also the
  mode that requires `src/c4ke/c4ke.c` on the medium under that exact slashed
  path. Someone should decide whether the default is right.

---

## 4. Method notes, earned expensively this session

- **Validate an instrument against a known-good case before believing it.** The
  memory census passed `{diskDir}` to `Devices` (which wants a drives array),
  silently ran every image with no disk, and produced a confident table of
  nothing. The PTY harness omitted `-i` and produced four hours of "lockups"
  that were the harness typing into a void.
- **Three eliminations do not license a guess about the fourth.** A reentrancy
  story for the vfsload bug was impossible on inspection — C4KE's preemption is
  implemented BY c4m's dispatch loop, so a guest task cannot interleave inside a
  c4m C function. The real cause was an unterminated buffer, in the one function
  that had not been read.
- **A warning printed at the moment a thing stops working is not incidental.**
  `c4m: unable to open uptime file` was dismissed as harmless; it was the whole
  bug (the clock latched at 0 forever, so the kernel's IPS calibration could
  never finish, at any cycle budget).
- Do not pipe a long-running background job through `tail`; it buffers until EOF
  and hides all progress.
