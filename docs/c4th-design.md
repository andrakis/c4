# c4th — a Forth for C4

`c4th` (pronounced "C forth") is a Forth-2012 CORE implementation for the C4
ecosystem, in the tradition of `c4sp` and `c4lc`: written in the C4 subset,
built two ways from one source, tested against an external oracle.

It is built **for its own sake** — an interactive systems language for C4IX, a
good target for this VM, and a third independent `.c4r` writer to differential
against. It is explicitly *not* a compiler host: the question "should c4lc be
rehosted on Forth?" was answered during planning and the answer was no. See
`docs/compiler-speed.md` for that work and the reasoning.

**Rule for this document:** tick a box only when its verification command has
actually been run and is green, and paste the one-line evidence beside it.
Anything discovered mid-implementation that changes a later milestone gets
written in here, not just said in conversation.

## 1. Why Forth, and why not for the compiler

C4's VM is already a Forth machine. It is a stack machine with the top of stack
cached in an accumulator, so on the base ISA:

| Forth | C4 |
|---|---|
| `+ - * / MOD AND OR XOR = <> < > <= >=` | `ADD SUB MUL DIV MOD AND OR XOR EQ NE LT GT LE GE` |
| `@` `C@` | `LI` `LC` |
| `DUP` | `PSH` |
| `NIP` | `ADJ 1` |
| `DROP` | `IMM 0; ADD` (`a = *sp++ + 0`), or `ADJ 1` when the accumulator is dead |
| `IF` | `BZ` |

Every ALU op is `a = *sp++ OP a` — Forth's exact semantics, one instruction.
Nothing in this tree has exploited that.

What Forth does *not* buy is a faster compiler. Its ceiling is no higher than
C's, because both end at native code, and its port would be the hardest of the
options: Forth is point-free, so `c4lc-gen.lisp`'s 1,534 lines of recursion
with many named locals would have to be redesigned rather than translated.

