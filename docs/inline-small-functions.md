# T3 — inlining small functions

## Why

A call in c4 costs four instructions nobody asked for — `JSR`, `ENT`,
`ADJ`, `LEV` — plus a push per argument and a `LEA n; LI` inside the
callee for every parameter read. Measured on the board
(`docs/c4bb-uart-block.md`), the firmware's one-line

    void __fw_putc (int c) { *(int *)DEV_UART_TX = c; }

compiles to **13 instructions and 80 microsteps per call** where the
store it exists to perform is a single `SI`:

    call site:  LEA 3; LI; LC; PSH; JSR __fw_putc; ADJ 1
    callee:     ENT 0; IMM 256; PSH; LEA 2; LI; SI; LEV

Inlined it is six instructions and 31 microsteps. The function is not
the problem — a one-line accessor is the right way to write that — so
the compiler should be the thing that stops charging for it.

## Where it goes, and why not the peephole pass

**Not in `c4opt.lisp` / `opt.f`.** Those work on the labelled
instruction list, and c4's calling convention makes inlining
inexpressible there: a parameter is read with `LEA n; LI`, which is
**bp-relative**, and a body spliced into a caller sits at a different
stack depth with no frame of its own. Rewriting those reads needs a
frame slot to spill into, and allocating one is exactly what the
peephole layer cannot do. (This is also why the ten fused opcodes were
expressible there and this is not: fusion never moves an instruction
between frames.)

**In the tree pass**, as **T3**, beside the two AST passes both
compilers already mirror:

| | c4lc (Lisp) | c4fc (Forth) |
|---|---|---|
| T1 constant folding | `c4lc-tree.lisp` | `tree.f` |
| T2 dead function elimination | `c4lc-tree.lisp` | `parse.f` |
| **T3 inlining** | **`c4lc-tree.lisp`** | **`tree.f`** |

Before code generation there is no frame yet, so the code generator
allocates the caller's slots for whatever T3 splices in and the whole
problem disappears.

**T3 runs before T2**, so that T2 sees the call graph as inlining left
it. What T2 may then do with a function whose callers all vanished is
the subject of "The linking rule" below, and the answer is *not* the
obvious one.

### This is one implementation, not three

`src/c4sc/c4tree_gen.c` is **generated** by c4sc from
`src/c4sp/lisp/c4lc-tree.lisp` ("do not edit" is the first line), so the
C-hosted compiler gets T3 by regeneration. Only two hand-written
copies exist: the Lisp and the Forth.

*(Naming note: `c4sc` is not a port of `c4fc`. c4fc is the C99 compiler
written in Forth; c4sc is a Lisp→C compiler that turns c4lc's Lisp into
C. They are unrelated except that both end up with a C-hosted compiler.
`src/c4fc/opt.f` IS a hand port of `c4opt.lisp`, which is probably the
resemblance worth remembering.)*

## The rules

Conservative on purpose: the first version must not be able to change
what a program means, and every relaxation after it should be paid for
with a measurement.

**A function is a candidate when all of:**

- not variadic, not `main`, not a constructor or destructor;
- its address is never taken (`fnaddr`) anywhere in the program;
- it declares no locals;
- its body is exactly one statement, either `(return E)` or `(expr E)`;
- `E` contains no `call` — so it is a leaf, and one pass cannot cascade;
- `E`'s node count is at or under the threshold.

**A call site is inlinable when all of:**

- argument count matches parameter count;
- every argument is side-effect free — no `call`, no assignment, no
  `++`/`--` anywhere inside it;
- any parameter used **more than once** in the body has an argument that
  is a literal or a plain variable, so duplicating it is free and cannot
  read a different value the second time.

**`(expr E)` bodies inline only in statement position**, `(return E)`
bodies only in expression position. A void function's body is not a
value and must not become one.

## Ladder

- [x] **I1** Candidate analysis in `c4lc-tree.lisp`: which functions
      qualify, and why each rejected one did not. Visible under a flag
      so the answer can be read rather than inferred.
- [x] **I2** Substitution at call sites for `(return E)` bodies.
- [x] **I3** Statement-position `(expr E)` bodies — the `__fw_putc`
      shape.
- [x] **I4** `make test-c4lc` green, and the default `-O` path still
      reports `inlined 0` — nothing that was not asked for changed. A
      correctness program covering the substitution trap, impure
      arguments, twice-used parameters and nested calls produces
      **byte-identical output** with and without `-minline` (11 sites
      inlined, 4 correctly refused).
