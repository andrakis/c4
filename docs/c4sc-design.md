# c4sc — compiling the Lisp instead of interpreting it

Scope for the last row of `docs/compiler-on-the-board.md`'s table: the
step that takes building C4IX inside c4bb from about thirty-one minutes
to minutes. Nothing here is built yet. This document exists to be argued
with before anything is.

**Rule for this document:** tick a box only when its verification command
has been run green, with the evidence pasted beside it. Anything found
mid-implementation that changes a later milestone gets written here.

## Where we are

| | C4IX on c4bb | |
|---|---:|---|
| in-machine, as found | ~2.5 h | |
| `-R` in the in-machine invocations | ~57 min | banked |
| the ten fused opcodes on the board | ~31 min | banked (F7) |
| **compiling the Lisp** | **this document** | |

c4sp is a tree-walking interpreter and c4lc is 5,016 lines of Lisp that
runs on it. `docs/compiler-speed.md` re-profiled it after every cheap win
was taken and found no hot spot left: ~41% `eval` dispatch, ~25%
environments, ~12% GC, ~9% builtin dispatch, 4% atom interning. That is
the shape of "a tree-walking interpreter is simply slow", and the only
thing that removes it is not interpreting.

## The survey, and why it changes the size of the job

Taken 2026-08-26 over all of `c4lc*.lisp`, `c4r.lisp` and `c4opt.lisp`
— 5,016 lines, the whole compiler:

| | count | what it means |
|---|---:|---|
| `lambda` | 397 | |
| ...of which `(define name (lambda …))` | **397** | **every one is a named top-level function** |
| anonymous lambdas | **0** | no closures to convert |
| functions that call one of their parameters | **0** | **no higher-order calls** |
| variadic lambdas | **0** | every arity is fixed and known |
| `call/cc` | **0** | |
| `apply` / `eval` | **0** | |
| `macro` / `fastmacro` | **0** | nothing to expand at compile time |
| vectors | **0** | |
| top-level `define` | 475 | globals and functions |
| internal `define` | 502 | locals |
| ...functions binding the same internal name twice | 6 | the only hoisting hazard |
| `next` (tail call) → itself | **224** | becomes a loop |
| `next` → another function | ~50 | becomes an ordinary call |
| `if` | 1,145 | |
| `set!` | 285 | |
| quoted literals `'x` / `'(…)` | 89 | built once at startup |

**So c4lc is a first-order program with global functions and local
variables, and it is written in about eight forms.** That is not a Scheme
to compile. It is C with different parentheses.

This is the fact the whole plan rests on, and it is worth stating
plainly: **no closure conversion, no lambda lifting, no CPS, no
trampolining for anything but tail calls, no runtime `eval`.** The hard
parts of compiling a Lisp are all absent from the only Lisp program we
care about making fast.

## The design

**Transliterate to C, and let c4lc compile the C.**

    c4lc.lisp  --c4sc-->  c4lc_gen.c  --c4lc -O -c-->  c4lc.c4o  --c4rlink-->  c4lc.c4r

- **No new code generator.** c4lc is already a C compiler that emits
  `.c4r`, is byte-identical against gcc across a corpus, and has object
  mode. It is the backend.
- **No new runtime.** Values stay exactly what they are today: c4sp
  cells, allocated by `gc_alloc_cell`, with `cells.h`'s accessors.
  Generated code calls the same `stdlib.h` builtins directly, which is
  where the 9% builtin dispatch goes.
- **No new collector, and no rooting work.** `gc.h` is *conservative over
  the C4 stack*, and its own comment records that under c4m "the scan is
  exact by construction". Generated C keeps its live cells in locals,
  locals live on the C4 stack, so they are rooted for free. **This is the
  single most important enabling fact and it is already true.**
- **One `.c4o` per `.lisp` file, linked by c4rlink.** Same shape as
  C4IX's own build, and it caps any translation unit at ~1,500 lines of
  Lisp. See the size risk below.

### The eight forms

| form | becomes |
|---|---|
| `(define f (lambda (a b) …))` | `int *f (int *a, int *b) { … }` |
| `(define x e)` at top level | a global, initialised in `c4sc_init()` |
| `(define x e)` inside a body | a local, hoisted to the top of the function (c4 has no mid-block declarations) |
| `(if c t e)` | a statement with a result temporary, since arms contain statements |
| `(begin …)` | a statement sequence; the last value is the result |
| `(set! x e)` | assignment |
| `(next f …)` | self: assign the parameters, `continue` a `while (1)`. Other: an ordinary call |
| `'lit` | a cell built once in `c4sc_init()` |
| a call | a call |

Everything else in the file is a builtin, and a builtin is a C function
that already exists.

### What c4sc itself is written in