## 2. Layout

    src/c4th/c4th.c            driver, flags, REPL, image save/load
    src/c4th/include/*.h       the kernel
    src/c4th/forth/*.f         core.f, coreext.f, asm.f, native.f, c4r.f, meta.f
    src/c4th/tests/            vendored Forth-2012 suite + expected/*.txt

Built exactly as `src/c4sp/c4sp.c` is (`Makefile:407-410`), but with `gcc -O2`:
c4th has no conservative collector, so it is not pinned open the way c4sp is.

    c4th:     gcc -O2 -Iinclude -I. -o c4th src/c4th/c4th.c
    c4th.c4r: $(PREPROC) src/c4th/c4th.c | $(C4CC) -o c4th.c4r -

## 3. Threading model — indirect, dispatched by `JSRS`

    int *ip;
    void th_run () {
      int *w; int *wcode;              // a LOCAL -> JSRS, reentrant
      while (ip) { w = (int *)*ip; ip = ip + 1;
                   wcode = (int *)w[W_CODE]; wcode(w); }
    }

`c4cc.c:808-810` emits `IMM d[Val]` for a function name, so the call is one VM
instruction. gcc accepts the same source through the macro idiom
`load-c4r.c:997-1001` already uses:

    #ifndef __c4cc__
    #define wcode(w) ((void (*)(int *))wcode)(w)
    #endif

The local is named `wcode` rather than `f` so the macro cannot collide with
anything else in the translation unit.

Rejected alternatives: a C `switch` compiles to ~15 VM instructions under c4cc
(`c4cc.c:1088-1183`) plus `SWITCH_MAX_CASES`/`SWITCH_MAX_RANGE` caps; a `JSRI`
global table has no per-word code field, so `VARIABLE`, `CONSTANT`, `VALUE` and
especially `CREATE`/`DOES>` would each need bespoke token numbers; direct
threading loses `>BODY`'s natural home.

**`do_colon(w)` is `rpush((int)ip); ip = w + W__Sz;`** — `EXECUTE` sets `ip` and
*returns*, never recursing into `th_run`. C-stack depth stays 1 for arbitrarily
deep Forth recursion. Worth noting for contrast: c4sp needed a 270-line CEK
conversion (`src/c4sp/include/cek.h`) to buy that same property.

## 4. Dictionary

Enum-as-record-offsets, the house idiom (`c4.c:53`, `include/u0.h:39-55`). The
xt *is* the header address, so `>BODY` is `xt + W__Sz` and every accessor is one
`LEA`/`LI` pair.

    enum { W_LINK,    // previous header, 0 = end of chain
           W_HASH,    // folded (len, first 3 chars) -- one integer compare
                      //   rejects almost every FIND candidate
           W_NAME,    // char * into the name pool
           W_NLEN,
           W_FLAGS,   // FL_IMMEDIATE | FL_HIDDEN | FL_COMPONLY | FL_NATIVE
           W_CODE,    // do_colon / do_var / do_const / do_does / a primitive
           W_PRIM,    // primitive number, or -1 (native compiler, B5)
           W_NATIVE,  // natively-compiled body, or 0 (B5)
           W__Sz };

`W_PRIM` and `W_NATIVE` are unused until B5 and go in at B1 anyway: growing a
header later means rewriting every saved image, and layouts here get pinned by
golden files.

## 5. Memory

One allocation holding dictionary, name pool, compiled bodies, user variables,
`PAD` and `TIB`; separate data (1024 cells) and return (256 cells) stacks.
`CELL = sizeof(int)`, `CHAR = 1` byte, so `C@`/`C!` are `LC`/`SC`.

Addresses in the image are **real machine addresses**, not offsets — that is
what makes `@` be `LI`, which is the point of the project. The price is that a
saved image is unrelocatable, which is the job the metacompiler earns its place
doing (§8).

Arenas grow with `realloc` once Track 0 of `docs/compiler-speed.md` lands. Until
then they must be sized once at startup from flags: `RALC` compiles under c4m
but its VM case is commented out (`c4m.c:1690`) and silently leaves the size
argument in the accumulator.

## 6. Line budget

    c4th.c    ~250   driver, argv, image save/load, REPL
    mem.h     ~180   image, stacks, HERE/ALLOT/,/C,
    dict.h    ~320   headers, hash, FIND, CREATE, wordlist walk
    inner.h   ~140   th_run, do_colon/var/const/does, EXECUTE
    num.h     ~220   double-cell math, >NUMBER, pictured numeric output
    io.h      ~150   source stack, KEY/EMIT/TYPE/ACCEPT/REFILL
    prim.h    ~900   ~120 primitives + registration
    outer.h   ~400   INTERPRET, COMPILE,, : ; POSTPONE, STATE
    except.h  ~110   CATCH/THROW, ABORT"
              -----
              ~2670   (c4sp is 2,221 across 8 headers)
    core.f    ~600
    coreext.f ~150

Watch c4cc's 512 KB text/data/sym pools (`c4cc.c:1893`) as `prim.h` grows.
Budget `num.h` generously — `UM/MOD`, `FM/MOD`, `SM/REM` and the
pictured-numeric words are where small Forths die, and none of it is optional
in CORE.

## 7. Testing

Vendor `tester.fs` and the Forth-2012 `core.fr` from forth-standard.org into
`src/c4th/tests/` verbatim, with a README recording URL and date. They are
written in Forth and self-verifying (`T{ 1 2 + -> 3 }T`), printing only on
failure, so they need no external Forth.

`make test-c4th` pins two things at once: the whole transcript against a golden
file — so a change in *which* things fail is a diff rather than silent drift —
and, independently, that
`grep -c "INCORRECT RESULT\|WRONG NUMBER OF RESULTS"` is exactly zero, so the
golden can never quietly bless a failure. Separate goldens for 32- and 64-bit
(`MAX-N`, `MAX-U` differ). The `.c4r` build under c4m must reproduce the 64-bit
transcript byte for byte, which is what pins the compiled interpreter against
the native one.

Deliberate divergences from the standard get their own section here, the way
`c4lc-design.md §5.1` documents c4cc's lexer quirks.

`/usr/bin/gforth` 0.7.3 is available as a second opinion when a `core.fr`
failure is ambiguous about whether c4th or our reading of the standard is
wrong. It is a second opinion, never the arbiter — 0.7.3 is a 2014 release with
incomplete Forth-2012 coverage.

## 8. The native backend and metacompiler (B5)

Two strategies; decide between them with a one-day cycle-count probe using
`__c4_cycles()` **before** writing the full version.

**(a) Subroutine threading over a software data stack.** A colon word compiles
to real `JSR &prim_add; JSR &prim_dup; …`. The C4 stack becomes the return
stack; the Forth data stack stays the software array. 3 instructions of call
overhead versus ~20 for `NEXT`, but `+` costs ~15-18 inside the primitive.
Every opcode used is ≤ `EXIT`, so generated images run under **plain c4** via
`c4l.c`. Zero VM changes.

**(b) `sp` is the data stack, TOS in the accumulator.** The table in §1 applies
directly: `+` is one instruction against ~18, and `SWAP`/`OVER`/`ROT`/`?DUP`
become compile-time renames costing nothing.

The one hard problem is calls: `JSR` pushes its return address onto `sp`, which
is now the data stack. The design is an explicit link with a **software return
stack** — caller pushes the return label, `JMP callee`; callee ends `R>; JMPA`.
~14 instructions per call, acceptable because a colon word does real work. The
return stack does **not** need VM opcodes: `>R`/`R>`/`R@`/`I` are cold, once per
loop rather than once per operation. The data stack is the hot one and it gets
`sp`.

**Mandatory safety rule:** the stack model is canonicalised at every
control-flow join and every word boundary to "TOS in the accumulator, everything
else on `sp`". Optimisation happens strictly within a basic block. A model that
survives an `IF`/`THEN` join where the two arms left values in different places
is a silent wrong-answer bug — the worst failure mode this project can have.

**New c4m opcodes are explicitly not planned.** They would have to start at
**79**, because `src/c4mp/c4mp.h` already occupies 66-78, leaving a permanent
hole in c4m's own table; and each needs mirroring in seven places (`c4m.c` enum
and name string, `load-c4r.c`, `c4l.c` and its `scan_extended` refusal,
`src/c4mp/{c4mp.h,vm.c}`, `src/oisc4/oisc4.c`, `c4cc.c`'s table, `c4r:ops` in
`c4r.lisp`, and the c4bb loader copy) — in exchange for maybe 1.3-1.6x over a
stack model that already makes `SWAP`/`OVER`/`ROT` free.

**The c4opt differential.** `src/c4sp/lisp/c4opt.lisp` implements six passes
(`fold shl adj0 jmpnext thread dead`) over exactly the representation
`c4r.lisp` decodes, already pinned by `make test-c4sp-opt`. Implement those six
with those semantics in `native.f` — noting `opt:fold1`'s "the stack operand is
on the LEFT" and `opt:mod`'s truncated modulo — then:

    c4th -native -O0 -o .a.c4r bench.f
    ./c4sp src/c4sp/lisp/c4opt-run.lisp .a.c4r .lisp.c4r
    c4th -native -O1 -o .c4th.c4r bench.f
    cmp .lisp.c4r .c4th.c4r          # BYTE IDENTICAL

Only after that gate is green do Forth-specific rules land (`PSH; IMM 0; ADD`
→ nothing, `PSH; IMM k; ADD; LI` fusion, `ADJ n; ADJ m` → `ADJ n+m`), at which
point the bar relaxes to "behaviour identical under c4m, size ≤ the Lisp
optimizer's".

**Metacompiler.** One vocabulary and a relocating `,`, not a two-vocabulary
cross compiler — host and target are the same word size and machine. Recording
every intra-image pointer while building into a buffer at a notional base *is*
the `.c4r` patch table (`asm-c4r.c:56-68`), so `c4th -save foo.c4r` writes a
real `.c4r` that `load-c4r.c` relocates and `c4rdump` inspects. No new format,
no new loader.

**Constraint:** the C4 VM has no `write` syscall. Natively c4th writes files
fine; under bare c4m it can only compile in memory and compare (as
`c4lc-eq.lisp` does) or write into the C4KE/C4IX RAM-FS. Same limit c4lc lives
with.

## 9. Milestones

- [ ] **B0** This document + ladder, before any code
- [x] **B1** Cells, dictionary, inner interpreter, fourteen primitives.
      `mem.h` 89, `dict.h` 98, `inner.h` 70, `prim.h` 90, `c4th.c` 175 — 522
      lines. *Verified:* `make test-c4th` — `./c4th -selftest` prints
      `3628800` and `selftest ok`, and the same golden is reproduced by
      `c4th.c4r` under c4m and by the same image running inside C4KE. The
      selftest also requires **both stacks to come back empty**, since a body
      that left junk behind would still print the right number.

      Notes from building it: `strcmp` and `atoi` are not C4 builtins (the
      list is open/read/close/printf/malloc/free/memset/memcmp/exit plus
      c4m's putchar/puts/realloc/memcpy/stacktrace), so the driver carries its
      own. And c4cc mis-emits a bare function name used as a value, so every
      code field is written `(int)&fn` — the `&` is load-bearing.
- [x] **B2** Outer interpreter and the primitive set, both builds.
      `io.h` 107 (line-at-a-time sources on a stack, so `INCLUDED` and
      `EVALUATE` can nest at B3), `outer.h` 216, `prim.h` 79 words. *Verified:*
      `make test-c4th` — `: SQ DUP * ; 7 SQ .` gives `49` natively and under
      c4m, and `src/c4th/tests/b2.f` walks every primitive group against one
      golden shared by both hosts. Also runs unchanged inside C4KE.

      Three things worth carrying forward:

      * **Name lengths were passed alongside the name, and one was wrong** —
        `DUP` was registered with length 4, so it silently vanished from the
        dictionary and every lookup of it failed. Fixed by removing the
        redundancy rather than the instance: `th_defword()` and `th_findz()`
        take the name alone and measure it.
      * **`.` must print in `BASE`**, not decimal. Forth-2012 defines it over
        the pictured-output words, which arrive with `num.h` at B3; until then
        the conversion is written directly so `BASE` means what it says from
        the start.
      * `HEX`/`DECIMAL` exist for a reason: `10 BASE !` typed while hex sets
        the base to sixteen, so a test that flips base has to use them.
- [x] **B3** `core.f` + the Forth-2012 CORE suite. **PASSES, zero failures**,
      natively, as `c4th.c4r` under c4m (byte-identical transcript), and
      inside C4KE. `num.h` 199, `core.f` 96, plus parsing/compiling words in
      `outer.h`. *Verified:* `make test-c4th`, which compares the whole
      transcript against a golden **and** independently asserts zero failure
      lines, so the golden cannot quietly bless a regression.

      Bugs the suite found, none of which self-testing would have:

      * **`RSHIFT` must be logical, `2/` arithmetic.** C's `>>` on a signed
        value is arithmetic, so `-1 RSHIFT 1` came back as `-1` instead of
        MAX-INT — and the suite *builds* MAX-INT and MIN-INT out of exactly
        that expression, so every comparison test at the extremes failed at
        once.
      * **`th_uless` was inverted.** When one operand has the top bit set it
        is the *larger* unsigned, not the smaller.
      * **`0 - MIN-INT` is still MIN-INT**, so `.` printed punctuation for it.
        The magnitude has to be read unsigned.
      * **gcc at `-O2` exploits signed-overflow UB** — see the note below; the
        fix was tree-wide.
      * **The `SWAP` in `BEGIN`/`WHILE`/`REPEAT` belongs in `WHILE`.** With it
        in `REPEAT` the single-`WHILE` case still works, while
        `BEGIN .. WHILE .. WHILE .. REPEAT .. ELSE .. THEN` compiles a branch
        to the wrong address — a segfault, not a failed assertion. The suite's
        `GI5` is exactly that shape.
      * **`ALLOT` reserves address units, not cells.** HERE is byte granular;
        cell-granular allotment fails `1STA 1+ -> 2NDA` and fails it quietly,
        since everything still runs and only the addresses are wrong.
      * **The text interpreter must consume the delimiter that ends a word**,
        or `CHAR " GS3 GOODBYE"` is eight characters instead of seven.

      **A finding that reaches past c4th: `-fwrapv` is a correctness flag for
      this whole tree.** `SM/REM` returned the wrong quotient for a divisor of
      MIN-INT, and only at `-O2`: gcc is entitled to assume signed overflow
      never happens, so it folded `if (d < 0) d = 0 - d;` on the assumption
      the result must be positive. `-O0` and `-fwrapv` both give the right
      answer. Everything built from `NATIVE_CC_OPTS` emulates a machine whose
      arithmetic wraps — c4m and c4mp *are* that machine, c4cc compiles for
      it, c4sp and c4th implement languages whose integers are its cells — so
      `-fwrapv` now applies tree-wide. It matters more since A1.4 put the
      native c4sp on `-O2` for the first time.
- [x] **B4** P1 threaded-code peephole + `asm.f`. `asm.f` 118, `peep.f` 168.
      *Verified:* `make test-c4th`, three ways.

      **`asm.f` against c4cc.** `src/c4th/tests/fact.c` is compiled by c4cc
      and the same program hand-assembled in `src/c4th/tests/fact.f`; the two
      instruction sequences must match. Addresses are masked — they depend on
      where the segments landed, and the claim is about the encoding.
      Emitting words carry a trailing comma (`IMM,`, `ADD,`), which is the
      Forth convention for "compile this" and incidentally keeps `AND`, `OR`,
      `XOR` and `LT` from colliding with the Forth words of those names.

      **The peephole, rule by rule.** `LIT a LIT b +|-|*` folds; `DUP DROP`,
      `SWAP SWAP` and `>R R>` cancel. Compaction moves addresses, so this is a
      real rebuild: decode the body, copy survivors into a scratch buffer
      recording where each lands, rewrite every branch operand through that
      map, copy back. A rule is refused if it would delete an instruction that
      something branches to.

      **And the check that actually matters**: the whole Forth-2012 CORE suite
      run again with the peephole applied to *every* definition, including the
      test harness's own, producing a transcript byte-identical to the
      unoptimized golden — natively and under c4m. A peephole exercised only
      by its own tests is one nobody trusts.

      **Honest result: 2 instructions removed across the entire suite.** Hand
      written Forth has almost no redundancy for these rules to find. That was
      the expectation going in, and it is why B4 exists: the pattern matcher,
      the instruction decoder and the address fixup all had to be built and
      debugged, and B5 needs all three.

      Two bugs, both instructive. `FOLD?` left its address on the stack in one
      branch and not the other. And the final `MOVE` ran backwards, so the
      rebuilt body was never installed while `HERE` still moved down —
      silently truncating every definition, which looked like success on any
      word that did not need its tail. The rebuilt code's branch operands were
      then written with addresses inside the *scratch* buffer rather than the
      final one; since the scratch buffer still held a copy, that too appeared
      to work until the next definition was optimized over the top of it. All
      three were found by the standards suite, none by the rule tests.
- [~] **B5** Native backend — **the compiler core is in and measured; the
      deferred-operand model and the metacompiler are not.**

      **The probe first** (`src/c4th/bench/`), because the strategy choice is
      expensive to get wrong. Three timings of one loop under c4m:
      threaded 343 cycles/iteration, strategy (a) 126, strategy (b) ceiling
      24. So: build (b), `sp` as the data stack with TOS in the accumulator.

      **`native.f` (200 lines) compiles the reorder-free subset**: literals,
      `+ - * / MOD AND OR XOR`, all six comparisons, `1+ 1- @ C@ DUP DROP`,
      `BRANCH`/`0BRANCH` and `EXIT`. Measured on a counting loop under c4m:

      | | cycles | |
      |---|---|---|
      | threaded | 60,400,337 | |
      | **native** | **1,400,337** | **43x**, 29 instructions emitted |

      `: T 2 3 + ;` compiles to `ENT 0; IMM 2; PSH; IMM 3; ADD; LEV` — optimal.

      **What it declines, and why it declines rather than trying.** `SWAP`,
      `OVER`, `ROT` and `!` are left threaded. C4's store is
      `*(int *)*sp++ = a`, so the destination must be pushed *before* the
      value is computed — and here the value is already in the accumulator,
      which loading the address would destroy. There is one register.  Done
      properly through the frame, `SWAP` costs about seventeen instructions,
      which is worse than what the threaded interpreter charges: compiling it
      would make code *slower*.

      The answer is a **deferred-operand model** — hold the top few stack
      items as compile-time descriptions (this one is a literal, that one is a
      fetch) and emit only when something forces them into existence. Then
      `1 2 SWAP -` emits `IMM 2; PSH; IMM 1; SUB` with `SWAP` free, and
      `x addr !` can push the address first because the *compiler* decides
      the order. That is the rest of B5.

      *Verified:* `make test-c4th` runs every word both ways against the
      threaded engine — which is the oracle, being the one that passes the
      CORE suite — and requires the declined words to say so.

### Should c4m get new stack opcodes? (asked and measured 2026-08-23)

`src/c4th/tests/survey.f` walks every colon definition in `core.f`, tries
to compile each natively, and reports what stopped it. Of the declines,
**two are stack reordering** — `SWAP` in `2!`, `OVER` in `WITHIN`. Every
other one is a primitive the backend has not implemented yet (`0=`, `>R`,
`,`, `HERE`, `MAX`) or a call to another colon word (`*/MOD`, `.`,
`CREATE`). Both of those are pure compiler work with no ecosystem cost.

Caveat worth stating: `core.f` is a *compiler library*, full of
compile-time words that poke `HERE` and `,`. It is not representative of
application code, so treat the ratio as directional.

**Decision: not yet.**

1. Reordering is not what is blocking compilation; coverage and calls are.
2. The deferred-operand model of B5b removes most `SWAP`s at compile
   time. Adding a `SWAP` opcode first would be optimizing a case the
   compiler is about to make disappear.
3. The cost is larger than the tables. The opcode name string and enum
   live in `c4.c`, `c4m.c`, `c4l.c`, `load-c4r.c`, `src/c4mp/{c4mp.h,vm.c}`,
   `src/oisc4/oisc4.c`, `src/c4cc/c4cc.c`, `src/c4bb/sim/devices.js` and
   `c4r.lisp` — and **c4bb needs microcode**, since
   `src/c4bb/hw/microcode.uc` implements each opcode individually
   (`op PSH:`, `op JMPA:` …). That is hardware-description work, not a
   table edit. Homeward mirrors it too.
4. Numbering starts at **79**: `src/c4mp/c4mp.h` already occupies 66-78,
   so c4m's own table keeps a permanent hole.

**If one is ever added, make it `SWAP`.** It is the single highest-value
choice because it also fixes stores: `!` becomes `SWAP; SI`. Stores are
the one place the compiler cannot always reorder its way out — C4's `SI`
wants the address pushed before the value is computed, while Forth writes
the value first, and when the value is a computed expression rather than
something re-materializable the deferred model cannot hoist it.

**A correction on what is possible:** operand-carrying opcodes are not
actually restricted to numbers below `ADJ`. Every VM decides with
`i <= ADJ`, but c4m already extends that to `i <= ADJ || i == JSRI ||
i == JSRS`, so an appended opcode *can* take an operand — it just has to
be added to that test everywhere the table is mirrored. Moot for this
decision, since `SWAP`, `OVER` and `DROP` need no operand.

**Decision rule:** build B5b, re-run `survey.f`, and add `SWAP` only if
reordering is still a top blocker in code that matters.

- [x] **B5b** The deferred model — **`SWAP`, `OVER`, `!`, `C!` and
      `VARIABLE` references now compile, and `SWAP` costs nothing at all.**

      The insight is to stop treating emitted code as fixed. Every item on
      the compile-time stack records **where its code begins**, so the
      compiler can still move it. `SWAP` becomes a rotation of the output
      buffer:

          [ codeA ][ PSH ][ codeB ]  ->  [ codeB ][ PSH ][ codeA ]

      Sound because each region is balanced — it computes one value into the
      accumulator and leaves the stack as it found it — and because no region
      spans a branch: every branch flushes the model first. `1 2 SWAP -`
      compiles to `IMM 2; PSH; IMM 1; SUB`; the `SWAP` is *gone*.

      **`!` falls out of it.** Forth writes the value before the address while
      C4's `SI` wants the address pushed first, so with `SWAP` free, `!` is
      `SWAP` then `SI` — and the reason B5 declined stores disappears.

      `OVER` re-runs the region below the top, which is only safe when that
      region can simply be executed again; it is allowed for a bare `IMM` — a
      literal or a variable's address, the case that actually occurs — and
      declined otherwise rather than guessed at. A `CREATE`d word is just its
      body address, so every `VARIABLE` reference is one `IMM`.

      Also fixed on the way: **`MOVE` used `memcpy`**, which is undefined for
      overlapping regions, and Forth-2012 requires overlap to work. The code
      motion above slides blocks over themselves, so this was load-bearing.

      | | threaded | native | |
      |---|---|---|---|
      | counting loop | 60,400,337 | 1,400,337 | **43x** |
      | loop through a `VARIABLE` | 124,600,868 | 2,400,342 | **52x** |

      Coverage of `core.f`, by `src/c4th/tests/survey.f`: **3 → 17 of 101**
      colon words. The remainder is calls to other words, `(DO)`/`(LOOP)`,
      and the compile-time dictionary words (`HERE`, `,`) that the control
      structures are built from.

- [x] **B5c** Calls, counted loops, and a frame — **every structural
      reason the backend used to decline a word is gone.** What is left is
      that primitives written in C cannot be inlined; nothing about the
      stack, the calls or the control flow blocks compilation any more.

      **Calls are inlined**, not called. B5b's note that a software return
      stack would cost about as much per call as the `NEXT` it replaces
      still stands, and inlining is better than either: the callee's items
      merge into the caller's model, so a called word optimizes as if it
      had been written out. A `CONSTANT` — which is `CREATE , DOES> @` —
      inlines to `IMM addr; LI`. Recursion is declined, which is the
      honest answer for an inliner, and is what `RECURSE` gets.

      **`ENT` earns its operand.** The backend now reserves frame cells
      below `bp`, which gives it somewhere to put things that are not
      stack items: `>R`'s saved value, and a counted loop's index and
      limit. `I` becomes `LEA`/`LI` — two instructions where the threaded
      engine charges twenty — and `LOOP` is thirteen. The frame size is
      not known until the end, so `ENT`'s operand and every frame-relative
      `LEA` are patched once compilation finishes.

      **And every live item now has an address**, because the frame is
      `bp`-relative while the data stack grows below it: item *i* is at
      `bp-(frame+i+1)`. That is the general fallback the deferred model
      never had. `OVER` and `2DUP` become two instructions each rather
      than a decline; `ROT`, `-ROT` and `2SWAP` join `SWAP` as free
      buffer permutations, with a through-memory version for when the
      regions are pinned; and `MIN`, `MAX`, `ABS`, `U<`, `U>`, `/MOD`,
      `+!`, `LSHIFT` and `2/` all become emittable.

      **Definitions that take arguments compile too**, with C4's own
      calling convention — so `INVOKE1`/`INVOKE2`/`INVOKE3` call a
      compiled word exactly as C4 calls any function, and `src/c4th/tests/b5.f`
      *runs* the compiled code rather than only checking that the compiler
      did not complain. Nothing in Forth declares an arity, so `NCOMPILE?`
      asks the compiler: the fewest arguments that compile without
      underflowing is the answer. It gets `2!` right at three and
      `WITHIN` at three.

      | | threaded | native | |
      |---|---|---|---|
      | `DO`/`LOOP` with `I` | 34,300,951 | 1,700,354 | **20x** |
      | `BEGIN`/`WHILE` with `>R`/`R>` | 107,201,208 | 3,100,351 | **34x** |
      | loop calling another word | 20,080,951 | 440,354 | **45x** |
      | loop reordering the stack | 21,681,110 | 1,540,357 | **14x** |
      | loop through a `VARIABLE` | 71,501,323 | 2,200,356 | **32x** |

      *Verified:* `make test-c4th`. Sixty-three words are compiled, called
      and compared against the threaded engine, and exactly four decline —
      pinned two ways, as the CORE suite is, so the golden cannot quietly
      bless a regression.

      **The design mistake worth recording.** The first version left
      arguments where C4 puts them, at `bp+2` and up, and taught the model
      to address two kinds of item. It passed every straight-line test and
      then failed on `?DO` inside a word with arguments — because a loop
      body that spills an argument *changes what kind of item it is*, so
      the model at the back edge was not the model the loop was entered
      with, and the code emitted on the first pass addressed the wrong
      place on the second. The fix was to stop having two kinds: the
      prologue copies the arguments onto the stack, three instructions
      each, once. The body then has one uniform model, `+` is one `ADD`
      again, and the loop converges. **A uniform model was both simpler
      and faster than the clever one**, and the only reason the clever one
      was caught is that the depth check at every branch target refused to
      compile it.

      Two other things that check turned into declines rather than wrong
      answers: an `IF` whose arms leave different numbers of values, and
      the fall-through into `?DO`'s loop entry — which is unreachable
      code, so the model there has to be *adopted* from whoever branches
      in rather than checked against whatever the dead path left behind.

      **The emitted code still uses only base opcodes** — nothing above
      `MOD`, and no `JSR`, since calls are inlined — so a generated image
      runs on plain `c4` as well as on `c4m`. That is worth more than the
      instructions a `PUTC` would save, and it is why `EMIT` is left
      declined.

      **What the survey now says** (`src/c4th/tests/survey.f`, over
      `core.f`): 8 of 30 colon words compile, and **every one of the other
      22 is stopped by a primitive written in C** — `MOVE`, `FILL`, `.`,
      `SPACE`, `M*`, `,`, `HERE`, `CREATE`. Not one is stopped by a stack
      operation, a call, a loop or a branch. The ratio understates the
      backend badly, because `core.f` is a *compiler library*: two thirds
      of it is compile-time words that poke `HERE` and `,`. The useful
      reading is not the fraction but the list of blockers, and that list
      is now entirely "primitives the backend cannot inline", which is
      B5d's problem, not the code generator's.

### The fuzzer, and the bug it found (2026-08-23)

`b5.f` is sixty-three cases somebody thought of, and the Forth-2012 CORE
suite exercises the *threaded* engine, not the backend. So
`src/c4th/tests/fuzz.f` generates cases nobody thought of — 75,000 of
them across five seeds with no mismatch, after the one below was fixed: random
definitions built from a table of operations, each one run on the
threaded engine — the oracle, since it is what passes the standards
suite — and then compiled and **called**, with the answers required to
agree. Two thousand per run, both with the fused opcodes and without,
pinned in `make test-c4th`.

Three things make it worth having rather than decorative:

* **Every generated definition is balanced by construction.** The
  generator tracks the compile-time depth and only picks an operation
  the depth can afford, then pads or drops back at the end of every
  block. A definition that underflows, or whose `IF` arms leave
  different depths, tests the backend's *refusal* rather than its code.
* **It generates control flow**, not just expressions: `IF`, `IF/ELSE`
  and counted loops with `I`, because the branch-target depth check, the
  pin and the loop's frame cells only come out under control flow. And
  `@`, `!` and `+!` on one scratch cell — always a valid address, and
  the reason to bother is that `!` is where the permutation machinery is
  used in anger, since C4's store wants its address pushed before the
  value is computed while Forth writes the value first.
* **It was verified to fail.** A one-character change to `-ROT`'s
  permutation turns 0 mismatches into 8, with the offending definition
  printed. A fuzzer that cannot fail is decoration.

**And it found a real one, in code that shipped at B5b.** A permutation
rebuilds the top regions and writes the `PSH` separators back between
them — and it assumed the cell just before each region *was* such a
separator. It usually is: `NEWITEM` spills the accumulator, and that
spill is the `PSH`. But when the item below is **already on the stack**,
no `PSH` is emitted, and the preceding cell is ordinary code. The rebuild
then wrote `PSH` over it. In the case the fuzzer found that cell was an
`ADJ`'s operand, which became `ADJ 13` — sixteen bytes of stack dropped
instead of one, `LEV` returning through garbage, and the VM executing
random memory.

The shape worth remembering is not the off-by-one. It is that **the
model knew something the buffer did not**: whether a spill had happened
was a fact about compilation that was never written down, so the rebuild
re-derived it from the buffer and got it wrong. `ISEP` writes it down.
The fix is three lines; finding it was the whole point of the exercise.

And it turns out to be the *right* check for a second reason. A region
is only movable because it is balanced — it computes one value into the
accumulator and leaves the stack as it found it. An item that reached
the stack without its own spill got there because something else pushed
it, which means the code attributed to it has a net effect on the stack
and is not balanced. So "no separator" and "not balanced" are the same
condition, and `ISEP` refuses both at once. `1 2 V ! 3 SWAP -` is the
smallest case: the region attributed to the `1` runs through the whole
store and leaves a value on the stack, so rotating it would be wrong
twice over. The word still compiles and still gives 2 — the permutation
just goes through the frame instead, which is what that fallback is for.

`nDUP`, `SWAP` after a store, and `2DROP` followed by a literal are all
ways to reach it, and none of `b5.f`, the CORE suite or `survey.f`
did — because it needs a permutation whose *lower* operand was left on
the stack by something other than its own spill, which is a shape hand
written test cases do not naturally take.

### Does c4m want new opcodes? (asked again, and measured, 2026-08-23)

B5b asked this about *stack* opcodes and answered no: the compiler makes
`SWAP` disappear, so an opcode for it would optimize a case that no
longer exists. B5c's decision rule was to re-ask once calls and loops
were in — this time with the experiment actually built, in **c4m only**,
and deliberately **not in c4bb**, whose microcode makes every opcode
hardware-description work.

**What was built.** Five opcodes appended to `c4m.c` at 79 — the first
numbers free after c4mp's 66-78 — implementing the sequences the backend
emits most:

| | | replaces |
|---|---|---|
| `LDL n` | `a = *(bp+n)` | `LEA n; LI` |
| `STL n` | `*(bp+n) = a` | `LEA n; PSH; …; SI`, *and the ordering problem with it* |
| `POPA` | `a = *sp++` | `IMM 0; ADD` |
| `ADDI n` | `a = a + n` | `PSH; IMM n; ADD` |
| `MULI n` | `a = a * n` | `PSH; IMM n; MUL` |

`native.f` emits them when `NOPC` is set; it is **off by default**, so
the committed compiler still emits nothing above `MOD` and its output
still runs on plain `c4`. Nothing else in the tree knows these numbers,
so nothing else can emit or receive them.

Three things the experiment had to get right, and they are the same
three any real version would:

* **`OPCD` had to be taught to refuse them.** It already refuses
  `LEA..ADJ` because an opcode executed through `OPCD` would read its
  operand out of the *caller's* instruction stream; `LDL`, `STL`, `ADDI`
  and `MULI` take an operand too, so they needed the same guard — and
  the debug disassembler needed the same one-line extension. Appending
  an opcode is never just the table.
* **The numbering hole is real.** 66-78 are c4mp's (`CPUI..TRAW`), so
  the enum names them `RS66..RS78` and starts at 79. c4m still traps
  them as `TRAP_ILLOP`; a guest must keep feature-testing with
  `C4I_SMP`, not by asking for a name.
* **The probe could not live in `c4m.c`.** Plain c4 compiles *both* arms
  of an `#ifdef` — `c4.c:74` skips only the `#` line, which is why
  `#if C4_ONLY` in `c4m.c` works the way it does — and `./c4 ./c4m.c` is
  a test (`test-c4m-mem`). An `#ifdef`'d probe using `long long`,
  `fprintf` and function-pointer parameters compiles fine under gcc and
  breaks that test, which is exactly what happened. The probe generates
  an instrumented copy instead.