- [x] **I5** Measured; see below. `__fw_putc` is deliberately NOT gone
      from the image — see the linking rule.
- [~] **I6** `make src/c4sc/c4tree_gen.c` regenerates cleanly and the
      pass is in the C: `_t_58inline`, `_t_58ninline`, 38,628 -> 65,621
      bytes. c4sc's bootstrap fixed point NOT yet re-run.
- [x] **T4** Multi-statement bodies with locals, statement position.
      Budget picked by measurement; C4KE, C4IX and C4DOS all build and
      run. `make test-c4lc` green.
- [x] **C4DOS** moved from c4cc to c4lc in the Makefile.
- [ ] **T4b** the jump node, for the 202 C4KE + 100 C4IX call sites
      whose bodies return early. The AST needs a forward-jump node that
      only T4 creates -- the C dialect needs no `goto` for this.
- [x] **I7** Ported to c4fc as `src/c4fc/inline.f` (+ hooks in emit.f,
      parse.f, gen.f, c4fc.f). `make test-c4fc` green, including
      `cmp .c4fc_ix.c4r c4ix.c4r` -- c4fc's C4IX is still byte-identical
      to c4lc's -- and the C4IX demo boots. Thirteen programs (the
      spike corpus plus the T3/T4/T4b correctness tests) produce
      identical output with and without `-minline`.

Tick only on evidence.

## The linking rule

**Inlining every call to a function must not be what deletes it.** This
pass sees one translation unit, and another one can still name the
function at link time. So a function T3 inlined becomes a **root** for
T2 rather than becoming garbage.

`static` is the exception, and it is C's own rule: nothing outside the
unit can refer to a static function, so if T3 took its last caller, T2
may have it. (In object mode T2 already keeps every non-static function,
so this only bites a whole-program build — which is exactly the build
where the call sites vanished.)

Verified: the firmware built with `-minline` inlines 16 call sites and
**still exports `__fw_putc`**, while the image gets *smaller* — 5,369
to 5,314 code words — because the call sites removed outweigh the body
retained.

## Results

What inlining is worth on its own, measured against the firmware as it
was before the block write, where `__fw_putc` was still called once per
emitted character:

| workload | `-O` | `-minline` | |
|---|---:|---:|---:|
| `hello32` | 522 | 480 | **−8.0%** |
| `test_printf` | 13,678 | 12,390 | **−9.4%** |
| `test_printloop` | 18,396 | 16,863 | **−8.3%** |
| `raycast-dos`, 5 frames | 3,311,952 | 3,157,665 | **−4.7%** |

That −8.3% on `test_printloop` is the number predicted by hand from the
disassembly before any of this was written — 219 emitted bytes × 7
instructions saved per call, over 18,396 — which is the best evidence
available that the pass is doing exactly the thing it was meant to and
nothing else.

**On the current firmware it is worth nothing**, and that is the
interesting part: `docs/c4bb-uart-block.md` already took the
per-character calls out of the formatter, so the 16 sites `-minline`
finds are the padding loops, which these workloads barely enter. The two
optimisations attack the same cost and the block write got there first.
Inlining keeps its value for every OTHER small function in the tree.

## The kernels

`-minline` on the two kernels c4lc builds. (C4DOS is out of reach as
things stand: `Makefile:271` builds it with **c4cc**, not c4lc, so it
has no `-minline` to give.)

| | inlined | effect |
|---|---:|---|
| **C4KE** | 2 sites | boot 927,424 -> 926,647 cycles, **-0.08%** |
| **C4IX** | 14 sites over 12 modules | not separately timed |

Both still boot and shut down clean. **That is close to nothing, and the
census says exactly why.** `-minline-why` reports T3's verdict on every
function together with how many times it is actually called, so the cost
of each RULE can be read rather than guessed:

C4KE — 190 functions, 454 call sites:

| shape | functions | call sites |
|---|---:|---:|
| 4+ statements | 122 | 303 |
| 2-3 statements **+ locals** | 18 | 82 |
| qualifies today (1 stmt, 0 locals) | 34 | 48 |
| 2-3 statements, no locals | 13 | 20 |
| 1 statement but has locals | 3 | 1 |

C4IX loses **97 call sites** to the locals rule alone across its twelve
modules.

And the functions that matter are unambiguous:

    kernel_add_cycles       called 47   locals 1   stmts 4
    cycles_difference       called 47   locals 2   stmts 4
    time_difference         called 47   locals 2   stmts 4
    install_custom_opcode   called 39   locals 1   stmts 2

