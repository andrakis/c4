# Fused opcodes: taking set B across the board

## Why

`docs/c4th-design.md`'s B5c opcode probe measured, on the instructions two
real workloads *actually execute*, how many a set of fused opcodes would
remove. The answer was about a third:

| workload | instructions | removed |
|---|---|---|
| `c4cc` compiling `c4.c` | 11,373,735 | **36.12%** |
| `c4sp -R` running c4lc's lexer over `c4.c` | 1,639,556,989 | **37.47%** |

That is not a c4th number. `c4sp` is what hosts `c4lc`, so the second row
is the compiler this whole track exists to make faster, and the first is
the C compiler everything else is built with.

`sh src/c4th/bench/fuse-probe.sh` reproduces it and breaks the total down
per rule, which is what fixed the opcode list below: the mirror cost is
paid **per opcode**, so a rule that earns nothing is not worth a number.

    rule                          c4cc     c4sp
    PSHL  (LEA LI PSH)            2.76%   17.24%
    PSHG  (IMM LI PSH)           19.46%    1.35%
    IMMP  (IMM PSH)               7.52%    1.01%
    LEAP  (LEA PSH)               0.43%    3.91%
    ADDL  (ADD LI)                3.41%    3.16%
    LDL   (LEA LI)                0.35%    3.20%
    LIP   (LI PSH)                0.59%    1.37%
    LDG   (IMM LI)                0.55%    1.08%
    ADDI  (PSH IMM ADD)           0.00%    2.38%
    EQI   (PSH IMM EQ)            0.23%    1.19%
    SHLI  (PSH IMM SHL)           0.00%    0.98%
    GTI   (PSH IMM GT)            0.00%    0.58%
    everything else              < 0.4%   < 0.4%

The two workloads disagree about *which* rules pay — `c4cc` reads globals
and `c4sp` reads locals — which is the argument for taking both halves
rather than the top of either list.

## What

**Ten opcodes at 79-88**, the first numbers free after c4mp's 66-78
(`CPUI..TRAW`), and **split across two machines**:

| | | replaces | c4m | c4mp |
|---|---|---|---|---|
| `LDL n`  | `a = *(bp+n)` | `LEA n; LI` | ✓ | ✓ |
| `STL n`  | `*(bp+n) = a` | `LEA n; PSH; …; SI` | ✓ | ✓ |
| `POPA`   | `a = *sp++` | `IMM 0; ADD` | ✓ | ✓ |
| `LDG n`  | `a = *(int *)n` | `IMM n; LI` | | ✓ |
| `PSHL n` | `a = *(bp+n); *--sp = a` | `LEA n; LI; PSH` | | ✓ |
| `PSHG n` | `a = *(int *)n; *--sp = a` | `IMM n; LI; PSH` | | ✓ |
| `LEAP n` | `a = (int)(bp+n); *--sp = a` | `LEA n; PSH` | | ✓ |
| `IMMP n` | `a = n; *--sp = a` | `IMM n; PSH` | | ✓ |
| `LIP`    | `a = *(int *)a; *--sp = a` | `LI; PSH` | | ✓ |
| `ADDL`   | `a = *(int *)(*sp++ + a)` | `ADD; LI` | | ✓ |

**Why three in c4m.** `LDL`, `STL` and `POPA` are what a stack-machine
code generator needs to treat the frame as registers, and c4th's native
backend uses those three and nothing else. Without them `>R`, `DO`/`LOOP`
and every spill cost three instructions instead of one. They are worth
about **twelve microsteps** on c4bb.

**Why the other seven are c4mp's.** They are compiler-shaped — they pay
for `c4cc`'s and `c4lc`'s output, not for a Forth's. c4m is the workhorse
that boots C4KE and the machine c4bb models in hardware, and there each
opcode is microcode and ROM depth on a breadboard. Keeping the seven in
c4mp is what lets a **c4mp-capable c4bb be an extension of the base
board** rather than a second one — the base machine boots C4KE, and the
extension makes the same software run faster.

