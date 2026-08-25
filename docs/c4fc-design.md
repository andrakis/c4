# c4fc — a C99 compiler in Forth, and the DSL it is written in

## What this is, and what it is not

`c4fc` is a C99 compiler with c4lc's coverage, written in c4th, emitting
`.c4r`. The interesting half is not the compiler but the **vocabulary it
is written in**: a compiler is the hardest thing to write in stock Forth
and the easiest thing to write in a Forth shaped for it, and the
difference between those two is about four hundred lines.

**It is not a speed play, and the ladder does not gate on speed.** For
the host, C compiled by `gcc -O2` is the ceiling and `c4lcc` (Track A2)
is the most mechanical way to it, with c4lc as a byte-identical oracle
at every step. That case is unchanged. What c4fc gives that nothing else
does is a C99 compiler **inside the machine, today**: c4th already runs
on c4m, C4KE, C4IX and C4DOS from one image (`docs/c4th-design.md`, B6),
so a compiler that runs on c4th runs on all four the day it works.

The premise that started this — "a Forth-hosted compiler would be much
faster" — is answered and the answer is no. The measurements are in
`docs/c4lc-design.md`; the whole C4IX kernel now builds in 1.7 s. Speed
is not why to do this.

## The one idea

**Every phase is a table of rows and a generic over tags. A C feature is
one row per table, and nothing existing is edited.**

That sentence is the design. Everything below is what it takes to make it
true, and `src/c4fc/tests/dsl.f` ends by *running* it: a new construct
added as one `NODE:` line and one `:M` per phase, with nothing above it
touched.

The negative example is `self.f`, which is what happens without this.
Its dispatch is a twenty-arm `DUP n = IF ... EXIT THEN` chain — linear
in the number of constructs, and every new construct edits it. It has
twenty sites doing `n CELLS + @` because it has no records. And it has
**twenty-one `VARIABLE`s whose only job is to hold a value across three
lines**, every one of which is a local that could not be spelled and a
re-entrancy bug waiting for the day the word is called recursively. A
parser is recursive.

## The DSL, in four layers

Layers 0–2 are **built and pinned** (`make test-c4fc`). Layer 3 is the
compiler's own vocabulary and arrives with the phases that need it.

### Layer 0 — Forth-2012 that c4th's kernel does not carry

`src/c4th/forth/ext.f`. Nothing invented: `CASE OF ENDOF ENDCASE` and
`DEFER IS ACTION-OF` are CORE EXT, `VALUE TO` are CORE EXT,
`BEGIN-STRUCTURE FIELD: CFIELD: +FIELD END-STRUCTURE` are the Structures
word set. Standard syntax is chosen deliberately: **a DSL built on it
has a standard test suite waiting, which one built on invented syntax
never does.**

`DEFER` is load-bearing rather than decorative. Recursive descent is one
large mutually recursive family and Forth insists on definition before
use, so the family is deferred first and filled in as each member is
written — which makes the deferral list a written-down statement of what
the parser consists of.

### Layer 1 — named locals

`src/c4th/forth/locals.f`, Forth-2012 Locals syntax:

    : ATOI {: a u -- n :}   0  u 0 ?DO 10 * a I + C@ 48 - + LOOP ;
    : HYPOT2 {: x y | s -- n :}  x x * TO s  s y y * + ;

Three parts and no magic:

1. **Names.** `{:` parses them into a compile-time table. They are *not*
   dictionary entries: c4th's dictionary and its code share one space, so
   a header created mid-definition would land in the middle of the body.
   They are found instead through **`NOTFOUND`** — the outer
   interpreter's one extension point, twenty-eight lines of C
   (`src/c4th/include/outer.h`), most of them comment. A word that is neither defined nor a
   number is offered to a handler before the error is printed, and a true
   flag means it was dealt with. A handler chains by saving the one it
   replaces, so this is an extension point for *any* later syntax, not
   just locals. **It is the only C change the DSL needs.**
2. **The frame.** Locals live on a stack of their own, not on the return
   stack, because c4th's `DO`/`LOOP` keeps its parameters there and `I`
   would read the wrong thing. Each frame stores its parent, so recursion
   works — `FACT` with a local is in the test.