Those are the four the kernel already hand-inlines with `#define`s, and
every one of them is refused for the same two reasons: **it declares
locals, and its body is more than one statement.** The conservative rule
set was written for `__fw_putc`, and a kernel is not made of
`__fw_putc`.

## T4 — SHIPPED

Splicing a whole body into statement position. Two shapes are taken:
`f(a);` (value discarded) and `x = f(a);` (the trailing `return` becomes
the assignment). A more interesting lvalue than a plain variable is left
alone -- its address would then be computed after the body ran.

**It is safer than T3, not more dangerous.** A parameter that needs one
becomes a real local initialised with the argument, so every argument is
evaluated exactly once, left to right, exactly as the call would have.
T3's purity and duplication rules exist only because it substitutes an
argument's *text*; none are needed here, and an argument may call,
assign and increment freely. `c4lc-gen` already gives a `block` its own
scope and pre-counts `declstmt` frame words, so the splice needs no
frame surgery.

Three things it got wrong first, all found by measurement:

**1. Not every parameter should become a local.** Doing that made C4KE
**8% slower**, and the arithmetic says why: a call argument is one `PSH`,
already where the callee wants it, where binding it to a local is
`LEA slot; PSH; <arg>; SI` -- three instructions more, per parameter,
per site, against the four a call costs in total. A parameter is now
bound only when it must be (impure or non-trivial argument, assigned in
the body, address taken, or used where only an identifier fits);
otherwise the argument is substituted and the parameter costs nothing.

**2. `(call NAME …)` carries an identifier that can be a LOCAL.** c4lc
resolves a call name through the ordinary symbol table, so C4KE's
`int *cb; … cb();` calls a local by name. Renaming the declaration
without renaming the call is what "undefined identifier: cb" was.
`sizeof(identifier)` is the same shape. `fnaddr` deliberately is not --
that names a function, and renaming it would break the link.

**3. A spliced block must not be walked again.** Doing so cascades: A
takes B's body, which still calls C, so C lands inside A too. Nothing
bounds that but the call graph happening to be acyclic.

### T4b — bodies that return early

A `return` in the middle of a body has to go somewhere; in the callee
that somewhere was `LEV`. Spliced, it becomes a jump to the end of the
block — **the same shape `break` already has**, lowered through the same
`JMP`-to-a-label. Two node kinds, `ijmp` and `ilabel`, that only T4
creates and only `c4lc-gen` reads.

**The C dialect gets no `goto`.** The thing that was missing was never a
source construct — it was a way for the AST to say "jump forward", and
an optimizer-only node says it without changing a line anyone writes.
The tree pass cannot name a generator label, so it names its own
*splice*, and `g:ilab` makes the mapping on first mention — which is the
jump, since the jump is always forward.

Refusals that remain: a body that mixes `return x;` with a bare
`return;` is refused in value position, because one path has nothing to
give and guessing is worse than declining. A body whose only return is
already last still gets no label at all — the common case, and free.

Effect on how much is reachable:

| | T4 only | with T4b |
|---|---:|---:|
| C4KE | 49 | **100** (51 labelled) |
| C4IX | 47 | **66** (19 labelled) |
| C4DOS | 23 | **33** (10 labelled) |

### The budget is the whole game

| budget (AST nodes) | splices | C4KE boot cycles | |
|---:|---:|---:|---:|
| — (baseline `-O`) | 0 | 927,424 | |
| 20 | 37 | 921,477 | −0.64% |
| **60** | **49** | **920,388** | **−0.76%** |
| 80 | 62 | 929,021 | +0.17% |
| 100 | 80 | 958,245 | +3.3% |
| 120+ | 97 | 978,275 | +5.5% |

Inlining is a win at a small budget and a **loss** at a large one, and
the crossover is sharp. What the bigger budgets add is mostly bodies
called once or not at all -- code growth buying nothing. The default is
**60**, chosen by this table rather than by taste.

### CORRECTION: that table measures the wrong thing

The budget table above, and the "−0.76%" it was read as showing, come
from C4KE's **self-reported** boot figure, and that figure is not a
valid A/B metric for a compiler change.

`main` computes it as `__c4_cycles() - measurement_cycles`, and
`measurement_cycles` is what `measure_ips()` burned — a **wall-clock
spin loop** (`while (elapsed_t < KERNEL_MEASURE_QUICK) elapsed_t =
__time() - last_t;`). How many instructions that burns depends on how
fast the host is and on how many instructions per iteration the loop
compiles to, so changing the compiler changes the number that gets
SUBTRACTED. It is bit-stable across runs of one binary, which is exactly
why it looked trustworthy.