### The immediate-ALU family, and why it is not here

An earlier draft had twenty-six opcodes: these ten plus the complete
immediate-ALU family (`PSH; IMM n; OP` → `OPI n`, sixteen of them). The
argument was code-generator uniformity — a partial family means the
compiler carries a list of which operators have an immediate form.

**Measured, that family is worth 1.0% of `c4cc`'s executed instructions
and 5.2% of `c4sp`'s.** The ten above are 35.1% and 32.3% on their own.
Sixteen opcodes, and on c4bb sixteen microcode routines to write and
verify, for the last twentieth. The uniformity argument also evaporates
once there are *no* immediate forms: there is nothing to be asymmetric
about, and the fuse pass gets smaller.

It stays measurable rather than settled: `sh src/c4th/bench/fuse-probe.sh`
still lists every candidate rule, so if a workload ever wants them the
case can be made from data.

## What this costs c4bb

The question this whole shape answers. `src/c4bb/hw/microcode.uc` is 549
lines / **319 microsteps** across 65 opcodes today.

* **No new circuitry.** Every fused opcode is a *concatenation of
  transfers the board already performs* — same registers, same ALU, same
  bus. The two control patterns needed are already in use:
  `ALU_OUT MAR_IN` at line 223 and `ALU_LA` at 355.
* **No second implementation.** `src/c4bb/sim/turbo.js` compiles the same
  assembled step tables into JS, so writing the microcode gets the fast
  engine free; the lockstep test pins step ≡ turbo, and `test-c4bb.sh`
  anchors both against native c4m.
* **The base board: three opcodes, about twelve microsteps** (+4%), plus
  three names and an `INS_SIZE` bump in `src/c4bb/sim/{devices,machine}.js`.
* **The c4mp extension: seven more, about thirty-four microsteps.**

What actually grows is ROM — dispatch depth and step count — which on a
breadboard is chips rather than gates. Worth knowing before starting:
c4bb's `INS_SIZE` is **66**, so it does not have c4mp's `CPUI..TRAW`
either; a c4mp-capable c4bb is already a larger job than these seven.

## Where it has to be mirrored

Opcode numbering in this tree is append-only and mirrored, and c4mp's own
header says so. The numbers and names must agree in:

- `c4m.c` — enum, name string, dispatch
- `src/c4mp/c4mp.h`, `src/c4mp/vm.c` — enum, name string, switch
- `src/oisc4/oisc4.c` — enum, name string, `has_operand`, `o4_size`,
  `translate_one`
- `load-c4r.c` — the loader's own copy
- `c4l.c` — its copy, and `scan_extended`, which refuses anything plain
  c4 cannot execute
- `src/c4cc/c4cc.c` — the name table `c4rdump` indexes **with no bounds
  check**, so this one is not cosmetic
- `src/c4sp/lisp/c4r.lisp` — `c4r:ops`

**`c4.c` is not on that list and is not touched.** It is the base VM and
the reference every layer degrades to. The consequence is stated plainly
rather than worked around: **an image built with fused opcodes runs under
c4mp and oisc4, and nowhere else** — not plain `c4`, and not `c4m` unless
the only fused opcodes in it are `LDL`, `STL` and `POPA`. That is why
emission is behind a flag and off by default. `c4or1k`'s `-mcisc`
(`docs/c4or1k-design.md`) is the precedent.

**And a machine must fail cleanly on an opcode it lacks.** It did not.
`c4m` trapped `TRAP_ILLOP`, found no handler, printed "missed a trap" and
*carried on* — with `pc` past the opcode but not past its operand, so it
executed a data word and walked off into memory. A segfault with no
explanation is a poor way to discover an image was built for c4mp, and
c4m now meets such images on purpose.

The fix had to be narrow, because trap-and-continue is load-bearing
elsewhere: a program raises a custom opcode a kernel emulates, and
running it with no kernel is *expected* to print the missed trap and
carry on — `src/oisc4/test-oisc4.sh` filters exactly that, which is how
the first, too-broad fix was caught. So the rule is now:

