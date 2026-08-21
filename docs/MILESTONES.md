# Milestones — a full-stack systems project built on c4

This document is a technical account of what has been built on top of
`c4`, `robert@swierczek`'s original ["exercise in
minimalism"](https://github.com/rswier/c4): a ~500-line self-hosting
C compiler/interpreter. Starting from `c4m` (this fork's extended
interpreter — traps, cycle-based preemption, custom opcodes) and
`C4KE` (a pre-emptive kernel written in the bare C4 subset), the last
week has added a Lisp interpreter, an optimizing C compiler written in
that Lisp, a second-generation OS compiled by that compiler, a
one-instruction reduction of the VM itself, a breadboard-style
hardware reimplementation of that same VM, and an OR1000 emulator —
compiled by the from-scratch toolchain — booting a real, unmodified
Linux kernel.

The point of writing this down: taken piece by piece, each of these is
a solid weekend project. Taken together, they form a coherent tower —
language, compiler, kernel, hardware model, and now a second OS
running *inside* the first — where every layer is built by, and
verified against, the layer below it. That combination (self-hosted
toolchain → OS → OS-on-hardware-model → guest-OS-in-an-emulator, all
sharing one C dialect and one VM contract) is not something we've seen
assembled from a `c4`-class minimal compiler before.

Every number below is measured, not estimated, and every "done" is
backed by a diff against a reference implementation, a byte-identical
round-trip, or an oracle. Where something is still open — a race
condition, a milestone in flight — it's called out as such. That's
deliberate: the credibility of the "done" claims rests on being honest
about the ones that aren't.

## The shape of the week

```
Aug 2   c4sp        — a Lisp interpreter, in the C4 subset
Aug 3   c4lc + C4IX — an optimizing C compiler (in that Lisp) and the
                       OS it compiles, built in lockstep, same day
Aug 4-5 oisc4,       — a one-instruction VM reduction, and three
        C4IX harden.   preemption races run to ground in C4IX
Aug 8   c4or1k       — jor1k ported to C4, boots real Linux to the
                       expected panic
Aug 8   c4bb         — c4m rebuilt as a breadboard computer; both
                       C4KE and C4IX boot on it
Aug 8   c4or1k M5    — 9P root filesystem, in flight
```

Everything here sits on top of `c4` (2014) and this fork's `c4m` and
`C4KE`. The rest of this document goes layer by layer.

---

## c4sp — a Lisp interpreter in the C4 subset