Measured properly instead — on c4bb, counting instructions to produce
**identical output**:

| | instructions | bytes emitted |
|---|---:|---:|
| `-O` | 4,027,255 | 1,209 |
| `-minline` (budget 120, 232 splices) | 4,030,886 | 1,209 |

**Under a tenth of a percent apart.** The "+31%" was the calibration
loop moving, not work being done. T4/T4b is close to neutral on C4KE's
boot, in both directions — which also means the earlier "−0.76% win"
was never real either.

The default budget is **60**, chosen now for restraint rather than for a
measured optimum: it is where code growth stays modest while the bulk of
the small bodies are still taken.

### The workload benchmark

**First, the harness.** C4KE schedules on the PIT — "every 10ms of real
time" — and c4bb leaves `RTC_MS`/`PIT_MS` on the wall clock, so the
number of context switches, and every score that follows from it,
depends on how busy the host was. `Devices` takes an injectable
`hostNow`, so pinning it to the cycle counter makes a run a pure
function of the image. Verified: the same kernel now scores identically
to the digit on repeat runs, where before it wandered.

**innerbench cannot resolve a difference this small.**

| tasks | `-O` | `-minline` | |
|---:|---:|---:|---|
| 4 | 201.583k | 197.374k | inline −2.1% |
| 10 | 227.415k | 230.974k | inline **+1.6%** |

The sign flips. Per-task spread *within* one run is ±3%, wider than the
gap, because a different kernel shifts the scheduling phase and
redistributes quanta between tasks. `bench` also mixes sleeping with
running (`time_settle` calls `sleep`), so simulated time advances partly
without burning cycles and the total is not a clean efficiency measure.
This is the same conclusion the tree reached before: innerbench is too
noisy for work at this scale.

**Fixed-work jobs under C4KE are the measurement that works** — same
program, run to completion, cycles counted exactly:

| job | `-O` | `-minline` | |
|---|---:|---:|---:|
| `mandel` | 11,987,013 | 11,965,240 | **−0.182%** |
| `tests` | 4,767,069 | 4,740,206 | **−0.563%** |

