# Compiler speed — c4m memory opcodes, c4sp cheap wins, and c4lcc

Tracker for the work that makes C4IX build fast. Companion to
`docs/c4th-design.md` (a separate, independent track).

**Rule for this document:** tick a box only when its verification command has
actually been run and is green, and paste the one-line evidence beside it.
Anything discovered mid-implementation that changes a later milestone gets
written in here, not just said in conversation — including bugs found on the
way and scope corrections. Say plainly what was *not* done and why.

## Why

Building C4IX costs ~60 s of compiler time for an OS of 3,700 lines: 12 serial
`c4sp`+`c4lc` invocations at ~43 s for the kernel, plus ~18.5 s for `libc4ix`
and the 14 userland programs. Under `c4m` the same work carries a ~65-70x
interpretation penalty, which is why every Makefile rule runs `./c4sp`
natively.

The cost is structural. c4sp's values are 4-word heap cells under a
conservative mark & sweep collector; environments are alists, so every variable
reference is a linear walk; every CEK continuation frame is an arena
allocation; builtin dispatch is a ~30-comparison if-chain
(`src/c4sp/include/stdlib.h:9-10`); and the native build is pinned at `gcc -O0`
because the collector finds roots by scanning the stack (`Makefile:404-408`).

## Measurements taken while planning (2026-08-23)

Recorded here because two of them contradict what the docs say.

- **The published baseline is stale.** `docs/c4lc-design.md:59-65` records
  0.67 s / 14,981 tokens for lexing `c4cc.c`. Actual, three identical runs:
  `./c4sp -c 2000000 .../c4lc-tokens.lisp -count src/c4cc/c4cc.c` →
  `tokens 15023`, **0.90 s**.
- **c4lc uses no `call/cc` and no first-class environments.** Zero occurrences
  across `c4lc*.lisp`, `c4r.lisp`, `c4opt.lisp`. So none of c4sp's expensive
  machinery — the CEK conversion, reified envs — is doing anything for c4lc.