One guest-visible consequence, recorded rather than hidden: `OPSL`
returns the opcode name string, which is now longer. Nothing in the tree
enumerates it — the full suite is green — but a guest that did would see
the reserved and experimental names.

**Correctness first.** The whole B5 suite — sixty-three words compiled,
called, and compared against the threaded engine — produces a
**byte-identical transcript** with the opcodes on. That is the check that
matters; fewer instructions proves nothing by itself.

**What they are worth to c4th**, on `src/c4th/bench/b5c.f`:

| | base | fused | | vs threaded |
|---|---|---|---|---|
| `DO`/`LOOP` with `I` | 1,700,354 | 1,000,343 | **1.70x** | 20x → **34x** |
| `BEGIN`/`WHILE` with `>R`/`R>` | 3,100,351 | 1,700,347 | **1.82x** | 34x → **63x** |
| loop calling another word | 440,354 | 300,343 | **1.47x** | 45x → **66x** |
| loop reordering the stack | 1,540,357 | 1,280,346 | **1.20x** | 14x → **16x** |
| loop through a `VARIABLE` | 2,200,356 | 1,500,346 | **1.47x** | 32x → **47x** |

and the emitted code is about 30% smaller (56 → 33 instructions for the
counting loop).

**But the interesting number is not c4th's.** `make c4m-fuse`
(`src/c4th/bench/fuse-probe.sh`) generates an instrumented c4m that runs
a greedy peephole over the instructions a workload *actually executes*
and reports how many a given opcode set would remove — a different and
more interesting question than a static count over an image, since the
hot code is a small part of any image. On two real workloads:

