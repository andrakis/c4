# What is still open — start here

Written 2026-09-06 at the end of a long session, for whoever picks this up.
`docs/dos-rung-fixes.md` is the tracker for what shipped; this file is only what
did NOT finish, and what is known about it.

---

## 1. The `innerbench -n 50` lockup — the corruption is fixed, the exhaustion is not

**Settled 2026-09-06: F13 is NOT the cause.** Measured both ways on two disks
identical but for `c4m.c4r`, `innerbench -mlqT -n 50` at `-m 128`: the pre-F13
build produced **11** `Custom opcode not found` crashes, the F13 build **0**.
Full write-up, including what F13 *did* silently cost, is
`docs/dos-rung-fixes.md` round four.

**What the crashes actually were.** `trap_handler` indexed `custom_opcodes` with
no bounds check and jumped to whatever word it read (`c4ke.c`, TRAP_ILLOP arm).
The opcode numbers are pointers, because `__c4_opcode` leaves its last-evaluated
argument in `a` when a request is not serviced and `__u0_ops_init` stores that.
So one task raising a bad opcode made the kernel jump into data, and the damage
landed on bystanders — including c4sh, which is why `ls` worked and then the
whole system stopped. Bounds check added; the same probe now kills only the
offender, and the same innerbench run drops to 2 wild opcodes with real stack
traces attached.

**Still open.** At `-n 50` / 128 MB the machine genuinely exhausts its heap and
then spins forever with no way back to a prompt. That is now an honest resource
limit (1708 `malloc failed` lines say so) rather than corruption, but a system
that cannot say "I am out of memory, here is your shell back" is still wrong.
Two leads, both newly visible only because F17 stopped eating them:

- `c4ke: task 33 () OVERRAN ITS STACK: 44 of 262144 bytes, guard broken` — 44
  bytes used and the guard broken is not an overrun, it is a wild write. Find
  the writer.
- `lc4r: unable to open '(null)' or '.c4r'` — a task started with a null name.

**Reproduce it in seconds, not half an hour.** `innerbench -mlqT -n 50` (`-l`
loads the precompiled `c4ke.c4r` instead of recompiling `c4ke.c`; `-T` skips
`top`). Drive it with `src/c4bb/tests/drive.mjs`: paced writes to `cli.js -i`,
output streamed to a file as it arrives.

**Harness rule, restated because it bit again in a new shape.** Do not prefeed a
pipe for a multi-command session. Until F18 was fixed the machine took the whole
buffer in one read and threw away everything after the first line, so a correct
machine looked hung. `cli.js -i` with paced writes is the only trustworthy way
to type more than one command. Validate the harness on a known-good session
before believing anything it tells you.

---

## 2. c4mpg — M1 and M2 are in, M3 is next

`make test-mpg` is green: the region table, checks on `LI`/`LC`/`SI`/`SC`, the
syscall buffer ranges, strings walked to their terminator, `C4I_C4MPG`
announced. Five known-bad programs caught AND named
(`mpg_overrun`, `mpg_underrun`, `mpg_freed`, `mpg_syscall`, `mpg_unterminated`),
73 corpus programs byte-identical to plain `c4m`, 1.07x on a load/store-heavy
loop against a budget of 2x. `docs/c4mpg-design.md` has the deviations and
every measurement.

`c4mpg.c` is a real fork of `c4m.c`, pinned by `src/tests/mpg/check-fork.sh`:
strip the marked blocks and it must be `c4m.c` byte for byte. It is a fork
because **`c4m.c` is read as SOURCE at run time** — `innerbench` runs
`c4 c4m.c load-c4r.c -- src/c4ke/c4ke.c` and plain `c4` has no preprocessor, so
an `#ifdef` there is unconditional code in every nested interpreter. Edit
`c4m.c`, then re-run the check; it names the lines that have gone out of step.

**M3 next:** the four opcodes (`MPG_DEFINE`/`MPG_DROP`/`MPG_CONTEXT`/
`MPG_QUERY`), the narrowing-only rule, and `TRAP_MPG_VIOLATION` so a program
can handle a violation instead of only halting on it. Then M4 (`c4ke_mpg.c`,
per-task regions from `TASK_EXTDATA`, **no change to `c4ke.c`**) and M6 (aim it
at `innerbench -n 50` and at F12 reverted locally — that is the run that would
name the wild write behind
`task 33 OVERRAN ITS STACK: 44 of 262144 bytes, guard broken`).

The user asked for this specifically because four bugs in one day were all
"something wrote where it should not, and it surfaced somewhere else much
later", and valgrind cannot help when the memory written IS validly allocated —
to somebody else. Ownership, not liveness.

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
