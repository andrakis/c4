# Compiling C4KE on the breadboard

Tracker for one question: **what is a realistic way to compile C4KE inside
c4bb quickly?** Companion to `docs/compiler-speed.md` (which is about the
host build) and `docs/c4fc-design.md` (which is about the compiler).

**Rule for this document:** tick a box only when its verification command
has actually been run and is green, and paste the one-line evidence beside
it. Say plainly what was *not* done and why.

## Where the 5m29s went

`docs/c4fc-design.md` records c4fc compiling one C4IX module on the board in
**5m29s** and says "nineteen seconds of that is loading the compiler". That
was true and it was the wrong thing to look at. The measurements below were
taken 2026-08-26 to find the real distribution.

### The unit, and the conversion factor

c4bb's `-s` counts VM instructions, the same unit `__c4_cycles()` returns
under c4m, so a cycle count taken on either host converts to board seconds
by one constant:

    node src/c4bb/sim/cli.js -s -m 64 spin32.c4r
    -> 78,001,780 cycles in 3.80s (20,516k inst/s)

**20.5M instructions/second.** Everything below is quoted in instructions
first and board-seconds second, because instructions are what a change to
the compiler moves and seconds are what the user waits.

The model checks out against the published figure: c4fc compiling
`src/c4ix/vfs.c` (`-O -c`) costs **7.22e9 instructions** under c4m, which
predicts 352 s on the board against 5m29s = 329 s measured. Close enough
that the constant can be trusted for everything else.

### c4fc compiling all of C4KE

    ./c4m load-c4r.c -- c4th.c4r <the 15 .f files> prof.f \
       -e ': GO 1 OPTIMIZE ! C4FC-INIT -P ... S" c4ke.c" C4FC-PROF ; GO'

| phase | instructions | share |
|---|---:|---:|
| lex + preprocess | 19,161,602,013 | 22.4% |
| parse pass 1 (discovery) | 8,771,431,501 | 10.3% |
| T2 close + externs | 5,347,305,429 | 6.3% |
| parse pass 2 | 8,592,802,408 | 10.1% |
| **optimizer (`-O`)** | **40,966,931,887** | **47.9%** |
| **total** | **~85.5e9** | |

**85.5 billion instructions = 69 minutes on the board.** My guess before
measuring was "the parser and the peephole fixpoint". Half right: the
optimizer is indeed the single biggest phase at 48%, but the *parser* is
only 20% and the *preprocessor and lexer* are 22% — and the preprocessor is
the interesting one, for the reason in the next section.

### The same work by a compiler that is native VM code

`cpp.c4r` and `c4cc.c4r` are ordinary `.c4r` images: C compiled to C4
instructions. Both were run on the breadboard against the same kernel
source, with the sources on a `-d` disk:

| tool | instructions | board seconds |
|---|---:|---:|
| `cpp32.c4r` preprocessing `c4ke.c` (5,513 lines out) | 56,604,737 | **2.89** |
| `c4cc32.c4r` compiling that `c4ke.i` | 124,140,840 | **5.93** |
| **together** | **180,745,577** | **8.8** |

**c4fc: 85.5e9. cpp + c4cc: 0.18e9. A factor of 472.**

Phase against phase, on identical work: c4fc's preprocessor+lexer costs
19.16e9 where `cpp` costs 0.057e9. **338x for the same job.**

### Where the 472x actually comes from

An instrumented build of c4th (a counter in `th_run`, scratch only) counted
every word the C4KE compile executes:

    words executed: 841,252,663  (colon 94,999,793, prim 746,252,870)  distinct 884

85.5e9 instructions / 841e6 words = **102 VM instructions per Forth word
executed**. The threaded `NEXT` is only about twenty of those; the rest is
that every primitive is a C function reached through a code field, and
inside it `th_push`/`th_pop` are C calls of their own.

The top of the profile says what those words are:

| share | cum | word |
|---:|---:|---|
| 13.06% | 13.06% | `@` |
| 12.49% | 25.55% | `EXIT` |
| 11.89% | 37.44% | `+` |
| 6.22% | 43.66% | `CELLS` |
| 5.65% | 49.32% | `LFP` |
| 5.41% | 54.72% | `LIT` |
| 4.82% | 59.54% | `LA` |
| 4.82% | 64.36% | `LSTK` |
| 4.72% | 69.08% | `1+` |
| 3.02% | 72.10% | `(LOCAL@)` |
| 2.91% | 75.02% | `!` |
| 2.54% | 77.55% | `0BRANCH` |

Read it as two facts:

1. **`@ + CELLS LIT 1+` = 41% of all word executions is field access.**
   The DSL stores every node, token and symbol as a cell array indexed by
   field number, so `y.nlen` is `LIT CELLS + @` — four words, ~400 VM
   instructions, where C's `y[NLEN]` is one `LI` at a constant offset,
   two or three instructions. That single idiom is over a hundred times
   more expensive than the C it stands for, and it is the most common
   thing a compiler does.
