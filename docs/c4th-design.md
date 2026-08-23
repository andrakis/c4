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
- [ ] **B3** `core.f` + the Forth-2012 CORE suite. **The first rung that means
      anything** — everything before it is scaffolding.
      *Verify:* `make test-c4th`
- [ ] **B4** P1 threaded-code peephole + `asm.f` (a C4 opcode assembler in
      Forth). *Verify:* `make test-c4th` green with P1 on; `asm.f` assembles a
      hand-written factorial to bytes identical to `c4cc`'s
- [ ] **B5** Native backend + metacompiler. **Do not start before B3 is
      complete** — the standard suite is the oracle that catches stack-model
      bugs. *Verify:* the c4opt byte-identical differential above; then
      `native.f` compiles itself → `gen2.c4r`, `gen2` compiles it again →
      `gen3.c4r`, `cmp gen2.c4r gen3.c4r`
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
