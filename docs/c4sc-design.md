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

## What it is actually worth — and a correction

`docs/compiler-speed.md` says compiling is "worth perhaps 10-30x". **That
number does not follow from its own profile and should not be relied
on.** Removing 41% + 25% + 9% = 75% of the work is 4x, not 30x.

The honest arithmetic:

- **4x** if compilation removed only dispatch, environments and builtin
  dispatch, and nothing else changed.
- Environments in c4sp are *alists*, so every parameter binding conses.
  Compiling makes locals into C locals, so a large share of the 12% GC
  and consing goes with the environments rather than surviving them.
- Atom interning (4%) mostly moves to startup.

That puts the realistic figure at **5-8x, not 10-30x**, which is still
the difference between thirty-one minutes and **four to six** — the first
thing on this ladder that reaches the word "minutes".

**M0 exists to settle this before anyone writes a compiler.** Guessing
this number wrong is exactly the mistake the `native.f` attempt made
(`docs/c4th-design.md`), and the lesson from that is to measure the
ceiling first and cheaply.

## Risks

1. **The generated C may be bigger than c4lc has ever compiled.** c4lc's
   largest input today is ~7,300 preprocessed lines (`c4cc.c`). 5,016
   lines of Lisp at a 2-4x expansion is 10,000-20,000. Mitigated by the
   per-file object build, which caps a unit at ~5,000 lines — but it is
   the risk most likely to bite, and M1 measures the expansion factor on
   one real file before committing.
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

- [ ] **M0** *Measure the ceiling before building anything.* Hand-write
      the C for the three hottest functions (from a fresh callgrind run),
      link them into c4sp as builtins, and time `c4lc -O -c` on a C4IX
      module against the interpreted originals. **If the extrapolated
      whole-program figure is under 3x, stop and say so.** Budget: a day.
- [ ] **M1** *Expansion factor.* c4sc emits C for `c4opt.lisp` (445
      lines, the smallest self-contained file) only. Measure the
      generated line count and that `c4lc -O -c` compiles it. No
      execution yet.
- [ ] **M2** *One file runs.* `c4opt.lisp` compiled and linked against
      c4sp's runtime, driven by `c4opt-run.lisp`'s entry point: the
      images it rewrites must be **byte-identical** to the interpreted
      pass's, across the existing `test-c4sp-opt` corpus.
- [ ] **M3** *The lexer.* `c4lc-lex.lisp` (375 lines). Token dump ==
      `expected/c4lc-tokens.txt`; `-count` on `c4cc.c` == 15,125.
- [ ] **M4** *The rest of the front end.* `c4lc-pp.lisp`,
      `c4lc-parse.lisp`. Preprocessed output byte-identical to `gcc -E`
      for all twelve C4IX modules; AST dump == the committed expected.
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
