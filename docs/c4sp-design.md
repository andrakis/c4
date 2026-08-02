# c4sp — C4 liSP

A design for a Lisp interpreter written in C4, modelled on
[alisp](https://github.com/andrakis/alisp), whose eventual purpose is to host a
**C4 optimizer written in Lisp** rather than in C4.

Status: plan. Nothing is implemented except the garbage-collector spike in
§4.6, which exists because the GC was the part of this that looked hardest and
a plan resting on an unproven mechanism is not worth much.

---

## 1. Why

Writing a peephole optimizer in C4 means writing pattern matching over
instruction sequences in a language with no structs, no arrays, no `break` and
one accumulator. Writing it in Lisp means writing:

```lisp
(defrule (IMM ?a) (PSH) (IMM ?b) (ADD)  =>  (IMM (+ ?a ?b)))
```

The interpreter underneath is unavoidably low-level; that cost is paid once.
Every optimization pass after it is a few lines of list manipulation. That is
the whole argument for the project, and it is a good one.

The secondary payoff: C4KE gets a scripting language, and `seval.lisp` — the
alisp evaluator written in alisp — becomes a serious stress test for the
kernel.

## 2. What is kept from alisp, and what changes

Kept, because these are the ideas that make alisp what it is:

| Concept | Notes |
|---|---|
| Specialised cell types | `Nil Atom Int Float String Cons Lambda Macro FastMacro Proc ProcEnv Env` |
| Interned atoms | global id ↔ name table; comparison is an integer compare |
| Environment chain | frame with a parent pointer; `define` creates locally, `set!` walks up |
| Special forms | `quote if define set! lambda macro begin` |
| `next` | transfer control reusing the current environment — tail calls without env churn |
| `fastmacro` | macro that reuses the environment captured at declaration |
| Trampolined eval | alisp's `goto recurse`; C4 has no `goto`, a `while (1)` loop is identical |

Changed, with reasons:

| alisp | c4sp | Why |
|---|---|---|
| `std::vector<Cell>` lists | **cons cells** | Fixed-size cells make the GC arena uniform, and `tail` becomes O(1) instead of an O(n) copy. Surface language is unchanged. |
| `unordered_map` environments | assoc list of cons cells, hashed for the global frame | No hash map in C4. Local frames hold 1–5 bindings, where a linear scan wins anyway. |
| `shared_ptr` lifetime | **mark & sweep GC** | See §4. Reference counting cannot work here. |
| `double` floats | binary32 via `include/c4_float.h` | Already built, already tested, already runs under plain c4. |
| C++ exceptions | error cell + an error flag checked by the eval loop | C4 has no exceptions or `setjmp`. |
| `'x` not handled by the reader | `'x` reads as `(quote x)` | alisp's C++ tokeniser made `'env:defined` an atom *named* `'env:defined`, so `macros.lisp` and `seval.lisp` only worked on the Node build. **Fixed upstream** in alisp `efdf948`; c4sp inherits the corrected behaviour. |

## 3. Cell representation

C4 has no structs, so a cell is four consecutive words, addressed through an
enum — the same idiom as C4KE's task struct.

```c
enum { CELL_TYPE, CELL_A, CELL_B, CELL_C, CELL__Sz };   // 4 words
```

| Type | `CELL_A` | `CELL_B` | `CELL_C` |
|---|---|---|---|
| `T_INT` | value | | |
| `T_FLOAT` | binary32 bit pattern | | |
| `T_ATOM` | atom id | | |
| `T_STRING` | `char *` (malloc'd) | length | |
| `T_CONS` | car | cdr | |
| `T_LAMBDA`/`T_MACRO`/`T_FASTMACRO` | args | body | env |
| `T_PROC` | builtin id | arity | |
| `T_ENV` | bindings (assoc list) | parent env | |

`nil` is the null pointer, `0`. That makes the commonest test in the whole
interpreter `if (!x)`, and costs nothing.

Four words rather than three wastes one word per cons. That buys a single
uniform cell size, which is what makes the allocator and collector as simple
as they are. Worth it.

## 4. Memory and garbage collection

This is the part the project has always stalled on, so it gets the most detail.

### 4.1 Why not reference counting

In Lisp, `(define f (lambda ...))` puts a lambda in an environment, and the
lambda holds that same environment so it can close over it. That is a cycle,
and it is created by *the most ordinary thing a program can do*. Reference
counting would leak on essentially every program. Ruled out.

### 4.2 Why not a copying collector

Cheney semispace copying is attractive — allocation becomes a pointer bump,
and it compacts. But it **moves objects**, so every reference held in a C4
local must be found and updated. That requires precise roots, which means
GCPRO-style discipline in every interpreter function. In a language with no
destructors, one missed pop is a memory-corruption bug that appears under load,
weeks later. Ruled out for the prototype; revisit if allocation rate becomes
the bottleneck.

### 4.3 Chosen: non-moving mark & sweep, conservative on the stack

* **Arena**: one `malloc` of `NCELLS * CELL__Sz` words. Cell references are
  real pointers into it.
* **Free list**: swept cells are threaded through `CELL_A`. Allocation is a
  pop; O(1).
* **Mark bitmap**: a separate `NCELLS`-byte array, so marking never touches
  cell contents and sweep is a linear scan.
* **Collect when** the free list is empty, plus an explicit `(gc)` builtin.

Non-moving is the key choice. It means a conservative guess about what is a
pointer can only ever *retain* a dead cell — never corrupt a live one. That in
turn means the interpreter needs no root-registration discipline at all.

### 4.4 Roots

1. The global environment.
2. The atom table.
3. A handful of interpreter globals (current expression, current env, the
   value being returned).
4. **The C4 stack**, scanned conservatively.

Everything the interpreter holds is either a global or a C4 local, and every
C4 local lives on the stack. So this set is complete — *provided* the design
rule in §4.7 is respected.

### 4.5 Scanning the stack

C4's stack grows down, and inside a function `&<first local> + 1 == bp`. So the
collector takes the address of its own first local as the low end, and scans
upward to a stack base recorded once at startup:

```c
int *p, *top;
top = (int *)(&p + 1);          // this frame's bp
p = top;
while (p < stack_base) {
    if (looks_like_cell(*p)) mark(*p);
    ++p;
}
```

`looks_like_cell` is two comparisons and an alignment check, because every cell
lives in one contiguous arena:

```c
static int looks_like_cell (int w) {
    int off;
    if (w < (int)arena) return 0;
    if (w >= (int)(arena + arena_words)) return 0;
    off = w - (int)arena;
    return (off % (sizeof(int) * CELL__Sz)) == 0;
}
```

### 4.6 This is verified, not assumed

`src/tests/test_gcscan.c` builds three nested frames each holding a pointer
into a pretend arena, then scans from the innermost frame and checks it finds
them all. It passes under **c4m and under plain c4**:

```
stack base 0x792846a8afe0, arena 0x5c7da62ba120..0x5c7da62ba320
level1 holds 0x5c7da62ba120
level2 holds 0x5c7da62ba140
level3 holds 0x5c7da62ba160
  found cell pointer 0x5c7da62ba160 at stack slot 0x792846a8af40 (depth 20)
  ...
scan found 6 cell-looking words (expected at least 3)
PASS: stack scanning works
```

Six rather than three because each pointer appears both as a local and as the
argument slot of the next frame. Conservative scanning finding a reference
twice is harmless.

### 4.7 The one design rule

> **Every structure that can hold a cell reference must itself be a cell in the
> arena, or be registered as an explicit root.**

Environments are cells. The atom table is cells. If some future component
stashes a cell pointer in a `malloc`'d block outside the arena, the collector
will not see it and the cell will be freed while still in use. This is the
single invariant that keeps the design sound, and it should be stated at the
top of the GC source file.

### 4.8 Marking without blowing the stack

Naive recursive marking uses one C4 frame per cons, so a 10,000-element list
would exhaust a C4KE task stack. Instead:

* an explicit worklist (a malloc'd array of cell pointers), and
* cdr-iteration: when marking a cons, loop along the cdr chain and push only
  the cars.

Depth then follows structure nesting, not list length.

### 4.9 Strings

`T_STRING` holds a `malloc`'d buffer. Sweep frees it when the cell dies. A
conservative false positive delays that free by one cycle; nothing worse.

### 4.10 Costs, honestly

* GC pause is O(arena + stack size). A 64K-cell arena is 2 MB; a sweep is a
  linear pass over 64K entries. Fine interactively, visible in a tight loop.
* Conservative scanning retains some garbage. Bounded by stack depth, and
  measurable — the collector should report retained-vs-freed so this can be
  watched rather than guessed at.
* A larger task stack makes GC slower, because the scan is proportional to it.

## 5. Reader and printer

A direct port of `parser.cpp`: tokenise into `(`, `)`, `"strings"`, `;;`
comments and generic tokens; `read_from` builds nested lists. Additions:

* `'x` → `(quote x)`.
* Numbers: integer if it parses as one, otherwise binary32 via
  `f32_from_string`.
* Anything else interns as an atom.

The printer is the inverse, and is also how the optimizer will emit its output,
so it needs to be exact rather than pretty.

## 6. Evaluator

alisp's `eval` with `goto recurse` becomes:

```c
while (1) {
    ... /* x and env reassigned instead of recursing */
}
```

Special forms as in alisp: `quote if define set! lambda macro begin`, plus
`fastmacro` and `next`. Tail position for `if`, `begin` and lambda bodies
reassigns `x`/`env` and loops. Non-tail positions (evaluating arguments) still
recurse in C4.

**Recursion depth is the real constraint.** A C4KE task stack is 0xFFFF bytes
= 8191 words; an eval frame is perhaps 12 words, so roughly 600 nested
evaluations. That is fine for ordinary programs and not fine for `seval.lisp`
evaluating itself. §6.1–6.3 work out what to do about it.

Errors: no exceptions in C4, so a global error flag plus an error cell,
checked at each loop iteration and propagated up. Ugly but explicit.

### 6.1 Two ways to stop using the C4 stack

Both amount to the same idea — move interpreter state out of C4 locals and
into the heap — but they reify different things.

**Elispidae's reified frames.** `PLint_stackless.cpp` gives each expression a
`LithpFrame` holding the expression, its subexpression list and an iterator
into it, the argument list and an iterator, the resolved arguments, and a
`subframe` pointer. `execute()` performs one step and returns. Frames form a
parent→child chain on the heap.

Two things are worth noting before copying it:

* Its purpose is **microthreading**, not depth relief. The `FrameWaitState`
  enum (`Receive`, `Sleep`, `Run_Wait`) and `MicrothreadManager` are the point
  — it exists so a computation can be paused, a message delivered, and the
  computation resumed. Erlang-style concurrency.
* It is not actually stackless in the native sense. `LithpFrame::execute`
  calls `subframe->execute(impl)`, and `deepestFrame()` recurses too, so
  native stack depth per step is still proportional to Lisp nesting depth. To
  get depth relief you would keep a pointer to the deepest frame and step it
  directly, following parent links back up — a small change, but a change.

**Defunctionalized continuations (CEK).** State is three values: `control`
(the expression being evaluated), `env`, and `kont` (what to do with the value
once it exists). The evaluator is one loop alternating between two modes:

```
EVAL:   decompose control; either produce a value (-> RETURN),
        or push a kont frame and descend into a subexpression
RETURN: pop a kont frame, consume the value, and either
        produce another value or descend again
```

Native stack depth is O(1) regardless of program nesting.

### 6.2 Recommendation: continuations, represented as cons cells

For c4sp specifically, continuations win, for reasons that are mostly about
C4 rather than about Lisp:

1. **The data structure already exists.** "The arguments still to evaluate" is
   just the cdr of the argument list. Elispidae needs vectors and iterators
   because alisp/Lithp lists are vectors; c4sp uses cons cells, so the
   continuation *is* the list. Materially less code.

2. **The GC gets it for free.** A kont frame is a cell, and the chain is a
   list, so §4.7 is satisfied with no extra work. Elispidae's frames are
   `new`-allocated C++ objects; the C4 equivalent would be malloc'd blocks
   outside the arena, which is exactly the thing §4.7 forbids — they would
   have to become cells anyway.

3. **It makes the collector cheaper, not more expensive.** With a CEK loop the
   C4 stack stays flat, so the conservative scan of §4.5 has almost nothing to
   walk and the real roots reduce to `control`, `env`, `kont` and the globals.
   The two hard problems solve each other: stack scanning stops being the
   primary root-finding mechanism and becomes a safety net for builtins that
   hold cells in locals.

4. **Resumability comes free anyway.** The entire machine state is three
   words. Saving them *is* saving the computation, which is cheaper than
   Elispidae's frame objects and gives C4KE what it actually wants: the
   interpreter can yield to the scheduler between steps, and Elispidae-style
   microthreading later costs three words plus a kont chain per Lisp thread.

5. **`call/cc` falls out.** If kont frames are immutable, capturing a
   continuation is copying a pointer. alisp has no equivalent.

The frame set is small. Each frame is one cell (`type` + three slots), chained
by consing onto `kont`:

| Frame | Slots | Pushed when |
|---|---|---|
| `K_ARG` | evaluated-so-far, remaining, env | evaluating a call's arguments |
| `K_IF` | conseq, alt, env | evaluating an `if` test |
| `K_DEFINE` | symbol, env | evaluating a `define` value |
| `K_SET` | symbol, env | evaluating a `set!` value |
| `K_BEGIN` | remaining, env | mid-`begin` |
| `K_MACRO` | env | re-evaluating a macro expansion in the caller's env |

Three slots is exactly enough for the widest of them. Tail positions push
nothing, which is what makes tail calls and `next` free rather than special.

### 6.3 Build it second, not first

Do **not** write the CEK machine at M1. Write the ordinary recursive evaluator
first, get the semantics right against the alisp samples, and only then
convert — validating by diffing the two evaluators' output across the whole
sample corpus.

Converting an evaluator whose semantics are already pinned down is a
mechanical, testable refactor. Designing the machine and the language
semantics at the same time, with no oracle to check against, is where this
kind of project usually dies. The recursive evaluator is ~150 lines and is
worth writing purely as the reference implementation; the CEK version is
perhaps 350–450 lines of flat, un-nested C4, which is a shape C4 handles well.

## 7. Builtins

`T_PROC` holds an integer id, dispatched through one function. Two builds:

* **c4m target** (default): `int *f; f(args)` — JSRI, ~3 instructions.
* **plain c4 fallback**: an `if (id == B_ADD) ... else if` chain, selected by a
  compile-time flag in the style the repo already uses for `NO_INLINE`.

The fallback averages ~30 comparisons per builtin call, which is slow but keeps
c4sp runnable under unmodified c4 — worth preserving, given that is the point
of the whole repo.

Initial set, following `stdlib.cpp`: `+ - * / = != < <= > >=`, `not nil?`,
`head tail size index list empty? atom typeof`, `print`, `env:get env:set!
env:define env:defined env:new env:capture env:recapture`, `cell:lambda
cell:macro cell:lambda_args cell:lambda_body cell:lambda_env`, `file:read
file:exists file:path`, `string:split string:join string:substr length`,
`debug:parse`, and `gc` / `gc:stats`.

## 8. Layout and build

```
src/c4sp/c4sp.c            main, REPL, command line
src/c4sp/include/cell.h    cell layout, constructors, accessors
src/c4sp/include/gc.h      arena, allocator, collector
src/c4sp/include/atoms.h   intern table
src/c4sp/include/read.h    reader and printer
src/c4sp/include/eval.h    the evaluator
src/c4sp/include/stdlib.h  builtins
src/c4sp/lisp/*.lisp       ported alisp samples, then the optimizer
```

Built like everything else in the repo:

```make
c4sp.c4r: $(C4CC) $(C4SP_SRCS) $(U0)
	$(PREPROC) $(U0) src/c4sp/c4sp.c | $(C4CC) -o c4sp.c4r -
```

Watch the pool limits: c4cc gives 256 KB each for text, data and symbols. The
whole interpreter must fit, and c4m/c4cc accept multiple source files
concatenated if splitting becomes necessary.

## 9. The optimizer

### 9.1 The bridge already exists: the .c4r patch table

An earlier draft of this document claimed c4cc emits bare absolute addresses,
and that a new labelled listing format was the first work item. **That was
wrong.** `asm-c4r.c` records a patch entry for every word in the code segment
that holds an address:

| Emitted by | Patch |
|---|---|
| `asmc4r_handler_IMM` | `LT_DATA` if the immediate points into the data segment, `LT_CODE` if into code |
| `asmc4r_handler_JMP` | `LT_CODE` |
| `asmc4r_handler_JSR` | `LT_CODE`, or **the symbol id** when the target is `ATTR_EXTERN` |
| `asmc4r_handler_BZPH` / `BNZPH` | `LT_CODE` placeholders, fixed up later by `asmc4r_handler_UpdateAddress`, which rewrites `LBL_VALUE` so forward branches end up correct |

Each entry is `(type, address, value)` where **both address and value are
offsets**, not absolute addresses — `load-c4r.c` relocates with

```c
if (ptype == C4R_PTYPE_CODE) *(code + paddr) = (int)(code + pvalu);
else if (ptype == C4R_PTYPE_DATA) *(code + paddr) = (int)(((char *)data) + pvalu);
```

So a `.c4r` is already fully relocatable, and — the part that matters here —
**every address word is identified together with its target**. Compiling

```c
int add(int a, int b) { return a + b; }
int main() { int i; i = 0; while (i < 3) { printf("hi %d\n", add(i, 1)); i = i + 1; } return 0; }
```

gives exactly four patches, which is exactly the four addresses in the program:

```
patch type CODE address 0x1c value 0x3c    <- the BZ target
patch type DATA address 0x1e value 0x0     <- the "hi %d\n" string
patch type CODE address 0x28 value 0x1     <- JSR add
patch type CODE address 0x3b value 0x14    <- the loop's backward JMP
```

No c4cc changes are needed. c4sp reads the `.c4r` directly.

### 9.2 Pipeline

```
foo.c --c4cc--> foo.c4r --c4sp + optimizer.lisp--> foo.opt.c4r
```

Reading, in c4sp:

1. Decode the code segment. Opcodes `LEA IMM JMP JSR BZ BNZ ENT ADJ` take an
   operand word; everything else does not, so the stream decodes unambiguously.
2. Every patch target becomes a **label**. Collect them first, then decode.
3. Emit cons lists: `((label L0) (ENT 2) (LEA 3) (LI) (PSH) (BZ L1) ...)`,
   with data references as `(IMM (data 0))` and external calls as
   `(JSR (extern "some_external_function"))`.

Writing is the inverse: lay the instructions out, resolve labels to offsets,
and regenerate the patch table. Because layout changes, *every* patch's address
and value are recomputed — which is precisely what the label form gives you,
and precisely why the label form is needed even though the file format is
already relocatable.

### 9.3 The linker this unlocks

The same table is the basis for the static and dynamic linking you want, and
the format already anticipates it: a **positive** patch type is a symbol
reference rather than a code or data offset, and `asmc4r_handler_JSR` already
emits one for `ATTR_EXTERN` targets. Cross-module references are therefore
already representable in files c4cc produces today.

`src/c4ke/bin/c4rlink.c` documents the whole merge algorithm in its header
comment and gets partway there. Linking two modules today:

```
./c4rlink m1.c4r m2.c4r -o linked.c4r
  : counts - code: 47  data: 16  patches:  2  cons: 0  des: 0  syms: 4
  : entry at m1.c4r+1
./c4rlink: allocating master structure...
(null): link failure
```

It loads both modules, computes merged segment sizes, and picks the entry
point, then fails allocating the master structure. The load-and-plan half
works; the merge, rebase and symbol-resolution half is the part that is
missing.

Finishing it is:

* concatenate code and data segments, remembering each module's base offsets;
* rebase every `LT_CODE`/`LT_DATA` patch by those offsets;
* merge symbol tables — an `extern` symbol that another module defines becomes
  a normal symbol, and patches referring to it turn from symbol-typed into
  `LT_CODE`/`LT_DATA`;
* merge constructor and destructor lists, honouring priority;
* error on any symbol-typed patch still unresolved, unless building a library.

Dynamic loading needs less: `load-c4r.c` already loads and relocates a module
at an arbitrary address. What it lacks is resolving symbol-typed patches
against symbols the *host* already has, which is the same lookup step as
above, done at load time instead of link time.

Worth saying plainly: this is independent of c4sp and probably worth more.
c4sp needs the read/write half of §9.2, which is the same code a linker needs,
so doing the linker first would leave c4sp with less to write.

### 9.4 Optimizations worth having

Ordered roughly by payoff, based on what c4's codegen actually emits:

| Pattern | Becomes | Note |
|---|---|---|
| `IMM a, PSH, IMM b, ADD` | `IMM (a+b)` | constant folding; also `SUB MUL DIV MOD` and the bitwise ops |
| `PSH, IMM 8, MUL` | `PSH, IMM 3, SHL` | every pointer index emits this multiply |
| `ADJ 0` | — | emitted whenever a call takes no arguments |
| `JMP L` immediately before `L` | — | falls through anyway |
| `JMP L1` where `L1: JMP L2` | `JMP L2` | jump threading |
| anything after `JMP`/`LEV` up to a label | — | dead code |
| `IMM 0, PSH, ...` in a `BZ` test | fold the comparison | `!x` compiles to `PSH IMM 0 EQ` |
| `JSR f, ADJ n, LEV` | `ADJ n, JMP f` | tail call — needs frame checking, do last |

Constant folding and the `MUL`→`SHL` reduction alone should be measurable,
because C4 emits `PSH; IMM sizeof(int); MUL; ADD` for *every* subscript.

### 9.5 Verification

The optimizer must be provably safe before it is useful:

1. Optimize each `.c4r` in the repo's test suite, re-run, compare output.
2. Optimize c4cc with itself and check the result still compiles c4ke.
3. Report instructions removed per pass, so regressions in the optimizer show
   up as a number rather than a crash.

## 10. Milestones

| | Deliverable | Proves |
|---|---|---|
| **M0** | Cells, arena, reader, printer. No GC — allocate until full. | Parse/print round-trips `fac.lisp` |
| **M1** | Evaluator: atoms, ints, `quote if define set! lambda begin`, proc builtins | `fac.lisp` produces 3628800 |
| **M2** | Mark & sweep with conservative stack scan; `gc:stats` | A loop allocating millions of cells runs in a fixed arena |
| **M3** | `macro`, `fastmacro`, `next`, tail calls | `macros.lisp`, then `seval.lisp` runs `fac.lisp` |
| **M4** | Strings, floats, file IO, REPL, C4KE integration | `c4sp` runs as a C4KE process |
| **M5** | `.c4r` reader/writer in c4sp, using the patch table for labels | `foo.c4r -> lists -> foo.c4r` is byte-identical |
| **M6** | Optimizer passes in Lisp | Test suite passes optimized; instruction counts drop |

M0–M2 is the risky part and is mostly mechanical now that the GC question is
settled. **M3½ is the CEK conversion of §6.3**, done before `seval.lisp` is
attempted. M5 needs no c4cc changes and shares its code with the linker, so
it could be built first, independently of the Lisp work.

## 11. Open questions

* **How much does the CEK conversion cost in speed?** Every kont push is an
  arena allocation where the recursive evaluator used a C4 stack frame. The
  free-list allocator makes that cheap, but it is a real cost and the two
  evaluators should be benchmarked against each other before the recursive
  one is deleted — if it ever is; keeping it as an oracle has value.
* **Arena sizing under C4KE.** 2 MB of cells inside a kernel whose tasks
  normally use 64 KB stacks is a large tenant. It may want to be a service
  rather than an ordinary task.
* **Is binary32 enough?** The optimizer needs no floats at all; sample programs
  do. If `double` becomes necessary the library from §2 needs a 128-bit
  multiply path.
* **Numeric atoms in the reader.** alisp's parser tries integer then float;
  with binary32, `123456789` is not exactly representable, so the integer path
  must win whenever the token has no `.` or exponent.