3. **The end.** `;` and `EXIT` are redefined to drop the frame. That is
   safe rather than clever: c4th hides a definition while compiling it,
   so the `;` that ends the *new* `;` finds the old one.

### Layer 2 — the compiler substrate

`src/c4fc/dsl.f`.

**Arena.** `ARENA-INIT`, `ALLOT:`. A compiler is a batch process:
bump-allocate, let go of everything at the end. No free, no GC, no
ownership question ever asked.

**Vectors.** `VEC-INIT V, V@ V! V#`. Token lists, instruction lists,
symbol tables — arrays, not cons cells, which is `c4lc-design.md §12`'s
own proposal.

**Nodes.** A node is a record whose first cell is a tag:

    NODE: n_add   NFIELD: >lhs  NFIELD: >rhs  ;NODE

Field names are shared across kinds **on purpose**: two kinds that both
have `>lhs` at the same offset *should* use the same accessor, and
Forth's redefinition makes that free rather than a collision. Shared
name, shared layout — the invariant is enforced by the thing that would
otherwise be a hazard.

**Generics.** A table of execution tokens indexed by tag, so dispatch is
one indexed fetch:

    GENERIC: EVAL
    :M EVAL n_add  {: n -- v :} n >lhs @ EVAL  n >rhs @ EVAL + ;M

`:M` synthesises a real name (`EVAL/n_add`) so a backtrace says
something. Unset tags hold `NO-METHOD`, which names the tag it could not
handle — a missing method is a message, not a wild call.

### Layer 3 — the C-specific vocabulary

Each of these is a table plus a generic, and each is written when the
phase that needs it is.

| table | a row is | adding a C feature |
|---|---|---|
| `KEYWORD"` | name → token id | one row |
| `OP:` | token, precedence, associativity, node tag | one row |
| `TYPE:` | kind, base, size | one row |
| `DIRECTIVE:` | `#name` → handler | one row |
| `:M GEN` | node tag → rvalue emitter | one method |
| `:M GEN-ADDR` | node tag → lvalue emitter | one method |

Two of those deserve a note.

**Expression parsing is a table, not a chain.** c4's own `expr(lev)` is
precedence climbing; the whole of it becomes a loop over `OP:` rows —
peek the token, look up its row, and if its precedence clears the floor,
consume it and recur. Adding `%=` or `<<` is one row and no code.

**Lvalues are a generic, not a flag.** `GEN` emits a value; `GEN-ADDR`
emits an address. `n_ident`, `n_index`, `n_member` and `n_deref`
implement both; everything else implements only `GEN`. "Is this an
lvalue" stops being a predicate you maintain and becomes "does it have a
`GEN-ADDR` method" — which is the same question, asked where the answer
already lives.

## Worked example: adding `<<=`

Three rows, no edits.

    KEYWORD" <<="  k_shleq                       \ lex.f
    OP: k_shleq  PREC 2  RIGHT  n_shleq  ;OP     \ parse.f
    :M GEN n_shleq  {: n -- :}                   \ gen.f
       n >lhs @ GEN-ADDR  DUP LI,
       n >rhs @ GEN  SHL,  SI, ;M

`src/c4fc/tests/dsl.f` does the same thing for a toy language and the
Makefile checks the result, so the claim is a test rather than a
paragraph.

## The compiler

Same phases as c4lc, same coverage, same output format. c4lc is the
oracle at every step, which is the whole reason to keep the shape.

    src/c4fc/dsl.f       the vocabulary above
    src/c4fc/lex.f       tokens                     (c4lc L0)
    src/c4fc/pp.f        the preprocessor           (c4lc L9)
    src/c4fc/parse.f     declarations, statements, expressions   (L1)
    src/c4fc/types.f     the type algebra           (part of L3)
    src/c4fc/gen.f       code emission              (L2, L3)
    src/c4fc/opt.f       the six c4opt passes       (L4, L6)
    src/c4fc/c4fc.f      the driver, and .c4r out
    src/c4fc/tests/      goldens and differentials

The `.c4r` writer already exists twice — `c4r.f` (B5d) and inside
`self.f` (B5d.5), where it is seventeen lines of code because everything
hard about the format is the patch table and that is built as you go.
c4fc uses `c4r.f`.