The same Lisp subset, so it runs on c4sp today and on its own output
later. `gen1 == gen2 == gen3` is the fixed point, exactly as `self.f`
does it in `docs/c4th-selfhost.md`. Estimated 900-1,400 lines — it is a
pattern match over eight forms plus a symbol table.

## What it is actually worth — measured, not argued

**M0 is done and the answer is 16x to 44x on the machine that matters.**

Two kernels, each written twice: once in the c4sp Lisp, once as C over
c4sp's own cells and collector — which is what c4sc would generate.
Kernel A is list walking (`head`, `tail`, `empty?`, `=`, `+`), which the
Lisp-level profile below says is c4lc's dominant shape. Kernel B is
`fib`, two non-tail *user* calls per node, the pessimistic bracket.
`src/c4sp/bench/` holds both and how to run them.

| | interpreted | compiled | |
|---|---:|---:|---:|
| **A native** | 2.435 s | 152.8 ms | **15.9x** |
| **A hosted (c4m)** | 8m14s | 11.2 s | **44.1x** |
| **B native** | 543.5 ms | 118.6 ms | **4.6x** |
| **B hosted (c4m)** | 1m26s | 5.3 s | **16.2x** |

**Hosted beats native, and that is the whole point.** Natively you remove
one level of interpretation out of one. Hosted you remove one out of two,
and the level that survives is itself a VM. So the machine that needs the
win most is the one that gets the most of it — c4bb, where C4IX is built
by `c4lc.lisp` interpreted by `c4sp.c4r` interpreted by the board, and
would instead be built by `c4lc.c4r` interpreted by the board.

### Why kernel A is the right shape

A scratch Lisp-level profiler in `eval`'s application path (counting the
name in head position, which is what a compiler turns into a C function)
over `c4lc -O -c src/c4ix/sched.c`:

    applications: 2,867,236
      20.67%  head          1.95%  string:byte
      16.34%  =             1.59%  opt:is
      11.17%  tail          1.26%  reverse/2
      10.38%  empty?        1.26%  cons
       6.84%  +             1.25%  lex:kwlook
       3.78%  list          1.02%  p:assoc/atom

**76% of all applications are builtins** — which are already C. What
compiling removes is not their bodies but everything around them: the
`eval` dispatch, the environment lookup, the argument list consed for
every call, and the builtin if-chain. Kernel A is that, exactly.

### Two corrections, and I got the second one wrong too

`docs/compiler-speed.md` said "worth perhaps 10-30x". I argued that down
to 5-8x, reasoning that removing 41% dispatch + 25% environments + 9%
builtin dispatch is 4x. **That arithmetic was wrong**, and the
measurement says so: it ignored that the interpreter conses an argument
list *per application*, so removing applications removes most of the
allocator traffic and the GC work with it, and it ignored that the
hosted case removes a whole level rather than a share of one.

Natively, 4.6x-15.9x — near my range. Hosted, 16.2x-44.1x — near the
original one. Both documents were partly right about different machines,
which is exactly why M0 was a measurement and not a paragraph.

### What that means for the board

C4IX inside c4bb is about 31 minutes today (`-R` plus the fused
opcodes). c4lc compiled, taken conservatively at the pessimistic bracket
of 16x, is **under two minutes**. At the shape c4lc actually has, less.

## Risks

1. **~~The generated C may be bigger than c4lc has ever compiled.~~**
   **Retired by M1, but only just, and with one condition.** At the
   measured 4.46x the whole corpus is ~22,400 lines of C — four times
   anything c4lc compiles today — but it is twelve units, and the
   largest, `c4lc-gen.lisp` at 1,534 lines, comes to about **6,800**,
   just under c4lc's current largest input (`c4cc.c`, 7,313
   preprocessed). So it fits **per unit**.

   The condition: M1's generated file `#include`s the whole of c4sp's
   runtime, which is another ~3,100 preprocessed lines *per unit*. That
   is fine for one file and not fine for twelve. M2 has to make the
   runtime one separate object that the generated units declare rather
   than contain — the machinery already exists, since c4sc already emits
   `extern` declarations for the names `c4opt.lisp` uses from
   `c4r.lisp`.
2. **Stack depth.** Compiled code recurses on the C4 stack. This is
   already retired: `-R` does exactly that and has run the whole corpus
   since A1.2. If it were going to blow, it would have blown then.
3. **The six double-bound internal names.** Hoisting two `define`s of the
   same name in one function to one C local changes meaning if they
   overlap. Six sites; read them, rename them, or refuse them loudly.
4. **`set!` on a captured variable** — cannot happen, there is nothing to
   capture.
5. **Divergence between compiled and interpreted c4lc** is the failure
   that would matter, and it is also the one thing here with a perfect
   oracle: the interpreter, on every input, byte for byte.

## Milestones

Each rung is verified against the interpreter, which is a working oracle
for all of them.

