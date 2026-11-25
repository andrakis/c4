# Vendored Forth-2012 test suite

`tester.fr` and `core.fr` are taken verbatim from Gerry Jackson's
Forth 200x / Forth-2012 test suite:

  https://github.com/gerryjackson/forth2012-test-suite
  src/tester.fr, src/core.fr — fetched 2026-08-23

`tester.fr` is John Hayes' original tester (C) 1995 Johns Hopkins
University / Applied Physics Laboratory, "may be distributed freely as
long as this copyright notice remains" — it does, at the top of the file.

**Do not edit them.** They are the oracle: the whole point of using a
standards suite is that it was not written by us and does not know what
c4th happens to implement. Anywhere c4th deliberately diverges from the
standard, the divergence is recorded in `docs/c4th-design.md`, not
patched into these files.

They are self-verifying — a test reads `T{ 1 2 + -> 3 }T` and prints only
on failure — so they need no reference Forth to compare against.


---

## fuzz.f — the differential fuzzer

`b5.f` is the cases somebody thought of. `fuzz.f` generates the ones
nobody did: random definitions built from a table of operations, each run
on the threaded engine — the oracle, since it is what passes the
Forth-2012 CORE suite — and then compiled and **called**, with the
answers required to agree.

Every definition is balanced by construction: the generator tracks the
compile-time depth and only picks an operation the depth can afford, and
every block pads or drops back to the depth it started at. A definition
that underflows, or whose `IF` arms leave different depths, would test
the backend's *refusal* rather than its code. It generates `IF`,
`IF/ELSE`, counted loops with `I`, and `@`/`!`/`+!` on a single scratch
cell.

The seed is fixed, so a failure is the same failure tomorrow and on the
other host, and a mismatch prints the offending definition's opcode
sequence. `make test-c4th` runs 2000 definitions with the fused opcodes
and 2000 without. For a deeper run:

    ./c4m load-c4r.c -- c4th.c4r src/c4th/forth/core.f \
        src/c4th/forth/asm.f src/c4th/forth/native.f \
        src/c4th/tests/fuzz.f -e '999 SEED ! 15000 FUZZ'

**It has caught two things so far.** A deliberate one-character change to
`-ROT`'s permutation turns 0 mismatches into 8 — a fuzzer that cannot
fail is decoration. And a real bug that shipped at B5b: a permutation
rebuilding the top regions assumed the cell before each region was the
`PSH` that spilled the one below, which is untrue when that item was
already on the stack. It wrote `PSH` over an `ADJ`'s operand. See
`docs/c4th-design.md`.
