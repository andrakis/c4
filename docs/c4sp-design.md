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
| `'x` not handled by the reader | `'x` reads as `(quote x)` | alisp's C++ tokeniser makes `'env:defined` an atom *named* `'env:defined`; the sample `macros.lisp` only works on the Node build. Worth fixing rather than reproducing. |

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
evaluations. `seval.lisp` evaluating itself will exceed that. Mitigations, in
order of preference:

1. Make argument evaluation iterative (build the evaluated list with an
   explicit loop, not recursion) — removes most of the depth.
2. Raise `TASK_STACK_SIZE` for the c4sp task specifically.
3. If that is still not enough, an explicit continuation stack — a much bigger
   change, and the point at which this becomes a different program.

Errors: no exceptions in C4, so a global error flag plus an error cell,
checked at each loop iteration and propagated up. Ugly but explicit.

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

### 9.1 The missing piece: a symbolic assembly format

Peephole optimization changes instruction *counts*, which invalidates every
absolute address. c4cc currently emits absolute addresses and backpatches them,
and `-S` produces a listing for humans, not for machines.

So the first real work item is a **labelled listing format** that c4cc can emit
and `asm-c4r.c` can consume:

```
func add
    ENT  2
    LEA  3
    LI
    PSH
    LEA  2
    LI
    ADD
    LEV
    LEV
end
```

with `BZ`/`BNZ`/`JMP`/`JSR` referring to labels rather than addresses. This is
useful on its own — it makes c4cc's output inspectable and diffable — and it is
the only thing standing between c4sp and a working optimizer.

### 9.2 Pipeline

```
foo.c --c4cc--> foo.c4s --c4sp+optimizer.lisp--> foo.opt.c4s --asm-c4r--> foo.c4r
```

c4sp reads `.c4s` into cons lists: `((ENT 2) (LEA 3) (LI) (PSH) ...)`, which is
exactly the shape pattern matching wants.

### 9.3 Optimizations worth having

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

### 9.4 Verification

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
| **M5** | `.c4s` symbolic listing: c4cc emits, asm-c4r consumes, round-trips | `foo.c -> .c4s -> .c4r` matches direct compilation |
| **M6** | Optimizer passes in Lisp | Test suite passes optimized; instruction counts drop |

M0–M2 is the risky part and is mostly mechanical now that the GC question is
settled. M5 is the one that unlocks the actual goal and could be done
independently of the Lisp work.

## 11. Open questions

* **Does `seval.lisp` fit?** It is the best available stress test, but it is
  also the deepest recursion. If M3 cannot run it even with an enlarged stack,
  the explicit continuation stack from §6 moves from "maybe" to "required".
* **Arena sizing under C4KE.** 2 MB of cells inside a kernel whose tasks
  normally use 64 KB stacks is a large tenant. It may want to be a service
  rather than an ordinary task.
* **Is binary32 enough?** The optimizer needs no floats at all; sample programs
  do. If `double` becomes necessary the library from §2 needs a 128-bit
  multiply path.
* **Numeric atoms in the reader.** alisp's parser tries integer then float;
  with binary32, `123456789` is not exactly representable, so the integer path
  must win whenever the token has no `.` or exponent.