- [x] **M0** *Measure the ceiling before building anything.* Done
      differently from the plan and better: the plan said "hand-compile
      the three hottest functions", but the Lisp-level profile showed the
      three hottest are `head`, `=` and `tail` — **builtins, already C**.
      So the thing to measure was the call protocol around them, which is
      what the two kernels in `src/c4sp/bench/` do.

      **Gate was 3x. Measured 16.2x-44.1x hosted, 4.6x-15.9x native.**
      Proceed.
- [x] **M1** *Expansion factor.* `src/c4sc/c4sc.lisp` (the compiler,
      ~430 lines) and `src/c4sc/scrt.h` (the runtime shim, 115) emit C
      for `c4opt.lisp`, and `c4lc -O -c` compiles it. `make test-c4sc`:

          test-c4sc: OK -- 445 lines of Lisp -> 1985 lines of C,
          and c4lc -O -c compiles it

      **4.46x, not the 2-4x this document guessed.** The first emitter
      gave 5.83x; inlining arguments that are just a variable or a
      literal — instead of giving every one its own temporary and its
      own statement — took a third off, and that was worth doing before
      recording a number rather than after. `opt:drop` comes out as:

          int *L_opt_58drop (int *L_L, int *L_N)
          {
              int *r; int *t0; int *t1;
              while (1) {
              t0 = sc_eq(mk_int(0), L_N);
              if (sc_true(t0)) { r = L_L; } else {
              t0 = sc_tail(L_L);
              t1 = sc_sub(L_N, mk_int(1));
              L_L = t0; L_N = t1;
              continue; }
              return r; }
          }

      — the self tail call as a `continue`, the literals hoisted into
      `sc_init`, and nothing else invented.
- [x] **M2** *One file runs, and it is right.* `make test-c4sc-run`:

          ok: factorial.c4r (image and pass counts identical)
          ok: c4cc.c4r (image and pass counts identical)
          ok: c4ke.c4r (image and pass counts identical)
          ok: tests.c4r (image and pass counts identical)
          ok: test_globals.c4r (image and pass counts identical)
          ok: mandel.c4r (image and pass counts identical)
          test-c4sc-run: OK

      `src/c4sc/c4sc-host.c` is `c4opt-run.lisp` with exactly one
      substitution: c4r.lisp's decoder and encoder still run in the
      interpreter, reached through `src/c4sc/host.h`'s bridge, and only
      the optimizer is compiled. Both the images and the per-pass
      counters match, with `-mfuse` as well (341 fusions either way).
      Built at c4sp's own `-O2`.

      **M1's condition is discharged.** The generated unit now declares
      the runtime instead of `#include`ing it, so it stands alone: no
      preprocessing at all, and `c4lc -O -c` produces a 154,049-byte
      object where the self-contained version produced 397,353.

      **Two things the design got wrong, both found here.**

      1. **Compiled globals are not rooted.** This document said
         compilation needs "no rooting work at all" because the
         collector scans the C4 stack. That is true of locals and false
         of globals: a generated unit keeps its literal table and its
         top-level defines in C globals, which are not on the stack.
         `gc.h` gains `gc_add_root`, a list of **slots** rather than
         values — a global like a counter is reassigned, so rooting the
         cell it held at startup would root the wrong thing after the
         first `set!`. c4sc emits a registration before each global's
         initialiser. Ten lines in the collector; every c4sp suite still
         green.
      2. **A self tail call may not inline its arguments.** Assigning
         the parameters happens in order, and an argument may name a
         parameter an earlier assignment has already overwritten.
         `c4opt:optloop` is exactly that shape — it passes the old `N`
         as `Prev` — so the fixpoint loop ran **one round** and stopped.
         Arguments to a self tail call now go to temporaries first.

         `factorial.c4r` came out byte-identical **with the bug still in
         it**, because one round happened to reach its fixpoint. Only
         `c4cc.c4r` — 423 KB, 26,818 instructions — showed it, as
         24,623 instructions against 24,617. That is the argument for
         having the big images in the corpus rather than the quick
         ones.