* an opcode **in c4m's table** that c4m does not implement is a machine
  mismatch — name it and halt;
* an opcode **outside the table** is a custom opcode — trap, and behave
  exactly as before.

**c4bb is deliberately out of scope**, per the standing instruction to
leave it until this is proven on the main toolchains.
`src/c4bb/hw/microcode.uc` implements each opcode individually, so this is
hardware-description work, not a table edit, and it should follow a
decision to turn the flag on by default rather than precede it.

## Ladder

- [x] **F0** This document, before any code
- [x] **F1** The opcodes, and who implements them. *Verified:* c4th's B5
      suite (63 words compiled, called and compared against the threaded
      engine) and its fuzzer (2000 random definitions) produce
      transcripts **byte-identical** with the backend emitting them and
      without — under plain `c4m`, since the three it uses are the three
      c4m has. Plus `src/c4th/tests/fused.f`, which checks each opcode
      against the sequence it replaces at the VM level: hand-assemble
      both, call both, require agreement. All ten under c4mp, zero
      mismatches, pinned in `make test-c4th`.

      The split is pinned from both sides: `test-c4th` requires c4m to
      *run* the three, and `test-fuse` requires c4m to *refuse* an image
      using the other seven.
- [x] **F2** Numbering and names mirrored everywhere above, and one
      rule per host decides which opcodes carry an operand
      (`c4m_has_operand`, `c4_has_operand`, `has_operand`,
      `c4r:has-operand`). *Verified:* every suite unchanged.

      Three pre-existing gaps closed on the way. Two of a kind:
      `c4l.c`, `load-c4r.c` and `oisc4.c` had no names at all for
      c4mp's 66-78, and `c4cc.c` was missing `TRAW` at 78 — and all four
      index those tables **with no bounds check**. Disassembling or
      refusing a c4mp image read past the end.

      `c4m`'s 66-78 are now c4mp's real names rather than `RS66..RS78`
      placeholders, so all seven tables are literally the same list. The
      visible consequence: `__opcode("CPUI")` now answers 66 on c4m
      instead of -1. Nothing in the tree looks opcodes up by name, and
      a guest is supposed to feature-test with `C4I_SMP`.

      And the third, found by this work rather than inherited from it:
      c4m had **no safe failure mode for an opcode it does not have** —
      see the note under "Where it has to be mirrored".
- [x] **F3** `c4mp` and `oisc4` execute all ten; `c4m` executes three.
      c4mp is ten switch cases; oisc4's expansions really are
      concatenations of the ones it already had, with the self-patch
      offsets recounted from each block's start.

      One trap found while doing it: oisc4's syscall catch-all was
      `op >= OPEN && op < INS_MAX`, so *extending the enum* would have
      made every appended opcode look like a syscall and silently
      mistranslate. It is now bounded by `DBG`, which is where that class
      actually ends. `make test-oisc4` green.

      `test-c4mp` still fails on `raycast: output differs` — verified
      pre-existing by building HEAD in a clean worktree and reproducing
      it there.
- [x] **F4/F5** Emission, as a `fuse` pass in **`c4opt`** rather than in
      either compiler.

      That is the change of plan worth explaining. c4cc emits into a code
      array *and* a parallel recorder, at dozens of sites; teaching both
      to fuse would have been dozens of edits and a rebuild of the patch
      table. `c4opt` already operates on the **labelled instruction list**
      `c4r.lisp` decodes, where a fusion is pure list manipulation — no
      address arithmetic, no patch-table surgery, because the labels are
      the addresses. One implementation, and it applies to any `.c4r`
      whichever compiler produced it.

      It runs **once, after the main pipeline reaches its fixpoint**, so
      no other pass has to understand the fused forms and so fusing never
      hides a fold from the round after. Longest window first, so
      `LEA;LI;PSH` becomes `PSHL` and not `LDL;PSH`. A window never
      crosses a label, for the same reason the other passes are safe: a
      label is not an opcode name, so it cannot match.

      Two ways in: `c4lc -mfuse` (which implies `-O`), and
      `c4sp c4opt-run.lisp -mfuse in.c4r out.c4r` for an image from any
      compiler, c4cc's included.

      *Verified* by `make test-fuse`: a real image, fused, must behave
      identically under **c4m, c4mp and oisc4** — and be refused **by
      name** under plain c4, which is what the mirrored tables bought.
      Plus a wider differential: the fused `c4sp.c4r` reproduces the
      native `c4sp` output for **all 27** files in `src/c4sp/lisp/`.

