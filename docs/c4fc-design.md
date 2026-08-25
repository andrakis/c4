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
- [ ] **F2** Lexer. *Verify:* token dump == c4lc's; 15,023 on `c4cc.c`.
- [ ] **F3** Preprocessor. *Verify:* byte-identical to `gcc -E` on the
      twelve C4IX modules.
- [ ] **F4** Parser. *Verify:* AST dump == c4lc's over `src/tests/*.c`.
- [ ] **F5** Types. *Verify:* `sizeof`, pointer scaling and struct
      offsets agree with c4lc on a dedicated corpus.
- [ ] **F6** Minimal codegen. *Verify:* `hello`, `factorial`, `puts`
      byte-identical to c4lc's.
- [ ] **F7** Full subset: arrays, structs, `switch`, varargs, statics.
      *Verify:* the whole `C4LC_DIFF` corpus, byte-identical.
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
