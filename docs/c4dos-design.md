# C4DOS — design notes

A single-tasking, trap-free disk operating system for the C4 family,
in the spirit of classic DOS: `CONFIG.SYS`, `AUTOEXEC.BAT`, a prompt,
transient programs that load, run, and return. Written for the
HOMEWARD game's bring-up campaign (the OS you can run BEFORE building
trap machinery), but a first-class citizen of this repo like every
other subsystem.

STATUS: in development, 2026-08-21. This doc is the contract; deviations
get written back here.

## Why it exists

c4lm ("CALM") attempted a microkernel on plain c4 and is on hold with a
root-caused context-switch bug (see internals.md Part 6 — `LEV`
re-derives `sp` from the *outgoing* `bp`). C4DOS deliberately wants
none of that: ONE task, no scheduler, no preemption. Everything it
needs is either plain-c4 or ordinary c4m microcode on c4bb:

- `OPEN/READ/CLOS` — the disk (read side).
- `PRTF/PUTC/PUTS` — the console.
- `MALC/FREE` — the one shared heap (c4bb firmware allocator; no
  preemption means its non-atomicity is irrelevant here).
- `TIME/C4CY` — real microcode on c4bb, NOT jsops. The clock C4 never
  had; gated behind `DEVICE=CLOCK.SYS` so a clockless build stays
  pure-c4 runnable.
- NO `ITH/C4CF/SIGH/SIGI/_TRP/DBG` — the jsop set stays untouched.

## The pieces

```
src/c4dos/
  cpp.c              # THE PREPROCESSOR (standalone pass; see below)
  c4dos.c            # shell: CONFIG.SYS, AUTOEXEC.BAT, prompt, builtins
  c4dos_load.c       # .c4r loader (c4ix/loader.c structure: v3 MEMSZ)
                     #   + c4l.c invoke-stub call mechanism (plain-c4 safe)
  include/c4dos.h    # transient-side API shim: exit(), dos_* services
  fs/CONFIG.SYS      # curated boot config, copied onto the disk
  fs/AUTOEXEC.BAT
  tests/build-images.sh
  tests/test-c4dos.sh
docs/c4dos-design.md # this file
```

## The preprocessor (cpp.c)

c4cc has NO preprocessor — `#` lines are skipped by its lexer; builds
lean on `gcc -E`, and c4lc's L9 is a Lisp program (far too slow under
nested interpretation). The self-hosting ladder (C4DOS builds C4KE,
C4KE builds C4IX, on the machine) therefore needs an in-machine cpp.

Decision: a STANDALONE pass, DOS-era style — `CPP FOO.C > FOO.I`,
then `C4CC FOO.I`. Batch files drive the pipeline; no compiler
integration required; native builds can use the same binary in place
of `gcc -E`.

- Dialect: the STRICT c4 subset (c4l.c's rules — no switch/break/
  for/struct, locals at top, single-word globals), so the full tower
  holds: `./c4 cpp.c file.c` works on the original interpreter.
- Features (scoped by a survey of the actual tree — nothing in
  include/ or the kernels uses more): `#include` (quoted + angle, -I
  paths), `#define` object-like and function-like (parameter
  substitution, recursive rescan with self-reference guard), `#undef`,
  `#ifdef/#ifndef/#if/#elif/#else/#endif` with a constant-expression
  evaluator (numbers, identifiers→macros→0, `defined()`, ! ~ - * / %
  + - << >> comparisons & ^ | && ||), `-D NAME[=VAL]` predefines.
  NOT implemented (unused in tree, error loudly): `##`, `#`
  stringize, backslash continuations, `#include_next`, variadic
  macros, `#pragma`.
- Comments are stripped from macro BODIES (a `//` in a body would eat
  the rest of every expansion site) and passed through everywhere
  else; expansion never fires inside strings, chars, or comments.
- Output: stdout (like `gcc -E`). Line markers: none (`-P` style);
  the verification pin is on COMPILED IMAGES, not preprocessed text.
