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

- [ ] **F0** This document, before any code
- [ ] **F1** `c4m` implements all twenty-five. *Verify:* c4th's B5 suite
      and its fuzzer, with the backend emitting them, produce transcripts
      byte-identical to the ones without — same answers, fewer
      instructions. Every existing suite unchanged.
- [ ] **F2** Numbering and names mirrored everywhere above. *Verify:*
      every suite unchanged; `c4rdump` disassembles a fused image without
      reading past its table; `c4l` refuses one by name rather than
      crashing.
- [ ] **F3** `c4mp` and `oisc4` execute them. oisc4's expansions are
      concatenations of the ones it already has, so the check is that a
      fused image and its unfused twin produce identical output under
      both.
- [ ] **F4** `c4cc -mfuse`. *Verify:* the default build is
      **byte-identical** to today's; the `-mfuse` build of the same source
      behaves identically and executes measurably fewer instructions.
- [ ] **F5** `c4lc -mfuse`. Same bar, plus the whole C4IX build.
- [ ] **F6** Measure what it was all for: C4IX build time, `c4lc` under
      `c4m`, `c4cc` compiling itself.