| workload | instructions | set A (the five above) | set B (see below) |
|---|---|---|---|
| `c4cc` compiling `c4.c` | 11,373,735 | **13.44%** | **36.14%** |
| `c4sp -R` running c4lc's lexer over `c4.c` | 1,639,556,989 | **23.81%** | **33.84%** |

Set A understates itself — `STL` and `POPA` replace patterns that are not
adjacent pairs, so the counter cannot see them. Set B is what the
instruction profile actually asks for: `LDL`, plus **`LDG` (`IMM g; LI`,
a global read)**, plus `PSHL`/`PSHG` (`LEA n; LI; PSH`, pushing a local
or global), plus the **whole immediate-ALU family** rather than just
`ADDI` and `MULI`. The pair histogram is unambiguous about why — on the
c4sp workload `LEA` alone is **15.7% of every instruction executed**, and
`LEA LI` is 11.8% of all adjacent pairs; on the c4cc workload `PSH IMM`
is 18.6% and `IMM LI` 10.3%.

**So: yes, but not the ones the question started from.** The lever is
**fused frame and global access plus immediate operands**, and it is
worth about a third of every instruction executed by `c4cc` and by the
c4sp that hosts c4lc — which is Track A's problem, not c4th's. That is a
much larger prize than anything c4th alone would justify.