Deterministic, and both produce byte-identical output ("Mandelbrot
rendered in 359ms" either way).

## The bug the benchmark found

Running innerbench at ten tasks under the wall clock, the inlined kernel
**spawned four of the ten**, reproducibly, three runs out of three,
while `-O` spawned ten. That is not a measurement artifact, and chasing
it turned up a miscompilation:

    innerbench: Spawning 10 inner benchmarks
    lc4r: unable to open '' or '.c4r'
    c4ke: task 12 () OVERRAN ITS STACK: 44 of 262144 bytes, guard broken.
    lc4r: unable to open '(null)' or '.c4r'

Task names and argv arriving empty, and a guard broken with 44 bytes
used — which the kernel's own comment says means the write came from
somewhere else entirely.

Bisected: T3 alone clean, T4 without exit labels clean, T4 **with**
them corrupt. Then bisected by budget: clean at 50 (3 labelled splices),
corrupt at 60 (51). The functions entering that range were mostly
`op_*` — C4KE's **custom opcode handlers**.

**The rule that was missing.** A handler is entered by a route that
bypasses its own prologue:

    handler = *(custom_opcodes + (ins - CO_BASE));
    __c4_adjust(*(handler - 1) * -1);   // reads the ENT operand
    __c4_jmp(handler);                  // jumps PAST the ENT

so the stored address, the word before it, and the frame it describes
are a contract with code elsewhere. Splicing changes a function's frame
and therefore its `ENT` operand — rewriting one side of that contract
only. `pm_syscall_handler` does the same thing
(`((int *)&fn) + 2 // skip ENT x`).

Taking a function's address is the only available signal for "something
else knows the shape of this function", so it now gates **both**
directions: such a function is neither inlined into callers nor spliced
into.

**And the detector itself was broken.** `&f` has two spellings, and the
guard only knew one:

    (fnaddr N)       -- &f in an INITIALISER, the only place the parser
                        makes this node (c4lc-parse.lisp:631)
    (addr (var N))   -- &f in an ORDINARY EXPRESSION, which is what
                        C4KE writes: install_custom_opcode(OP, &op_halt)

Every C4KE handler is registered the second way, so the address-taken
set came back **empty** on a kernel that takes dozens of addresses, and
the guard had never excluded anything — in T3 as well as T4. Fixed, and
C4KE's splices fall from 100 (51 labelled) to 36 (5), the corruption
goes, all ten tasks spawn, and the fixed-work numbers above get BETTER
than the miscompiled build's, which is the shape a real fix has.

**So: a real win, and a small one.** A quarter of a percent on a fixed
user workload. That is what it should be — `top` reports the kernel
taking about 2% of cycles on these jobs, so a 0.3% whole-system saving
means the kernel's own share got several percent faster, which is the
same order as the 8% measured on the firmware. Inlining a kernel cannot
beat the fraction of time spent in the kernel, and on C4KE that fraction
is small.

## The kernels, with T4

| | result |
|---|---|
| **C4KE** | 49 splices, boot **920,388** vs 927,424 (**−0.76%**), clean shutdown, correct output |
| **C4IX** | **47** bodies spliced across 12 modules (was 14 with T3 alone) |
| **C4DOS** | now built by c4lc; 23 splices available |

### C4DOS moved to c4lc

`Makefile` now builds all three C4DOS images with c4lc instead of c4cc.
It is the base rung and nothing below it compiles it, so what does is a
free choice -- and c4lc `-O` is simply better:

- 32-bit image **55,752 -> 53,915 bytes**
- boot **851,400 -> 851,184 cycles** (851,154 with `-minline`)
- console output **byte-identical** to the c4cc build

## What the kernels needed (superseded by T4 above)

Inlining a multi-statement body with locals is a different pass, not a
loosened constant:

- it can only go in **statement position** — a block is not a value;
- the callee's locals must be **renamed into the caller's frame**, and
  its parameters bound to fresh locals, since a body may assign to them;
- and the hard one: **an early `return` needs a jump target.** This
  dialect has no `goto`, so only a body whose sole `return` is its last
  statement can be spliced without one.

That last constraint may still cover the four functions above — each is
2-4 statements and looks like "compute, return". Worth measuring before
building.

## I7 — the c4fc port, and why it is smaller

Two facts about c4fc made this a third of the job the design predicted:

**`GEN` is read-only over the AST** — not one store into a node in
`gen.f` or `emit.f`. The plan had been a `GENERIC: COPY` with a method
per node kind, because Forth nodes are mutable and splicing one body
into N sites would alias them. Unnecessary: a body is spliced by
pointing the callee's symbols at fresh slots in the caller's frame and
generating the *same tree* again. No copy, no rewrite, no substitution.

**c4's result lives in A**, so a spliced `return E` leaves E exactly
where the call would have. It only has to jump instead of `LEV` — one
flag and one branch in `STMT n_ret`, where c4lc needed the whole T4b
pass with two AST node kinds of its own. And because the value arrives
the same way, **a c4fc splice works in expression position**, which
c4lc's T4 cannot do.

Three places carry it, all one-liners because c4fc emits as it parses:

| | |
|---|---|
| `parse.f` n_fnref | the ONE place a function name becomes a value, so address-taken is recorded there with no walker |
| `parse.f` function end | a candidate keeps its symbols (`y.nlen` blanked so `ST-FIND` cannot match them) because its tree points at them |
| `gen.f` n_call / n_ret | the splice, and the return-becomes-jump |

`ENT` is emitted before the body, so a splice that grows the frame is
too late to change it — the operand is patched afterwards at
`entp+1` rather than pre-scanned, which is exact and cheaper than
c4lc's scan.

### The bug this port found on its own

`sq(sq(2))` returned **4** instead of 16. An argument can contain a call
that is itself spliced, and when it is the *same* function the inner
splice retargets the very same symbols — so binding `y.val` before
evaluating the arguments has the inner splice overwrite the outer one's
slots, and the outer body then reads the inner's. Reserving slots and
binding them *after* the arguments are evaluated is what makes nesting
safe. c4lc never hit this because it substitutes argument expressions
rather than binding slots.

### The budget is inherited, not measured

c4fc's is in TOKENS (the span the parser consumed) rather than AST
nodes, because the token cursor is already a size and needs no walk.
Default **40**, chosen as the same neighbourhood as c4lc's measured 60
nodes — c4fc has no speed benchmark of its own and `docs/c4fc-design.md`
is explicit that it is not a speed play, so inheriting a measured figure
and saying so beats inventing one. What IS measured is the cost of
getting it wrong: on the stress corpus the image goes 9,502 bytes at
budget 24, 13,469 at 32, 16,029 at 60. This grows code quickly.

## Still to do

- **I6** c4sc's bootstrap fixed point (the regeneration itself is done
  and verified).
- **I7** the port to `src/c4fc/tree.f`, and c4fc's spike suite and
  self-hosting fixed point.