- Verification: for each corpus file, `cpp | c4cc` vs
  `gcc -E -P | c4cc` must produce byte-identical .c4r images (the
  same pin style c4lc's L9 used).

## Program loading and return

The loader is `src/c4ix/loader.c`'s parser (v3 MEMSZ-aware — c4l.c is
NOT; it predates BSS) + `c4l.c`'s invoke stub for the actual call, so
DOS itself stays plain-c4 clean while running any image the MACHINE
can execute (on c4bb that includes JSRI/JMPA programs; DOS only scans
and refuses when the underlying machine is plain c4).

**`exit()` is the hard part**: the `EXIT` opcode HALTS the machine on
c4bb (POWER latch) and ends the VM natively; C4KE survives it only by
trapping. Trap-free DOS instead ships `include/c4dos.h` whose `exit()`
performs a non-local return using the double-`LEV` trampoline proven
in `src/tests/test_coop_switch.c`: DOS saves its frame pair before
`invoke2`, the transient's `exit()` restores it and returns THROUGH
the trampoline — control lands at the RUN call site as if main had
returned. Programs that simply `return` from main need nothing.

Transients find DOS via the systable pattern (`src/c4ix/loader.c
loader_systable`): the loader scans the image's symbol section for
`__c4dos_api` and writes the service-table address into it. One
binary can run under C4DOS, C4KE, or bare — the shim checks the slot.

## Files, writing, and the RAM disk

c4/c4m have NO file-write primitive (READ only). Writing therefore is
a DOS SERVICE: an in-memory RAM disk (name → buffer table) reachable
through `__c4dos_api`, DOS-style `DEVICE=RAMDISK.SYS`. `DIR` lists
the union of the host/c4bb disk (read-only) and the RAM disk (rw);
opens check the RAM disk first. This is what makes the self-hosting
ladder possible in-machine: compiler outputs land on the RAM disk,
and the next stage reads them back. (In HOMEWARD, persistent disk
write arrives later as built hardware; the RAM disk is honest about
what the machine can do.)

DECISION (user, 2026-08-21): NO `>` output redirection. Compilers and
tools write files DIRECTLY through the API (`dos_create`/`dos_write`/
`dos_close` slots) instead of a captured console — simpler, and
anything printf-shaped can be done in software where needed (the c4lm
stdio.h vsnprintf is sitting right there when a tool wants it). Batch
stays a command list, not a shell language.

## CONFIG.SYS and AUTOEXEC.BAT

- `CONFIG.SYS`: `DEVICE=CLOCK.SYS` (enables TIME/C4CY use),
  `DEVICE=RAMDISK.SYS SIZE=n`, `FILES=n`, `SHELL=...` (reserved).
  Parsed with a flattened-locals descent (the vfsload.c skeleton,
  c4cc-dialect).
- `AUTOEXEC.BAT`: line-per-command batch, `ECHO`, `REM`, `@` prefix,
  run through the same reader as the interactive prompt (the c4sh
  INPUT_STDIN/INPUT_STREAM split, minus its known multi-line-read bug).
- Builtins: `DIR`, `TYPE`, `RUN` (implicit for *.C4R names), `ECHO`,
  `VER`, `TIME` (wants CLOCK.SYS), `MEM`, `EXIT` (halts — the one
  legitimate use of the EXIT opcode).

## Testing

- `make test-c4dos`: build 32-bit images, boot under c4bb
  (`sim/cli.js`), pipe a scripted session, grep-assert (the
  test-c4bb.sh pattern); plus native `./c4m c4dos-boot` parity where
  output interleaving allows.
- Purity pin: `./c4 c4l.c c4dos.c4r` runs the CLOCKLESS build (the
  test-c4l pattern) — regression guard against extended opcodes
  creeping into DOS core.
- cpp pin: image byte-equality vs the gcc -E path over the corpus.

## The ladder (for HOMEWARD)

samples → C4DOS (this) → C4DOS runs cpp+c4cc to build C4KE → C4KE
(traps, preemption) hosts c4sp + c4lc → c4lc builds C4IX (protected
mode) → network coprocessor sub-board + CPU support → C4IX rebuilt
with networking → the time machine's control link (c4mp direction;
SMP is a stretch goal).