- [x] **M3** *The lexer.* `make test-c4sc-lex`: the dump and the
      `-conforming` dump both match the committed expected files,
      `-count` on `c4cc.c` is 15,125, and the whole of test-c4fc's lexer
      sweep — thirteen files, ~67,000 tokens — is identical to the
      interpreter's, `c4cc.c` included. 375 lines of Lisp → 1,601 of C
      (4.27x, in line with c4opt's 4.46x), and `c4lc-lex.lisp` needs no
      externs at all.

      **The bar had to be the DUMP, not the count.** The first version
      counted 15,125 tokens — exactly right — while lexing every
      keyword as an identifier. `c4lc-lex.lisp`'s keyword table is a
      quoted literal, `'(("char" Char) ("else" Else) ...)`, and c4sc was
      materialising quoted data by printing it back and re-reading it.
      The printer does not quote strings, so `"char"` came back as the
      atom `char` and `lex:kwlook` never matched. Quoted data is now
      built **structurally** — cons by cons, `sc_str` for a string and
      `sc_atom` for an atom — which is exact by construction and drops
      the runtime reader dependency entirely.

      **And a shim that delegates is not a shim.** `sc_str_byte` was
      `builtin_call(B_STR_BYTE, cons(s, cons(i, 0)), 0)` — which conses
      an argument list and walks the thirty-way if-chain, reintroducing
      one layer down exactly the call protocol compiling exists to
      remove. Invisible on c4opt, which barely touches strings;
      immediate on a lexer. `string:byte`, `string:byte!` and
      `string:substr` are now written out against cells.

      **Speed, on the first rung that is on c4lc's real hot path.**
      Lexing eight concatenated copies of `c4cc.c` (16,872 lines,
      120,993 tokens, both sides agreeing):

      | | wall | user |
      |---|---:|---:|
      | interpreted | 1.019 s | 0.945 s |
      | **compiled** | **191 ms** | **117 ms** |
      | | **5.33x** | **8.1x** |

      On `c4cc.c` alone it is 1.73x wall, because at that size half the
      wall clock is arena setup in both — which is why the measurement
      uses an input big enough for the work to dominate. Between M0's
      two brackets (4.6x and 15.9x native), which is where a real
      workload should sit.
- [x] **M4** *The rest of the front end.* `make test-c4sc-front`: the
      compiled preprocessor's token stream is identical to `gcc -E`'s
      across **all twelve C4IX modules** (the differential test-c4fc
      uses — gcc preprocesses and we lex, we preprocess and we lex), the
      AST dump matches the committed expected file, and all twelve
      modules parse to the same declaration counts as the interpreter.
      576 → 2,549 lines and 888 → 3,869 (4.43x and 4.36x). All four
      units compile standalone under `c4lc -O -c`.

      **Three bugs, and the third is the one worth remembering.**

      1. `sc_init` used temporaries it never declared — a top-level
         `(define x (f (g y)))` needs one, and C4 has no mid-block
         declarations. Invisible in the first three units because none
         of their top-level forms is a nested call.
      2. c4 spells an empty parameter list `()`, not `(void)`. c4opt and
         the lexer have no zero-argument functions; `pp:epeek` and
         `p:kind` are full of them.
      3. **Every unit named its literals `Q0, Q1, …`, and four units in
         one translation unit merged them into single C globals.** The
         lexer's `Q50` was the atom `Id`; the preprocessor's `Q50` was
         the empty string; `sc_init_pp` ran second and won, so every
         identifier came out of the lexer with an empty kind and the
         preprocessor decided `main` was a macro. Literals are now named
         per unit. This was never going to stay a host-only problem —
         c4rlink links separate objects into one symbol namespace, so
         M6 would have hit it with the collision spread across files.

      And one more from the AST dump, which is why that dump is the bar
      rather than a declaration count: `'(continue)` was being built as
      the improper list `(continue . nil)`. The end of a list is the
      **null cell**, and `typeof` calls that an atom — the same answer it
      gives the atom `nil` — so the terminator has to be recognised with
      `empty?` as well. One printed dot, in one place, in one file.
- [ ] **M5** *The back end.* `c4lc-gen.lisp` (1,534 lines),
      `c4lc-tree.lisp`, `c4r.lisp`. Emitted `.c4r` **byte-identical to
      the interpreter's** across the whole `C4LC_DIFF` corpus.
- [ ] **M6** *The whole compiler.* `c4lc.c4r` as a native image. C4IX
      rebuilt with it: twelve objects byte-identical to today's, linking
      to a kernel byte-identical to the committed `c4ix.c4r`, which
      boots.
- [ ] **M7** *The fixed point.* c4sc compiles itself: gen1 == gen2 ==
      gen3.
- [ ] **M8** *On the board.* `c4lc.c4r` at 32 bits, `-mfuse`, on the
      c4bb disk. Re-time one C4IX module and the twelve-module build, and
      write the number into `docs/compiler-on-the-board.md`.

## What this is not

- **Not a Scheme.** The survey is the specification: eight forms, fixed
  arity, no closures. A form c4lc does not use is a form c4sc refuses,
  loudly, the way `self.f` refuses a word it does not know.
- **Not a replacement for c4sp.** The interpreter stays: it is the
  oracle, it is what runs a `.lisp` you are editing, and it is what makes
  the REPL a REPL.
- **Not a rewrite of c4lc.** Not one line of the 5,016 changes. That is
  the point — c4lc's L7, its object mode and its byte-identity all come
  along untouched.