## Verification

The bar rises rung by rung and c4lc supplies it at every one.

- **Tokens.** Dump matches `expected/c4lc-tokens.txt`; `-count` on
  `c4cc.c` is 15,023.
- **Preprocessed text.** Byte-identical to `gcc -E` for all twelve C4IX
  modules — the bar `c4lc` already meets.
- **AST.** Dump matches c4lc's for the `src/tests` sweep. The node
  schema gives the printer away for free, which is why the schema
  carries names at all.
- **Code.** `.c4r` **byte-identical to c4lc's** across `C4LC_DIFF`.
  Where a construct makes byte-identity impossible, the fallback is B5's
  bar: behaviour identical under c4m, size no worse.
- **The closing loop.** c4fc compiles `src/c4th/c4th.c`, and the c4th
  that comes out passes the Forth-2012 CORE suite. *The Forth compiles
  the C compiler that compiles the Forth.* It costs nothing to set up —
  both halves are already pinned — and nothing else in the tree would
  catch a subtle codegen bug so thoroughly.

## The ladder

- [x] **F0** This document, before the code.
- [x] **F1** The DSL: `ext.f`, `locals.f`, `dsl.f`, the `NOTFOUND` hook.
      *Verified:* `make test-c4fc`, native and under c4m, including the
      add-a-construct demonstration.
- [x] **F2** Lexer, `src/c4fc/lex.f` — three tables and a loop.
      *Verified:* c4lc's golden dump of the sample, the same again with
      `-conforming` escapes, 15,024 tokens on `c4cc.c`, and a sweep of
      twelve real sources (`c4.c`, `c4m.c`, `c4dos.c`, `cpu.c`, …)
      each compared against freshly generated c4lc output —
      byte-identical on all of them. Incidentally 3x faster than c4lc's
      on `c4cc.c`, 0.30 s against 1.0 s, which is not the point and is
      not claimed anywhere else.

      Two things it shook out.

      **The golden dump is lossy and c4fc reproduces that.** c4sp prints
      a token line as a C string, so `"tab\there\rcr\0nul..."` loses
      everything from the `\0` onward — the rest of the string, the
      line number and the closing paren. c4lc's *token* is intact; only
      its dump is. Reproducing the truncation keeps the oracle exact,
      which is worth more than a prettier transcript: a change in what
      the lexer does stays a diff instead of hiding among a known one.
      The bytes are still checked, by every phase after this one, which
      reads the token rather than the transcript.

      **The count is 15,024, not the 15,023 the plan recorded.** c4lc
      says 15,024 today. The plan's figure was measured before something
      moved and nobody had a reason to look again — which is the same
      thing `docs/c4lc-design.md`'s stale 0.67 s turned out to be.