**What it is.** `src/c4sp` is a Lisp dialect (modeled on
[alisp](https://github.com/andrakis/alisp)) implemented in plain C4.
Its entire reason to exist is to host a C compiler written in Lisp
instead of C — see c4lc below.

**Status: implemented, 2026-08-02.** Every milestone in the design doc
is done, M0 through M6: cells/reader/printer, an evaluator for the
core special forms, mark-and-sweep GC, macros and tail calls, a CEK
continuation-passing conversion, strings/floats/file IO/REPL,
`.c4r` read/write, and an in-Lisp optimizer.

**The GC decision, and why it matters.** A closure capturing its own
defining environment — `(define f (lambda ...))` — creates a
reference cycle from "the most ordinary thing a program can do."
Reference counting was ruled out on that basis; it would leak on
essentially every real program. A copying (Cheney) collector was also
ruled out — it requires precise GC-root registration everywhere a
pointer might be live across an allocation, and "one missed pop is a
memory-corruption bug that appears under load, weeks later." The
chosen design is a non-moving mark-and-sweep collector, conservative
on the C4 evaluation stack: because it never relocates an object, a
conservative false-positive can only over-retain a dead cell, never
corrupt a live one. The one invariant this design leans on is stated
directly in the design doc: *"every structure that can hold a cell
reference must itself be a cell in the arena, or be registered as an
explicit root."* It's verified, not just asserted —
`src/tests/test_gcscan.c` builds nested stack frames holding pointers
into a pretend arena and confirms the conservative scanner finds them
all, under both `c4m` and plain `c4`.

**Why CEK, not reified frames.** A C4KE task stack is 8191 words —
enough for roughly 600 nested calls of the naive recursive evaluator,
which a real 4000-deep non-tail Lisp recursion blows through. The fix
is a CEK-machine conversion (continuations as explicit values instead
of the host call stack), diffed instruction-for-instruction against
the recursive evaluator to prove equivalence. Because c4sp already has
cons cells, "the continuation *is* the list" — kont frames are
ordinary cells, so the GC handles them for free, with zero extra
plumbing.

**The linker fell out for free.** An early design draft assumed
`.c4r` needed a new labelled format for c4sp to parse. It didn't:
`asm-c4r.c` already records a `(type, address, value)` patch entry for
every address word in the image, so c4sp reads `.c4r` directly with no
changes to the compiler. The same patch-table insight produced
`c4rlink` — full multi-module linking (segment concatenation with
rebasing, symbol merge by name, constructor/destructor list merge,
`.c4l` archive mode) — verified `valgrind`-clean under both `c4m` and
plain `c4`.

**Numbers.** The in-Lisp optimizer shrinks `c4cc.c4r` by 8.9% with
identical output. Builtins dispatch via `JSRI` on `c4m` (~3
instructions/call) and fall back to a ~30-comparison if/else chain
under plain `c4`, so c4sp stays runnable on the unmodified original
interpreter.

---

## c4lc — an optimizing C compiler, written in c4sp's Lisp

**What it is.** `src/c4lc` is a ground-up replacement for the
original single-pass `c4cc` frontend: a real recursive-descent parser
producing an AST, a code generator, a peephole optimizer, a tree-level
optimizer, and (as of L9) its own preprocessor — the entire thing
written in c4sp Lisp and self-hosting.

**Status: L0–L9 all done.** The original L0–L6 roadmap shipped
2026-08-03 in eleven commits in one day, interleaved with C4IX's X0–X5
(below) — the compiler and the OS it would go on to build were
developed in lockstep. L7 (structs/unions/typedef/C99 control flow)
and L8 (separately-compiled `.c4o` object files, resolved by
`c4rlink`) followed the same day to unblock C4IX. L9 (a real
preprocessor) landed after that.

**A real bug in the compiler it replaces.** Building L3 turned up a
genuine `c4cc` bug: a bare function name used as a value emits a
bogus `LI` (load-from-address) instead of a function pointer — the
existing code only works because `&fn` happens to rewind the emission
afterward. Found by differential testing, not inspection.

**Verification discipline.** Every stage is checked against an
independent reference, not just "compiles and runs": L2 is verified
four ways including a byte-identical round-trip through the `.c4r`
reader/writer; L3's battery is 17 exact + 4 pointer-masked +
2 variadic byte-identical comparisons against committed `gcc` output
(`c4cc` can't compile the L7-era test files at all, so `gcc` is the
only available oracle); L9's preprocessor is checked by rebuilding all
eleven C4IX kernel modules with `c4lc -P` instead of `gcc -E` and
diffing for byte-identical objects — which is also the moment `gcc`
left the C4IX build entirely: *"the last host-toolchain dependency is
gone."*

**L9's preprocessor works on tokens, not text** — the lexer gained a
pp-mode where identifier tokens carry a flag for whether a `(`
"touched" them, which is the actual C standard rule distinguishing
`#define ADD(a,b)` from `#define TWO (x+y)`. It implements
`#include` with `-I` search, object/function-like macros, the full
`#if`/`#elif`/`#else` family with a real constant-expression
evaluator, gcc-style `# N "file"` line markers, and both `#`/`##`
stringize/paste — including the two-level `STR`/`XSTR` idiom real C
code relies on for it.

**Measured results (2026-08-03), instruction count / bytes:**

| image | `c4cc` | `c4opt` | `c4lc -O` | vs. `c4cc` |
|---|---|---|---|---|
| c4ke | 23,260 / 339K | 20,710 / 312K | 18,495 / 276K | **−20.5%** |
| c4sp | 19,695 / 280K | 18,719 / 270K | 16,125 / 235K | **−18.1%** |
| c4m  | 15,215 / 225K | 14,538 / 218K | 11,139 / 172K | **−26.8%** |

Kernel boot cycles to "ready": `c4cc` 898.0k → `c4opt` 890.5k →
`c4lc` 801.9k (**−10.7%**) — wall-clock boot time is unchanged, the
win is in cycles spent, not time waited. Compiler-on-itself workload
(c4sp hosting c4lc, compiling `c4lc_l2.c`, median of 3 runs): `c4cc`
build 10.5s, `c4opt` build 7.32s, `c4lc` build 7.31s — **1.44x faster**
than the `c4cc` build, output byte-identical across all three.

**Host independence, proven not assumed.** Compiling the same source
on three different hosts — native c4sp, `c4sp.c4r` under `c4m`, and
the interpreter c4lc itself just built — produces byte-identical
images. And it isn't just self-referential: `test_ramcc` starts c4sp
and c4lc as a C4KE task, the compiled image lands in the kernel's
RAM filesystem, and the kernel executes it straight from memory — "no
write ever touches the host filesystem."

---

## C4IX — a second-generation OS, compiled by c4lc

**What it is.** `src/c4ix` is a ground-up rewrite of C4KE — structs,
separately-compiled objects linked by `c4rlink`, real syscalls,
a VFS with pipes/redirection/directories, a userland shell — built
specifically to prove c4lc's language extensions (L7/L8) hold up under
a real OS, not just test programs.

**Status: X0–X6 all done, all landing 2026-08-03 through 2026-08-05.**

- **X0 — boot.** Six c4lc-compiled, `c4rlink`-linked modules, booting
  natively on `c4m` and in degraded mode on plain `c4`. Varargs needed
  their own subsystem (`va.c`) because stock `stdarg.h`'s per-unit
  static area doesn't merge across separately compiled objects.
- **X1 — tasks.** One context-switch state format, two backends behind
  it: on `c4m` every switch runs inside a trap handler; on plain `c4`,
  `sched_yield` rewrites its own stack frame and returns through a
  double `LEV` — a technique proven correct by a dedicated test before
  it was trusted in the scheduler.
- **X2 — syscalls.** "Two doors, one dispatcher": a custom illegal
  opcode under `c4m`, or a symbol the loader injects into the image
  under plain `c4`. Measurement caught two real perf bugs: a spin-wait
  masquerading as a blocking wait, and a formatter writing one
  character per syscall — fixed, `uhello` went from **>30s to
  0.107s**.
- **X3 — IO.** A real vnode/file-description/fd VFS layered for
  `dup2` and pipes. Measurement (not inspection) found four bugs here,
  the worst being a missing `MODE_UNPROTECTED` transition on the
  syscall trap path that caused a protected task's *own* first
  `putchar` to raise a nested trap — "the root cause of a week of
  heisenbugs."
- **X4 — shell.** An ordinary userland program (no privilege), with
  pipelines, `<`/`>` redirection, and `&` background jobs — built
  without `fork()`, by pointing the shell's own file descriptors at
  each stage before spawning it.
- **X5 — polish**, plus **X6 — C4KE binaries run unmodified.** C4KE's
  own `ps`/`top`/`spin`/`bench`/`benchtop`/`innerbench` — the exact
  files C4KE's own test suite uses — run as C4IX tasks with no
  rebuild, attaching through the same syscall gateway C4IX uses for
  itself. `bench` scores ~72% of its native C4KE figure and wall time
  stays within 6% — "the tax for having pipes, redirection and a VFS
  at all."

**Three preemption races run to ground (2026-08-04/05) — this is the
most technically interesting part of C4IX.** The most serious: the
preemption mask was global rather than per-task, so a task holding it
across a context switch handed the *next* task an interrupt-masked
machine — "a compute-bound task landing in that state never traps
again... nothing else is scheduled and nothing can stop it." The
deepest one — the eventual root cause of both a `spin 1` hang and a
`ps -s` hang — was a genuine VM-level liveness stall: *"a trap handler
cannot re-arm the interrupt from inside itself. It does not own the
registers — `TLEV` installs them, several instructions after the
handler's last statement."* An interrupt landing in that gap between
switching the current task and reaching `TLEV` swaps two tasks'
contexts mid-flight, and the machine ends up spinning on a `TLEV`
whose frame returns to itself. It was caught under `gdb` as a literal
self-referential stack frame, then reproduced on demand with a
scratch trap/`TLEV` ring buffer, and fixed in `c4m` itself
(`CONF_TRAP_RESTORES_INTERVAL` — the interrupt mask becomes part of
the saved trap context, not a machine-global). Stress-test result
after the fix: **4/4 clean runs at twenty times the shipped
preemption rate**, up from failures at the shipped rate before it.

**Measured boot, from the VM's own cycle counter:** 311,615 cycles to
load the kernel image, 12,994 for kernel init, 18,859 to spawn and run
the first user program — **343,468 cycles, power-on to userland.**
A syscall or a full context switch each cost 1,237 cycles.

---

## C4KE — the kernel this whole tower descends from

**What it is.** `src/c4ke/c4ke.c` is the original pre-emptive
multitasking kernel, written entirely in the bare C4 subset, where —
per `docs/internals.md` — "a context switch is just a trap handler
assigning new values to its own `a`/`bp`/`sp`/`returnpc` parameters
before returning through `TLEV`." It predates this week's work and
everything above builds on the trap/interrupt contract it established.
It's still actively maintained, not frozen:

- **A double-free that only protected mode exposed** (2026-08-02):
  every built-in task shared the kernel's own `.c4r` module pointer,
  so cleaning up any built-in task freed the kernel's own image;
  nothing triggered it until protected mode added tasks that actually
  exit and get reaped. One-line fix, confirmed against `make test`
  (8 runs), `make test-alt`, and 20 concurrent processes.
- **A RAM filesystem** (2026-08-02) — the kernel's only write path,
  since the VM itself has no write syscall — consulted by the program
  loader before the host filesystem, letting a program compiled
  entirely inside a running C4KE task be executed straight from
  kernel memory.
- **A real filesystem manifest** (2026-08-08, alongside c4bb below):
  the RAM filesystem existed but was never populated — `ls` printed a
  hardcoded fake listing. `vfsload.c` now reads a small declarative
  manifest DSL off disk at boot and populates it for real.

---

## c4bb — c4m rebuilt as a breadboard computer

**What it is.** `src/c4bb` is a hardware-style, microcoded, 32-bit
reimplementation of the `c4m` VM — in the spirit of Ben Eater's
breadboard computers — simulated in JavaScript with both a
cycle-accurate step engine and a JIT "turbo" engine, and capable of
booting **both C4KE and C4IX**, with full preemption and protected
mode, on the same hardware model. Landed 2026-08-08.

**Verification is the headline, not the simulation itself.** The
trap microroutine — c4m's `trap()` reimplemented as ~45 microcode
steps — is checked byte-for-byte against native `c4m32` by dedicated
tests covering custom opcodes, re-entrant traps, both preemption
re-arm styles, and protected-mode violations. `tools/lockstep.js`
proves the step engine and the turbo (JIT) engine stay
register-identical at every instruction boundary. `make test-c4bb`
diffs ~15 userland images byte-for-byte against native `c4m32`,
including exact cycle-counter parity and the floating-point device.
The turbo engine runs at roughly **15–18M instructions/sec** — about
17x realtime for what's modeled as "a 1 MHz machine."

**A real firmware bug found and fixed:** `fw_malc`/`fw_free` mutated
a shared free list with no interrupt masking — invisible under C4KE
(which never preempts inside firmware calls) but a genuine hazard
under C4IX's preemption. Fixed, but this is a project that reports
its remaining open bugs rather than hiding them:

- A timing-sensitive race intermittently corrupts one filesystem
  manifest entry under C4IX's preemption ("roughly one run in a few")
  — heals when unrelated debug output shifts timing, "the signature
  of a preemption race, not a logic bug." Deliberately *not* masked
  around, because a first attempt at masking made it worse (5/5
  failures instead of intermittent).
- A longer-running `top` (~1.1M cycles) corrupts a live stack slot and
  locks the PC permanently at the trap-return address. The trap
  microcode itself is independently verified correct, and simple
  stack exhaustion has been ruled out, but the actual cause isn't
  found yet — `top` is marked "not recommended for long unattended
  runs" rather than silently left as-is.

**A genuine upstream finding:** native `c4m32` (a `gcc -m32` build of
the reference interpreter) segfaults on some custom-opcode-heavy test
images and on booting C4IX at all — "the c4bb simulator is currently
the more robust 32-bit host" than the reference implementation it's
modeled on.

---

## oisc4 — the same VM, reduced to one instruction

**What it is.** `src/oisc4/oisc4.c` runs already-compiled `.c4r`
images — from either `c4cc` or `c4lc` — by translating every C4/`c4m`
opcode into a fixed-length sequence of exactly one instruction form:
`add [source], [literal], [dest]`, which also latches comparison
flags. Landed 2026-08-04, replacing a 2023 attempt that forked the
compiler frontend and was, in its own postmortem, "non-relocatable and
SIMD-hostile." The rewrite's key idea is not touching the compiler at
all: the existing `.c4r` patch table already says exactly which
operand words are code/data addresses versus plain integers, so
translation needs no guessing and no symbolic assembler.

**Verification: bit-identical against native `c4m`** across roughly
twenty real test images, including 3,365 floating-point test cases run
through `test_float`. One genuine robustness win came out of this
comparison for free: `fun_with_ptrs` segfaults on *both* `c4m` and
`c4`-via-the-loader — "OISC4 survives further" than either reference
interpreter on that input.

**The acid tests.** Self-hosted `c4` compiles and runs `hello.c` —
inside its own inner VM — entirely on the one-instruction machine.
The full c4sp Lisp interpreter, GC included, runs under OISC4 and
correctly round-trips a `.c4r` file. And it nests both ways at once:
`c4m` running an `oisc4` guest running `hello.c4r`; plain `c4` running
`oisc4` running `hello.c4r`; and `oisc4` running an `oisc4`-compiled
copy of itself running `hello.c4r` — OISC on OISC.

**Cost of the reduction:** code expands roughly 5–7x in words; on a
compiler workload, native `oisc4` runs 5.8x slower than native `c4m` —
"for a machine with one instruction," a fair trade, not a surprise.
One documented hard limit: `c4m.c4r` itself can't run under `oisc4`,
because `c4m`'s invoke-stub trick pattern-scans its *own* return
address backward through raw code words at runtime — a technique that
is, by construction, outside what a static binary translator can
support.

---

## c4or1k — jor1k ported to C4, booting real Linux

**What it is.** `src/c4or1k` is a from-scratch C port of
[jor1k](https://github.com/s-macke/jor1k) — a JavaScript OR1000/
OpenRISC CPU emulator — written in C and compiled by c4lc: a full
integer ISA, SPRs, exceptions, DTLB/ITLB miss vectors, a 16550 UART,
and a boot loader, sufficient to run an unmodified `vmlinux.bin`
through kernel init. Landed 2026-08-08, in five milestones the same
day.

**Why this was worth trying at all.** jor1k's own CPU core is only
~1,100 lines — a plain switch on the primary opcode with sub-switches
on function-code fields — genuinely an order of magnitude simpler than
either OS already running on this toolchain. The only real open
question was throughput: M0's tight-loop benchmark measured **~1.37M
guest instructions/sec** against a pre-M0 worst-case estimate of only
50–200K/sec, which is the number that turned this from "interesting
idea" into "worth building the rest."

**Verification: bit-for-bit against jor1k's actual code, not a
respecification of it.** M1 and M2 are checked by a Node.js oracle
that `require()`s jor1k's real `safecpu.js` directly and diffs every
register, flag, and a RAM window after each test instruction —
**exact match**, covering every instruction form the CPU implements.
This caught real bugs before they could hide in a kernel boot: a
sign-extension idiom that only works on a genuinely 32-bit register
(c4lc's `int` is 64-bit, so the classic shift-truncate trick silently
reconstructed the original value instead of sign-extending — fixed
with a width-independent mask+XOR form); a branch-delay-slot bug in
the *test program*, not the CPU, where an unprotected instruction
after a branch silently overwrote every compare result — caught
immediately by the oracle diff rather than by inspection; and two bugs
in the oracle itself, including the discovery that jor1k's "big-endian"
memory accessors are not byte-swapped storage at all but native
access through XOR'd addressing — the kind of detail an
independently-invented "obviously correct" formula would get
plausibly, silently wrong.

**M4 reaches the real, generic Linux panic, not a wedge.** With the
UART, MMIO, and boot loader in place, `make c4or1k-boot` runs an
unmodified `vmlinux.bin` from the actual reset vector through kernel
boot banner, memory/MMU setup, DTLB/ITLB miss handler registration,
and dozens of subsystem inits (network protocol families, SCSI, ALSA,
block layer, io schedulers, 9P registration) to exactly:

```
VFS: Cannot open root device "host" or unknown-block(0,0): error -2
Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0)
```

— the same generic failure *any* emulator without a working root
filesystem produces, which is what makes it trustworthy evidence on
its own rather than something that needed a side-by-side jor1k run to
confirm. Every unimplemented device (virtio-block, a second UART,
framebuffer, ATA, keyboard, ethernet, RTC) probes and fails
gracefully instead of wedging the boot.

**The bug that made the first boot attempt never get this far:**
automatic tick-timer interrupt delivery had been deliberately deferred
since M2. The first real boot attempt didn't panic — it hung forever
retry-polling an unimplemented ATA controller, which was the tell that
something was blocked on a timeout that could never expire. Wiring in
`cpu_tick_check` (advance `TTCR`, raise `EXCEPT_TICK` on schedule) let
the *same* instruction budget that previously never left the ATA probe
reach the panic with room to spare.

**In flight right now: M5, a 9P root filesystem.** Not yet committed,
but the plumbing is written and wired in: a virtio-mmio transport
matched against jor1k's device model at the register level; a
synchronous 9P2000.L protocol handler (no async event loop needed,
since there's nothing to defer to in a single-threaded emulator); and
an in-memory inode tree loaded from an offline-converted copy of
jor1k's own `basefs.json` (1.44MB of filesystem image, ~38 inodes,
already built and sitting in `src/c4or1k/images/`). The kernel command
line already baked into `vmlinux.bin` — `root=host rootfstype=9p
rootflags=trans=virtio` — is what pins the virtio-mmio base address
and IRQ line the transport code targets. This isn't verified against
a successful mount yet; that's the next result to land, not a claim
being made now.

---

## Why this adds up to more than the sum of the parts

Reading top to bottom, every layer above is checked against the layer
below it, and several are checked against something built entirely
independently:

- `c4sp` (Lisp) is verified against its own recursive evaluator via
  CEK equivalence, and its `.c4r` I/O against round-trip identity.
- `c4lc` (a C compiler, written in that Lisp) is verified against
  `c4cc` where they overlap and against `gcc` where c4lc's language
  coverage exceeds `c4cc`'s.
- `C4IX` (an OS, compiled by that compiler) runs C4KE's own unmodified
  binaries and is measured against C4KE's own cycle counts.
- `c4bb` (a hardware model of the VM everything else runs on) boots
  both operating systems and is checked microcode-step-for-step
  against the native interpreter it's modeling.
- `oisc4` (the same VM reduced to one instruction) is checked
  bit-for-bit against the native interpreter, then made to run the
  compiler, the Lisp, and *itself*, nested three ways.
- `c4or1k` (a second CPU architecture, compiled by the whole
  toolchain) is checked against an actual reference emulator's source
  code, not a written spec, and then handed a real, unmodified Linux
  kernel that has no idea any of this exists.

That's the shape worth naming: a minimal self-hosting C compiler that
now hosts its own Lisp, which hosts its own optimizing C compiler,
which builds a second OS and a second CPU architecture faithful enough
to run production Linux — while the original VM has independently
been rebuilt as both simulated hardware and a one-instruction machine,
and both of those run the *same* stack again on top of themselves. Any
one piece is a good project. The fact that they all share one C
dialect and one VM contract, and every seam between them is verified
rather than assumed, is the part worth being excited about.

## What's still open

Kept here on purpose, not swept into the wins above:

- **c4or1k M5** — 9P plumbing is written but an actual successful
  root-fs mount hasn't been confirmed yet.
- **c4bb** — a preemption-timing race that corrupts one filesystem
  manifest entry intermittently, and a separate long-run `top` bug
  that locks the PC after ~1.1M cycles; root cause not yet found for
  either.
- **C4IX** — a known intermittent stall on a backgrounded, redirected
  command after many prior commands, tracked outside the pinned test
  suite rather than claimed fixed.
- **C4KE** — `realloc()` is confirmed broken at the VM level
  (`RALC`'s case is dead code, falls through to an unhandled trap);
  protected-mode's `kernel/io` dispatcher costs ~13% of cycles because
  it spin-waits instead of parking.