**And the second register is answered by the same data.** Once `LDL`/
`STL` exist, a frame cell costs exactly one instruction to read or write,
which is what a register costs on an interpreted VM — so a second
register buys nothing a frame cell does not, while `LDL`/`STL` give an
unbounded number of them. The places the backend still spends
instructions are three-way permutations and pushes, and a second register
does not fix those either. **Fused access dominates a second register on
this machine**, and it does so for every program, not just c4th's.

**The cost of going across the board is unchanged and is the open
question.** An opcode's number and name live in `c4.c`, `c4m.c`, `c4l.c`,
`load-c4r.c`, `src/c4mp/{c4mp.h,vm.c}`, `src/oisc4/oisc4.c`,
`src/c4cc/c4cc.c`, `src/c4bb/sim/devices.js` and `c4r.lisp` — and c4bb
needs **microcode**, since `src/c4bb/hw/microcode.uc` implements each
opcode individually. Emitting them also means changing `c4cc` and
`c4lc-gen.lisp`, which is where the 36% would actually be collected;
c4or1k's `-mcisc` (`docs/c4or1k-design.md`) is the precedent for gating
that behind a flag. **Everything above is c4m-only and reverts in one
commit** if the answer is no.

- [ ] **B5d** The metacompiler and `.c4r` emission — the `cmp gen2.c4r
      gen3.c4r` fixed point and the `c4opt` byte-identical differential.
      This is also where the C primitives stop being a wall: an image
      whose words are all native does not have a C `th_dstack` for them
      to work on.
- [ ] **B6** c4th inside C4IX. *Verify:* runs from the C4IX shell, output pinned

### Risks

1. **The stack model is where silent wrong answers live.** Mitigated by §8's
   canonicalisation rule and a brutal oracle — but only if B3 precedes B5.
2. **The threaded core will look slow on its own** (~20 VM instructions per
   `NEXT`). It is a bootstrap and an oracle, not a product; overselling it at B3
   makes the project look failed when it has not started.
3. **c4cc's 512 KB pools** — watch as `prim.h` grows.
4. **`core.fr` is unforgiving about double-cell arithmetic** — budget `num.h`.

### References

Brad Rodriguez, *Moving Forth* (the canonical treatment of ITC/DTC/STC/token
threading — it is the B5 decision); *jonesforth* (a literate ITC Forth, the
closest prior art to §3).
