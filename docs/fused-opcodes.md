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

Twenty-five opcodes at **79 and up**, the first numbers free after c4mp's
66-78 (`CPUI..TRAW`). Eight fused memory and stack forms, the accumulator
pop c4th's backend wants, and the complete immediate-ALU family.

| | | replaces |
|---|---|---|
| `LDL n`  | `a = *(bp+n)` | `LEA n; LI` |
| `LDG n`  | `a = *(int *)n` | `IMM n; LI` |
| `PSHL n` | `a = *(bp+n); *--sp = a` | `LEA n; LI; PSH` |
| `PSHG n` | `a = *(int *)n; *--sp = a` | `IMM n; LI; PSH` |
| `LEAP n` | `a = (int)(bp+n); *--sp = a` | `LEA n; PSH` |
| `IMMP n` | `a = n; *--sp = a` | `IMM n; PSH` |
| `LIP`    | `a = *(int *)a; *--sp = a` | `LI; PSH` |
| `ADDL`   | `a = *(int *)(*sp++ + a)` | `ADD; LI` |
| `POPA`   | `a = *sp++` | `IMM 0; ADD` |
| `OPI n`  | `a = a OP n`, for all sixteen ALU opcodes | `PSH; IMM n; OP` |

**The immediate family is complete rather than trimmed to what the
measurement shows.** A partial family means the code generator carries a
list of which operators have an immediate form and a fallback for the
rest, and that asymmetry is where bugs live; a complete one is the single
rule "a binary operator whose right operand is a constant". The VM cost of
the ones that never fire is one line each.

**Every operand is one word**, so nothing that walks code has to learn a
new instruction shape — only which opcodes carry an operand. `LDG` and
`PSHG` take a data address, so their operand goes in the `.c4r` patch
table exactly as the `IMM` they replace did.

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
rather than worked around: **an image built with fused opcodes does not
run under plain `c4`**, which is why emission is behind a flag and off by
default. `c4or1k`'s `-mcisc` (`docs/c4or1k-design.md`) is the precedent.

**c4bb is deliberately out of scope**, per the standing instruction to
leave it until this is proven on the main toolchains.
`src/c4bb/hw/microcode.uc` implements each opcode individually, so this is
hardware-description work, not a table edit, and it should follow a
decision to turn the flag on by default rather than precede it.

## Ladder

- [x] **F0** This document, before any code
- [x] **F1** `c4m` implements all twenty-six. *Verified:* c4th's B5
      suite (63 words compiled, called and compared against the threaded
      engine) and its fuzzer (2000 random definitions) produce
      transcripts **byte-identical** with the backend emitting them and
      without. Plus `src/c4th/tests/fused.f`, which checks each opcode
      against the sequence it replaces at the VM level: hand-assemble
      both, call both, require agreement. All twenty-six, zero
      mismatches, pinned in `make test-c4th`.
- [x] **F2** Numbering and names mirrored everywhere above, and one
      rule per host decides which opcodes carry an operand
      (`c4m_has_operand`, `c4_has_operand`, `has_operand`,
      `c4r:has-operand`). *Verified:* every suite unchanged.

      Two pre-existing gaps closed on the way, both of the same kind:
      `c4l.c`, `load-c4r.c` and `oisc4.c` had no names at all for
      c4mp's 66-78, and `c4cc.c` was missing `TRAW` at 78 — and all four
      index those tables **with no bounds check**. Disassembling or
      refusing a c4mp image read past the end.

      `c4m`'s 66-78 are now c4mp's real names rather than `RS66..RS78`
      placeholders, so all seven tables are literally the same list. The
      visible consequence: `__opcode("CPUI")` now answers 66 on c4m
      instead of -1. Nothing in the tree looks opcodes up by name, and
      a guest is supposed to feature-test with `C4I_SMP`.
- [x] **F3** `c4mp` and `oisc4` execute them. c4mp is twenty-six switch
      cases; oisc4's expansions really are concatenations of the ones it
      already had, with the self-patch offsets recounted from each
      block's start.

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