- **Therefore `-R` (the recursive evaluator, already shipped as c4sp's oracle)
  is both safe and faster for c4lc.** Full lex+parse of `c4cc.c`:
  CEK **3.75 s**, `-R` **2.60 s** — **1.44x from a flag that already exists.**
- **The C4 L7 dialect already carries programs bigger than c4lc.**
  `src/c4or1k/` is 4,917 lines in that dialect, compiled by c4lc, and boots a
  real Linux kernel. c4lc itself is 4,823 lines of Lisp.

All later timings use `hyperfine` (warmup + medians), not hand-rolled loops.

---

## Track 0 — c4m's memory opcodes

Three findings, verified by reading `c4m.c`:

- **`RALC` is dead.** The case is commented out (`c4m.c:1690`) but `realloc` is
  in the builtin table (`c4m.c:308`) and `RALC` is in the opcode enum, so guest
  code compiles and then falls through to the unknown-instruction path, leaving
  the size argument in the accumulator.
- **`c4_malloc`/`c4_free`/`c4_realloc` do not exist on either host.** The
  native block has all three commented out with `// TODO: native c4_malloc, etc
  disabled due to bad implementation` (`c4m.c:1026-1029`); there is no
  C4-hosted implementation at all. The VM calls raw `malloc`/`free`.
- **`MSET`, `MCMP`, `MCPY` have no protected-mode guard** (`c4m.c:1700-1702`)
  while `MALC`, `FREE`, `PRTF` do. A protected task can `memset`/`memcpy` over
  arbitrary host memory, including kernel structures and the VM's own state.
  `STRC`'s guard is commented out too (`c4m.c:1704-1709`).

**Design as built — a side table, not a size header.** The obvious fix is to
over-allocate, stash the length in front of the block and return the word after
it. **That is wrong here, and it was tried first.** Guest `free()` is reached by
pointers that guest `malloc()` never produced: instrumenting the MALC and FREE
opcode handlers on `./c4 c4m.c load-c4r.c -- prog.c4r` shows the loader issuing
**8 MALCs and 16 FREEs**, where the identical workload on native c4m is a
balanced **17/17**. With a header, those eight extra frees become `free(q - 1)`
on pointers that were never offset — heap corruption, and in practice an
immediate `free(): invalid pointer` abort.

So sizes live in a **pointer-keyed side table** and guest pointers are handed
out and taken back byte-for-byte as before:

    c4_malloc(n)      p = malloc(n); record (p, n); return p
    c4_free(p)        forget p (a no-op if it was never ours); free(p)
    c4_realloc(p,n)   !p -> malloc; n <= 0 -> free, null;
                      look up old size; malloc, copy min(old,n), free old;
                      refuse (return 0) if the pointer is not one of ours

Open addressing, linear probing, power-of-two capacity, tombstones on delete,
rehash on growth. It needs only `malloc`, `free`, `memset` and the existing
`c4_memcpy`, so **one implementation serves both hosts** and they cannot drift.
The compatibility risk is zero *by construction* rather than by audit: `free()`
of something we never allocated does exactly what it did before.

- [x] **0.1** Audit of `FREE` call sites — **and the lesson**: the source-level
      audit (`load-c4r.c`, `c4ke.c`, `src/c4ix/*`, `c4sp`, `c4l.c`,
      `src/c4mp/*`) found no `free()` of a non-`malloc` pointer and was
      therefore **wrong**. Only instrumenting the opcode handlers at runtime
      revealed the 8-vs-16 asymmetry. Audit the behaviour, not the source.
- [x] **0.2** `src/tests/test_realloc.c` — grow, shrink, NULL ptr, from-null,
      growth loop, alloc/realloc/free churn — green under gcc, which generated
      `src/tests/expected/test_realloc.txt`. `realloc(p, 0)` is deliberately
      **not** exercised for its return value beyond "frees and yields null",
      since the C standards diverge there; c4m matches glibc.
- [x] **0.3** `c4_malloc`/`c4_free`/`c4_realloc` + the side table implemented in
      `c4m.c`; `RALC` re-enabled with the same protected-mode guard `MALC` and
      `FREE` carry
- [x] **0.4** `make test-c4m-mem` green: native c4m and `./c4 c4m.c` (c4m
      interpreted by unmodified c4) both reproduce the gcc golden exactly. The
      second leg is also the proof that the new code is in the plain c4 subset.
- [x] **0.5** Full suite green — `test-c4l`, `test-link`, `test-c4sp`, `test`,
      `test-c4ke-ramfs`, `test-oisc4`, `test-c4lc`, `test-c4ix` all PASS.
      **`test-c4mp` FAILS, and did so before this change too** (verified by
      stashing `c4m.c` and rebuilding): `test-c4mp: FAIL raycast: output
      differs`. Pre-existing, unrelated, and **still open** — see below.
- [ ] **0.6** PM guards for `MSET`/`MCMP`/`MCPY` behind a `CONF_` flag, opt-in
      like `CONF_TRAP_RESTORES_INTERVAL`, with kernel-side emulation in C4IX
      (**separate change** — guarding them unconditionally would trap every
      protected task's first `memset`, since libc4ix uses them in userland).
      Not started.

### Open questions raised by this work

- **Why the plain-c4 chain frees more than it allocates — ANSWERED.**
  `load-c4r.c:648-652`: when `__c4_info() & C4I_C4` says the host is plain c4,
  `c4r_load_opt` routes the whole load through `c4r_load_opt_pure`, which is
  `__c4_invoke((int *)&c4r_load_opt_pure_passthrough)` — the `C4IV` opcode,
  "call a section of code as if it were a C4 function". That runs the load
  outside the normal guest instruction loop, so its nine `malloc` calls
  (255, 72, 88, 98, 760, 32, 120, 56, 6 bytes) land on the **outer** VM's MALC
  and never reach c4m's handler. Cleanup through `c4r_free` runs as ordinary
  guest code, so its `free` calls **do** reach c4m's FREE. Hence 8 MALCs and
  16 FREEs where the native run is 17/17.

  So memory is allocated by one VM and freed by another. Raw `malloc`/`free`
  both bottom out in libc, so it works; any header scheme breaks on exactly
  those pointers, which is what happened. The side table is immune, and this
  is the specific reason it was the right call rather than a lucky one.

  Latent, not urgent: an allocation crossing a VM boundary is fragile, and
  anything that ever makes the two VMs' allocators differ will break this
  path. Worth tidying when `C4IV`'s semantics are next revisited.
- **`test-c4mp` / raycast is red on `main` as it stands.** Not investigated.

Payoff beyond the bug: removes the "never call realloc" constraint, so c4th can
grow its arenas instead of pre-sizing them.

---

## Track A1 — the cheap wins

The honest denominator for anything measured later. Re-measure
`make c4ix.c4r $(C4IX_PROGS)` from clean after each and paste the number.

- [x] **A1.0** Baseline, hyperfine, 3 runs: the 12-module C4IX kernel build is
      **43.013 s ± 0.199**.
- [x] **A1.1** Profile first — and it overturned the plan. `perf` is unusable
      here (`perf_event_paranoid = 4`, no sudo), so callgrind on one module
      (`c4lc -O -c src/c4ix/boot.c`, 7.5e9 instructions):

      | share | function |
      |---|---|
      | **77.08%** | `cells.h:env_local_pair` |
      | 4.31% | `cek.h:eval_cek` |
      | 3.66% | `gc.h:gc_alloc_cell` |
      | 3.03% | `cells.h:cell_type` |
      | 2.06% | `gc.h:gc_collect` |
      | 1.40% | `cells.h:cons` |

      **The environment walk *was* the interpreter.** Not the collector (5.7%
      for alloc and collect together), not the CEK machine, and the builtin
      if-chain I had expected to dominate does not even appear. Every item
      below was re-ordered around this.
- [x] **A1.1a** **Index the global frame** (`src/c4sp/include/cells.h`). The
      header comment there says a linear scan "wins over any hash" for a frame
      holding "a handful of bindings" — true for locals, and catastrophic for
      the global frame, which under c4lc holds every function of all eight Lisp
      modules plus the builtins. Atom ids are interned dense integers, so no
      hash is needed: an array from id straight to the `(atom . value)` pair.
      The bindings list is still maintained, so printing, `env:capture` and the
      collector see exactly what they saw before. Safe because `env_define` is
      the only code that adds a binding and nothing anywhere removes one, so an
      indexed pair cannot be collected out from under the index; and the index
      needs no GC root of its own, since every pair in it is also in the global
      list. ~40 lines.

      **Measured, 5 runs each, output verified byte-identical:**

      | workload | before | after | |
      |---|---|---|---|
      | `c4lc -O -c src/c4ix/sched.c` | 5.341 s | **1.048 s** | 5.10x |
      | lex `c4cc.c` (the doc's benchmark) | 0.90 s | **0.571 s** | 1.58x |
      | **C4IX kernel, all 12 modules** | **43.013 s** | **7.811 s** | **5.51x** |

      Green: `test-c4sp`, `test-c4sp-opt`, `test-c4sp-deep`, `test-c4lc`,
      `test-c4ix`. All 12 C4IX objects byte-identical to the pre-change
      compiler's.
- [x] **A1.2** `-R` is now the default for the c4lc **build** rules
      (`C4SPLC := ./c4sp -R`, `Makefile:55`). c4lc uses neither `call/cc` nor
      first-class environments — the two things the CEK machine exists for — so
      it only pays CEK's cost, an arena allocation per continuation frame.
      Verified byte-identical on all 12 C4IX modules **and** on the three deep
      bootstrap images (`c4ke.c` 200,713 bytes / 5,618 lines, `c4sp.c`, `c4m.c`),
      which also settles the C-stack-depth worry — the deepest input in the tree
      does not come close. The **test** rules deliberately stay on the CEK
      machine so both paths keep running, and `test-c4lc` now pins that the two
      evaluators emit the same image with and without `-O`.

      **Full clean `make c4ix.c4r`, 12 modules plus the link: 4.509 s**
      (baseline 43.013 s for the compiles alone).

- [ ] **A1.3** c4sp JSRI builtin dispatch — **deprioritised by A1.1**: the
      if-chain did not appear in the native profile at all. It may still matter
      under c4m, where each comparison is a whole VM instruction, so measure
      there with `__c4_cycles()` before spending anything on it.
- [x] **A1.4** `gcc -O0` unpinned — native c4sp now builds at
      `-O2 -fno-omit-frame-pointer`, worth **2.26x** on a module compile
      (522.2 ms → 231.5 ms), byte-identical output. `gc_collect()` spills the
      callee-saved registers with `setjmp` into a buffer in its own frame and
      starts the conservative scan at that buffer, so a cell whose only
      reference is in a register is still found. Native-only (`#if NATIVE`):
      under the C4 VM there are no such registers and the scan is already
      exact.

      **The pin was real, and the fix was verified to be what lifts it**: an
      `-O2` build *without* the spill dies immediately on `gcloop.lisp` with
      `undefined variable: n` and crashes every real compile. With it,
      `gcloop.lisp` passes and `sched.c` compiles byte-identically at arena
      sizes of 200k, 400k and 1M cells — small enough to force collections
      throughout. `test-c4sp`, `test-c4sp-opt`, `test-c4sp-deep`, `test-c4lc`,
      `test-c4ix` all green.
- [x] **A1.5** `c4sp.c4r` and `c4sp32.c4r` are now built by `c4lc -O`, not
      c4cc. 289,852 → 243,957 bytes (−15.8%) at 64 bits, 147,392 → 124,361
      (−15.6%) at 32 bits. This is the image every hosted run uses — under c4m,
      inside C4KE, and on c4bb — so it is the one whose size and speed are felt.
      `test-c4sp` green.
- [x] **A1.6** C4IX module build parallelised through the existing
      `c4lc_compile_par`. Full clean build of kernel + `libc4ix` + all 14
      userland programs: **5.110 s**, from ~60 s at the start. `test-c4ix`
      green.

      **Found while doing it, and pre-existing: `c4rlink` is not
      reproducible.** Linking the *same* twelve `.c4o` objects twice gives
      images of identical size differing in 7,395 bytes. It is not the
      parallelism and not c4lc — individual module compiles are deterministic
      and byte-identical to the pre-change compiler. It is **ASLR**: under
      `setarch -R` two links are byte-identical. c4rlink writes its own heap
      addresses into the operand words of patched slots (the image has 1,878
      patches), and those words are dead — the loader overwrites them via the
      patch table, which is why every test still passes. Worth fixing, because
      byte-comparison is this tree's main verification tool and any
      link-involving "identical image" check is currently impossible. Not
      fixed here: it is a change to a tool C4KE, C4IX, c4or1k and c4bb all
      depend on, and it is outside A1.

- [x] **A1.7** Final measurements, all wins combined. hyperfine, 5 runs
      (3 for the builds), output byte-identical to the pre-change compiler
      throughout.

### Native (64-bit)

| workload | before | after | |
|---|---|---|---|
| **C4IX kernel** (12 modules + link) | 43.013 s | **1.490 s** | **28.9x** |
| **C4IX full** (kernel + libc4ix + 14 programs) | ~60 s | **3.411 s** | **~18x** |
| one module, `sched.c -O -c` | 5.341 s | 0.2315 s | 23.1x |
| lex `c4cc.c` (75,651 bytes) | 902.6 ms | 128.5 ms | 7.02x |
| full `-O` compile of `c4lc_l2.c` | 627.8 ms | 107.7 ms | 5.83x |

### Hosted under c4m

| workload | before | after | |
|---|---|---|---|
| lex `c4cc.c` | 41.9 s | 15.2 s | 2.76x |
| full `-O` compile of `c4lc_l2.c` | 7.21 s | 3.81 s | 1.89x |

### Hosted on c4bb (the 32-bit simulated hardware)

Cycle counts as well as wall clock, since cycles are host-independent.

| workload | before | after | |
|---|---|---|---|
| lex `c4cc.c` | 477.5 s / 10.06 G cyc | 177.6 s / 3.68 G cyc | 2.69x |
| full `-O` compile of `c4lc_l2.c` | 85.0 s / 1.758 G cyc | 46.3 s / 0.938 G cyc | 1.87x |
| **real C4IX module** `sched.c -O -c -P` | 829.9 s / 16.49 G cyc | **283.1 s / 5.95 G cyc** | **2.93x** |

**Why the hosted gains are smaller than the native ones.** `-O2` is worth
2.26x and applies only to the native binary — a `.c4r` image cannot benefit
from it. Divide the native ratios by 2.26 and they land on the hosted ones
(7.02 / 2.26 = 3.1 against 2.76 measured for the lexer). The hosted images
did take the equivalent win available to them, which is being built by
`c4lc -O` (A1.5). So the index, `-R` and the `c4lc -O` image are the whole
of the hosted gain, and they are at their ceiling for this set of changes.

**On c4bb specifically**: a real C4IX kernel module now compiles in **4m43s**
of simulated hardware, down from **13m50s** — so the twelve-module kernel goes
from roughly 2h46m to roughly **57 minutes** inside c4bb. Nothing here changes that order of magnitude — c4bb runs at
about 20M instructions/s simulated, and the compiler needs ~6 G of them
per module.

---

## How much is the Lisp interpreter actually costing? (measured 2026-08-23)

Asked after A1 landed, because the c4bb figures still looked large. Answered
with callgrind, exact instruction counts, same sources, same machine — c4cc
(a C compiler written in C) against c4lc (a Lisp compiler on a Lisp
interpreter):

| workload | c4cc | c4lc | ratio |
|---|---|---|---|
| startup only (`int main(){return 0;}`) | 186,244 | 58,248,216 | 313x |
| `src/tests/c4lc_l2.c` (~2.5 KB) | 369,408 | 152,648,002 | 413x |
| `src/c4cc/c4cc.c` (75,651 B) | 579,257 | 2,255,151,237 | **3,893x** |

Subtracting startup gives the marginal cost per source byte on the large file:
**c4cc 5.2 instructions/byte, c4lc 29,040** — about **5,600x**. c4lc's *startup
alone* is 100x c4cc's entire self-compile.

c4cc is not a fair opponent — it is single-pass, has no AST, no optimizer, no
preprocessor and a smaller language — so a C- or Forth-hosted compiler with
c4lc's actual feature set would cost more than c4cc, plausibly 10-50x more.
Even so that lands at 50-250 instructions/byte against c4lc's 29,040: a
**100-500x** reduction.

**What that means for c4bb.** A real C4IX module is 5.95 G cycles today. At
100-500x less it is 12-60 M cycles, i.e. **roughly 0.6-3 seconds** on c4bb at
~20M instructions/s — instead of 4m43s. The twelve-module kernel would go from
~57 minutes to well under two minutes.

**This overturns the conclusion recorded after A1** ("c4bb is not fast enough
and no rewrite fixes it"). That was reasoned from the assumption that the
residual cost was inherent compilation work. It is not: it is ~99.97%
interpretation overhead. A2 is therefore justified again, and on much stronger
evidence than the build-time argument that originally motivated it — the case
now rests on hosted execution, where A1's wins cannot reach because `-O2` and
native code do not exist for a `.c4r` image.

**Host language for A2 is now an open choice.** C and Forth both compile to
native code and so both collect the same 100-500x; speed no longer
discriminates between them. What is left is ergonomics: C ports more
mechanically from the existing Lisp (recursive functions over tagged lists),
while Forth gives back the metaprogramming that Lisp was providing and is a
language the project wants for its own sake.

---

## Track A2 — c4lcc, c4lc in C

Port c4lc to the C4 L7 dialect, keeping its architecture, phases, IR and `.c4r`
output exactly. Builds two ways: `gcc -O2` natively, and `c4lc`/`c4lcc` for
`c4lcc.c4r` under c4m.

**Why this is the low-risk option:** at every step c4lc is a working oracle
emitting byte-identical expected output. The port never has to be guessed at.

**Where the speed comes from**, only one of which is "being compiled":
native code; **arena allocation, no GC** (a compiler is a batch process —
bump-allocate, free everything at exit); and **arrays instead of cons cells**
for the hot structures (`c4lc-design.md §12` proposes this itself — the token
list for `c4cc.c` is 15,023 tokens × 4-word cells today).

Each module is differentially verified against the Lisp before moving on.

- [ ] **A2.1** `c4r.c` ← `c4r.lisp` (561 L) — byte-identical round trip of
      existing `.c4r` images vs `c4r-roundtrip.lisp`
- [ ] **A2.2** `lex.c` ← `c4lc-lex.lisp` (375 L) — token dump ==
      `expected/c4lc-tokens.txt`; `-count` on `c4cc.c` == 15,023
- [ ] **A2.3** `pp.c` ← `c4lc-pp.lisp` (576 L) — output byte-identical to
      `gcc -E` for all 12 C4IX modules (the existing bar)
- [ ] **A2.4** `parse.c` ← `c4lc-parse.lisp` (888 L) — AST dump ==
      `expected/c4lc-ast.txt`; parse sweep over `src/tests/*.c`
- [ ] **A2.5** `gen.c` ← `c4lc-gen.lisp` (1534 L) — emitted `.c4r`
      **byte-identical to c4lc's** across the whole `C4LC_DIFF` corpus
- [ ] **A2.6** `tree.c` + `opt.c` ← `c4lc-tree.lisp` + `c4opt.lisp` (779 L) —
      `-O` output byte-identical
- [ ] **A2.7** Driver + `-c` object mode — `.c4o` objects link with c4cc/c4lc
      objects in either order
- [ ] **A2.8** Bootstrap fixed point: c4lc compiles `c4lcc.c` → `c4lcc.c4r`,
      then c4lcc compiles `c4lcc.c` itself → **byte-identical**, because a
      faithful port emits what c4lc emits
- [ ] **A2.9** C4IX rebuilt with c4lcc, images byte-identical to the c4lc
      build, before switching `Makefile:839` over
- [ ] **A2.10** Final numbers: native C4IX build time, and `c4lcc.c4r` under
      c4m vs `c4cc`

Expect ~6,000-7,000 lines of C for 4,823 of Lisp.

---

## Out of scope / later

- Anything about hosting a compiler on Forth. Answered during planning: Forth's
  ceiling is no higher than C's (both end at native code) and its port is the
  hardest of the options, because it is point-free — `c4lc-gen.lisp`'s 1,534
  lines of recursion with many named locals would need redesigning, not
  translating. c4th is built for its own sake instead; see
  `docs/c4th-design.md`.
- Host choice does not affect C99 coverage: the features live in the ~4,800
  lines of front-end logic, so every option carries all of them and the
  "keep C99" requirement is exactly the porting cost.

## Process

`timeout N` on every `c4`/`c4m`/`c4sp` invocation and
`pkill -f "c4m load-c4r"` afterwards; no background jobs left running; read any
regenerated golden file for its failure text before committing it.