- [x] **The vertical slice**, taken ahead of F3-F5 on purpose. `hello.c`
      and two programs of straight-line C compiled all the way to a
      `.c4r` and **byte-identical to c4lc's** — `src/c4fc/{ast,emit,gen,
      parse,c4fc}.f`, about 400 lines. The ladder's one real unknown was
      whether byte-identity is reachable at all or whether matching
      c4lc would mean copying incidental decisions rather than making
      them. **It is reachable, and nothing had to be copied** — every
      difference that came up was a rule c4lc follows for a reason.

      Six of those rules, none of them in any document, all found by
      decoding an image rather than reading source:

      1. **c4lc's code stream is ZERO-based.** c4cc emits through `*++e`
         so its images never use word 0; c4lc puts its first instruction
         there. Both are loadable and both conventions live in the tree
         — which means `c4l.c`'s pre-flight walk cannot assume either.
         It now takes a zero first word as the signal, because `LEA` is
         opcode 0 and no function begins with one.
      2. **Version 3, and v2's eight padding bytes hold the data
         segment's MEMSZ** — its size in memory, which is larger than
         the bytes written.
      3. **The data segment is written with trailing zeros trimmed.**
         The loader zero-fills to memsz, so a string's terminating nul
         at the very end of the segment is simply not stored.
      4. **Globals are laid out after every string literal**, so their
         addresses are the last thing known — which is why their patches
         are revisited at the end, and why c4lc's symbol table lists a
         global declared first after every function.
      5. **A patched operand's code word depends on the patch kind**: a
         data reference writes zero there and carries the value in the
         patch, a code reference writes the target. Neither half is
         visible in a small program — the first shows up with two string
         literals, the second with a call to anything but function zero.
      6. **The symbol record's `type` field is the C type**, so
         `char *greeting` is 2 where `int count` is 1. One byte.

      And one that is not about the format at all: **the type system is
      not optional even at this scale.** `char c; c = 65;` is `SC`, not
      `SI`. A third generic, `CT`, answers "what type is this
      expression" in seven one-line methods, and `GEN n_asgn` asks the
      lvalue rather than assuming. That is F5 arriving inside F6, and
      the ladder below is reordered to admit it.

- [ ] **F3** Preprocessor. *Verify:* byte-identical to `gcc -E` on the
      twelve C4IX modules.
- [x] **F4** Control flow, and everything that turned out to come with
      it. `if`/`else`, `while`, `for`, `do`/`while`, `break`,
      `continue`, `?:`, `switch`; the unary operators `! ~ - * &`,
      prefix and postfix `++`/`--`, short-circuit `&&`/`||`, `sizeof`,
      compound statements. *Verified:* `src/c4fc/tests/spike3.c` and
      `spike4.c`, **byte-identical to c4lc**.

      Three things worth writing down.

      **`&&` and `||` are control flow, not arithmetic.** They had been
      rows in the infix table mapping to `AND` and `OR`, which is wrong
      and which nothing had caught because the slice's programs did not
      use them. They are branches around the right operand, so they are
      node kinds with methods — the table is for operators that really
      are one opcode.

      **`continue` in a `for` loop is a FORWARD branch.** It targets the
      step, and the step is emitted after the body. So `break` and
      `continue` are both marks resolved when the loop closes, rather
      than one being a jump to a known address — which also makes the
      three loop kinds share one resolver.

      **`switch` is a jump table in the data segment**, dispatched
      through `JMPA`, with the table's entries as data-to-code patches
      (type -3) written after every code patch because that is the order
      c4lc writes them and `c4r.lisp` walks the two lists together. The
      table is allocated *after* its body is parsed — a string literal
      inside the switch gets the lower address, which is checkable and
      is checked. Entries with no case of their own hold the default
      target, which is the end when there is no `default` at all. And
      the `IMM lo SUB` that turns a value into an index is **omitted
      when the lowest case is zero**: four words, invisible until a
      switch happens to start at `case 0`, and the only difference left
      when everything else matched.
- [x] **F5/F6** Types and the codegen that asks them questions, done
      together because the slice had already shown they interleave.
      Pointer scaling, arrays, structs, enums, `sizeof`, `[]`, `.` and
      `->`. *Verified:* `spike5.c` and `spike6.c`, **byte-identical to
      c4lc**, along with everything before them.

      **A type is one integer, and the encoding is c4lc's** rather than
      one of my own — the `.c4r` symbol record carries it, so
      byte-identity means carrying the same number. `char` is 0, `int`
      is 1, every `*` adds 2, and `struct k` is `1024 + 64k`. Sixty-four
      apart is what leaves room for thirty-one levels of indirection
      before two structs could collide; it was read off three structs in
      one file rather than guessed. `src/c4fc/types.f` is that algebra
      and nothing else: how big is a type, what does a pointer to it
      step by, what is it a pointer to.

      Four things the differential settled that no reading would have.

      **A pointer steps by what it points at, and a step of one emits no
      multiply at all.** `char *p; p + i` is `PSH; ADD` with no `MUL`
      anywhere — not an optimisation, just what c4lc emits, and the same
      rule governs `++`, `--`, `[]` and pointer subtraction. A `struct P
      *` steps by 24.

      **Struct members are cell-sized.** `{ char a; char b; int c; }` is
      twenty-four bytes and `b` is at eight, not one.

      **An aggregate is a name that stands for its own address.** Arrays
      and struct variables both carry `attrs 0x40` in the symbol record,
      and the whole of array decay is that one flag: the `GEN` method
      emits the `LEA` and stops, where a scalar would go on to load.
      A local array of four ints takes slots 1..4 and its name is
      `LEA -4`, because locals grow downwards and an array's name is the
      address of its lowest slot.

      **`x.m` is `(&x)->m`.** There is one member node, not two: the
      parser wraps the base and codegen never asks which spelling it
      came from.

      Left for F7, and left deliberately: array members inside structs,
      initialisers, `static`/`extern`, varargs, and constant expressions
      in `enum` bodies (c4lc's L11) — the parser takes a literal there.
- [x] **F7** The rest of the subset: storage classes, prototypes,
      initialisers, constant expressions, variadic functions, and the
      constructor and destructor lists. *Verified:* `spike7.c` and
      `spike8.c`, **byte-identical to c4lc**, with everything before
      them.

      **The data segment is three regions, in this order:** globals
      *with* an initialiser, then string literals and jump tables, then
      globals without. That is not the obvious layout and it is not
      source order — a global initialised in a file whose first function
      contains a string still comes first. Only region 1's addresses are
      known as they are handed out, so regions 2 and 3 are relative
      until the last declaration has been read and every patch that
      names one is revisited at the end.

      **`...` is not a special form.** It is one more parameter,
      unnamed, and the *call site* does the work: push everything, push
      how many were extra, call `__c4cc_make_va`, drop the count and the
      extras, push what it returned. The callee needs no prologue at
      all — which is exactly why `int vsum(int n, ...)` finds `n` at
      `bp+3` and not `bp+2`.

      The symbol record's attribute word turns out to carry five things,
      each found by compiling a file that used one: `0x1` constructor,
      `0x2` destructor, `0x8` static, `0x20` variadic, `0x40`
      aggregate. Constructors and destructors also put their code index
      in the image's own `c` and `d` lists, which is what makes them
      run.

      **Calls are all forward references now.** A prototype means a call
      can precede the definition, so every call to a user function is
      recorded and fixed when the program has been read — both halves of
      it, because a code reference carries its target in the patch *and*
      in the code word.

      One deliberate divergence, in the direction of accepting more:
      c4lc rejects `sizeof` inside an `enum` body ("enum initializer
      must be an integer constant") and c4fc allows it, because the
      constant evaluator walks the same precedence table the code path
      does and `sizeof` was already on it.
- [ ] **F8** The optimizer. *Verify:* `-O` output byte-identical to
      `c4opt`, the differential B5 already uses.
- [ ] **F9** The closing loop. *Verify:* c4fc compiles `c4th.c`, and the
      result passes the Forth-2012 CORE suite.

## Risks, and one thing deliberately left out

1. **Byte-identity may not survive every construct.** c4lc's temporary
   and label allocation is deterministic but incidental; matching it
   exactly could mean copying decisions rather than making them. The
   fallback bar is stated above, and the moment it is used it goes in
   this document with the construct that forced it.
2. **`c4th`'s image is one arena and c4fc will want a lot of it.** The
   arena is separate (`ALLOCATE`), so this is a flag, not a redesign.
3. **The DSL uses `EVALUATE`, `POSTPONE` and `CREATE/DOES>`**, which
   `self.f` — deliberately a compiler with no compile-time execution —
   cannot compile. So c4fc cannot be compiled to a `.c4r` by self.f, and
   **it does not need to be**: it runs on c4th, and c4th runs everywhere.
   Making c4fc native is a later rung and the route is `native.f`, whose
   strategy (b) backend already compiles c4th definitions; what it needs
   first is multi-word compilation with real calls, which B5d.5 sketched
   as the two-stacks-and-a-bridge and did not build.
4. **Scope creep into real C99.** The target is c4lc's coverage, not the
   standard's. Anything c4lc declines, c4fc declines identically, and
   `docs/c4lc-design.md §5.1`'s deliberate-divergences section is the
   model for saying so.

**Left out on purpose: a grammar DSL.** Parser combinators over C would
be elegant for a language C is not. C's grammar needs a typedef table to
tell a declaration from an expression, and its declarator syntax reads
inside out; c4lc uses precedence climbing plus hand-written recursive
descent for exactly that reason and c4fc keeps it. The table earns its
place for *operators*, where the rows really are uniform. Everywhere else
a table would be a grammar pretending C is regular.