2. **`LFP LA LSTK (LOCAL@) (LOCAL!) LSP (LFRAME) (LDROP)` = 20%** is
   `locals.f` — named locals, entirely bookkeeping.

So the gap is not mainly the threaded interpreter. It is that c4fc's data
model spends ~400 instructions on what C spends 3 on, and does it 340
million times.

### What compiling the Forth to native code would actually buy

`native.f`'s strategy (b) turns `@` into `LI`, `+` into `ADD`, `LIT` into
`IMM`, and constant-folds `CELLS`; `locals.f`'s words become `bp`-relative
frame slots, which is what B5c's frame already is. Bottom-up estimate:

    746M primitives x ~2 instructions        = 1.5e9
     95M colon entries x ~14 (call + return) = 1.3e9
                                        total ~2.8e9  -> 137 s on the board

**About 30x, so 69 minutes becomes a bit over two minutes.** Real, and
still 15x slower than `cpp` + `c4cc` at 8.8 s. The B5 probe's 14x
(343 cycles/iteration threaded vs 24 native) is the *arithmetic* ceiling;
the estimate above is higher than it because the DSL's field-access idiom
folds away completely, which a counting loop has none of.

And it is not free: `native.f` inlines calls and declines recursion
(`docs/c4th-design.md`, "native.f and c4fc: measured, and the answer is
no"), c4fc is recursive descent, and `self.f` deliberately has no
compile-time execution while the DSL is built on `CREATE`/`DOES>`,
`POSTPONE` and `EVALUATE`. Either route is a project, not a flag.

## The decision

**Two compilers for two jobs, which is what the numbers say and what DOS
always did.**

- **`cpp` + `c4cc` is how C4KE gets built on the board.** It is native VM
  code, it is 472x cheaper, it is already what the host Makefile uses to
  build `c4ke.c4r`, and `c4dos-build/` already carries it as a bootable
  floppy. Nothing has to be written; it has to be built at 32 bits and
  demonstrated.
- **c4fc stays what it is:** the compiler that matches c4lc byte for byte,
  has a real preprocessor, `-O` and object mode, and is written in
  something a person can read and extend at the C4DOS prompt. It is the
  interactive compiler, not the batch one.

The native-Forth path is not abandoned, but it is now correctly sized: it
is worth ~30x, it lands at ~2 minutes, and it should be judged against
what else two months of work could buy. It is **not** the way to make
C4KE build quickly on c4bb.

## Milestones

- [x] **M0** This tracker, with the measurements above, before any code.
- [x] **M1** `c4cc32.c4r` plus `init32.c4r`, `c4sh32.c4r`,
      `c4ke.vfs32.c4r`; `dostar32.c4r` and `dosload32.c4r` already had
      rules and had never been built. `make c4dos-build32` builds all six
      with `c4cc32` and nothing else.
- [x] **M2** `c4dos-build32/`, and `make run-c4dos-build32` for the
      interactive session.
- [x] **M3** `BUILD` on the breadboard. Sources unpacked in the machine,
      preprocessed in the machine, compiled in the machine:

          c4ke.i    <ram> 199124 bytes
          c4ke.c4r  <ram> 187771 bytes
          init.c4r  <ram>  61911 bytes
          c4bb: 268504123 cycles in 13.26s (20243k inst/s), status 0

      **13.26 seconds**, against c4fc's 85.5e9 instructions = 69 minutes
      for the kernel alone. A factor of **312** on the whole task.
- [x] **M4** Boot what it built. `RUN dosload.c4r c4ke.c4r` on the board:

          c4ke: Kernel ready in 631ms after 278.791 M cycles
          C4SH - The C4 SHell v 0.1a
          c4ke: clean shutdown in 390ms, ... total C4KE runtime 4.478s

- [x] **M5** `make test-c4dos-build32` — build, boot and clean shutdown,
      non-interactive:

          c4bb: 282761026 cycles in 13.73s
          test-c4dos-build32: OK

      **Fourteen seconds for a machine to compile and boot its own
      operating system**, which is the whole point of the exercise.

## What is deliberately not here

- **No change to c4fc.** Nothing measured here says c4fc is wrong; it says
  it is not a batch compiler for a 20 MHz machine.
- **No new VM opcodes.** `docs/c4th-design.md` §8 already rules on that.
- **No `-O` on the board.** `c4cc` does not optimize and the kernel does
  not need it to boot; `c4opt` remains a host-side pass.
- **`vfsload` is missing from the floppy.** The booted kernel says
  `lc4r: unable to open 'vfsload' or 'vfsload.c4r'` and carries on to the
  shell; the 64-bit `c4dos-build/` has the same gap. Not fixed here
  because it is not what this tracker is about.
- **The `.c4o` route is not on this disk.** c4rlink and object mode are
  c4fc's, and `c4cc` compiles the kernel whole in one pass, so there is
  nothing to link. If the board ever wants separate compilation it is
  `c4cc -c` that would have to grow it, not c4fc that would have to get
  faster.