- [x] **F6** What it was all for.

      | workload | instructions | fused | |
      |---|---|---|---|
      | `c4sp -R`, c4lc's lexer over `c4.c` | 1,639,556,989 | 1,025,451,397 | **−37.5%** |
      | `c4cc` compiling `c4.c` | 11,393,181 | 6,514,076 | **−42.8%** |
      | C4IX, full demo boot (kernel only) | 6,924,837 | 4,834,738 | **−30.2%** |

      Wall clock under `c4m`, by hyperfine: the c4lc lexer **1.56x**
      faster (6.80s → 4.37s), `c4cc` **1.64x** (57.7ms → 35.2ms). Images
      shrink too — `c4cc.c4r` 412,942 → 323,446 bytes (−21.7%),
      `c4sp.c4r` 243,957 → 199,997 (−18.0%).

      The measurement predicted 36-37% and the pass delivered 37.5% and
      42.8%. That is not luck: the probe and the pass use the same greedy
      left-to-right rule, so the only place they can disagree is where
      the pass refuses to cross a label.

### What the split costs

Dropping sixteen opcodes and moving seven cost less than it sounds.

| | c4m unfused | best available |
|---|---|---|
| c4lc's lexer, all 26 on c4mp | 6.88 s | 3.71 s (**1.83x**) |
| c4lc's lexer, these 10 on c4mp | 6.88 s | 3.96 s (**1.74x**) |

c4th, whose backend only ever used three of them, keeps most of its win
from `LDL`/`STL`/`POPA` alone:

| | threaded | +3 opcodes | (all 26 gave) |
|---|---|---|---|
| `DO`/`LOOP` with `I` | 1x | **28x** | 34x |
| `BEGIN`/`WHILE` + `>R`/`R>` | 1x | **51x** | 63x |
| loop calling another word | 1x | **59x** | 66x |
| loop through a `VARIABLE` | 1x | **42x** | 47x |

And two things that are *not* costs, measured rather than assumed:

* **Adding opcodes to c4m's dispatch is free.** Same unfused workload,
  c4m before any of this versus c4m now: **6.828 s vs 6.834 s, 1.00x.**
  So "it slows the base VM down" is not an argument against; "it is
  microcode and ROM on a breadboard" is, which is why the split is where
  it is.
* **Running under c4mp is not a penalty.** c4mp is already 1.13x faster
  than c4m on this workload, so moving the seven there costs nothing to
  reach — it is the fastest configuration in the tree either way.

### What is left

**About 6% is still on the table**, and it is all the same thing: after
fusing, the probe still reports ~6% removable on both `c4cc` and the
C4IX kernel. Those are pairs that are *dynamically* adjacent but have a
label between them statically, and the pass will not fuse across a label
because something might branch to the second instruction. Many of those
labels are targets of nothing — c4opt has no pass that drops an
unreferenced label. That pass, and then re-running fuse, is the obvious
next increment.

**Turning `-mfuse` on by default is a separate decision**, and it needs
c4bb first: the moment the default build fuses, C4IX stops running on
the breadboard. Everything above is opt-in, so nothing is blocked on
that.

**Userland is unfused.** The C4IX number above fuses only the kernel;
the programs it runs are ordinary images. Fusing those too is a Makefile
change once the default question is settled.
