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

- [x] **F3** Preprocessor. `#include` (both forms, searched along `-I`),
      `#define` object- and function-like, `#undef`, `#ifdef`/`#ifndef`/
      `#if`/`#elif`/`#else`/`#endif` with constant expressions, `#`
      stringize, `##` paste, line continuation, `-D`, and gcc's
      `# 12 "file"` markers consumed so an already-preprocessed source
      still works. *Verified:* three bars, in `make test-c4fc`.

      **The token stream is a stack.** c4lc splices an expansion onto
      the front of a cons list and walks the result; here the pending
      tokens live in a vector used as a stack, and "push it back so it
      is rescanned" is one `V,`. `#include` is the same operation with
      a whole file's tokens, which is why an include costs no
      recursion and no second walk.

      **It works on tokens, and there is still only one lexer.**
      `PPMODE` changes four things -- `#` and `##` become tokens, a
      `<header>` after `include` is one `Str`, an identifier records
      whether `(` TOUCHES it, and a newline clears the header scan --
      and changes nothing else. That last bit is the whole of what
      separates `#define ADD(a,b)` from `#define TWO (x + y)`, and the
      standard draws the line at the space, so the lexer has to be the
      one to see it.

      **A token now carries the serial of the buffer it came from.** A
      directive runs to the end of its LINE, and after an `#include`
      two files' line numbers sit next to each other on one stream;
      c4lc splices token lists and has the same exposure. One extra
      field closes it.

      Four deliberate divergences from c4lc, all toward what C says
      and all invisible on this tree's sources: a conditional level
      records whether a branch has been TAKEN, so `#if 1 / #elif 1 /
      #else` does not run the `#else` arm; `defined(X)` protects `X`
      from expansion even when `X` is a macro (c4lc expands it first
      and then asks whether the *result* is defined); a macro is
      painted blue while its own expansion is rescanned, so a
      self-referential `#define` terminates instead of looping; and a
      quoted `#include` resolves against **the including file's
      directory** before the `-I` list, rather than against the
      current one -- which is the difference between `include/c4.h`
      and the `c4.h` that is not in the root, and the only divergence
      any real file in this tree noticed.

      *Verified*, weakest bar first:
      - `src/tests/c4lc_pp.c`, the battery c4lc's own L9 test uses,
        **token for token identical to c4lc's preprocessor**. (That
        needed a one-line fix to `c4lc-ppdump.lisp`, which had never
        been wired into a test and did not load the file `cons` lives
        in, so it died on the first `#elif`.)
      - **Twenty-one real sources preprocessed by c4fc against the
        same sources preprocessed by `gcc -E`**, identical token for
        token -- the twelve C4IX modules the design named, plus
        libc4ix, a userland program, `c4or1k/cpu.c`, `c4mp/vm.c`,
        `c4cc.c`, `c4ke.c`, `c4th.c`, `c4sp.c` and `load-c4r.c`.
        Tokens and not text, because gcc emits `# 12 "file"` markers
        and c4fc consumes them, so the two can never agree on a line
        number and must agree on everything else.
      - `src/c4fc/tests/spike9.c`, a program that *uses* the
        preprocessor, compiled to an image **byte-identical to
        `c4lc -P`'s** -- and then run, because two compilers agreeing
        on a wrong image is not a passing test.

      `-P` is c4lc's flag and, like c4lc, it is off by default: without
      it the lexer skips `#` lines, which is what a source that has
      already been through `gcc -E` needs. Keeping the default the same
      as the oracle's is what lets every F2-F8 differential stay a
      straight byte comparison.

      Two things neither compiler models, recorded so nobody hunts
      them: a `#define` whose name is a **keyword** (`c4dos.c` has
      `#define int long long`, and is built by gcc's cpp and c4cc for
      that reason), and system headers -- `#include_next` and
      `/usr/include` are not on the map for either.
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
- [x] **F8** The optimizer: `fold`, `shl`, `adj0`, `jmpnext`, `thread`,
      `tail` and `dead`, run to a fixpoint in c4opt's order.
      *Verified:* `c4fc -O` against the Lisp `c4opt` applied to c4fc's
      own unoptimised output, on all nine programs, and the optimised
      image does exactly what the unoptimised one did.

      **The passes cannot work on a finished image** — deleting one
      instruction moves every address after it — so `src/c4fc/opt.f`
      decodes the image into the labelled form c4opt works on, optimises
      that, and assembles it again. Decoding has to be c4r.lisp's
      reconstruction exactly, because a different set of labels is a
      different program shape: a code offset becomes a label if the
      entry, a `-1` patch, a `-3` patch (a switch table entry naming
      code from data), a constructor, a destructor or a defined
      function's symbol points at it. **Round-tripping with no passes
      reproduces the image byte for byte**, and that was checked before
      any pass was trusted.

      **Byte-identity with c4opt turned out to be the wrong bar, and
      finding out why took the longest.** c4opt leaves the
      PRE-optimisation address in the code word of a patched operand and
      puts the correct one in the patch; the loader overwrites the word,
      so the image runs correctly and the stale word is never executed.
      Two images can therefore be identical in every way that runs and
      still differ byte for byte — the patch lists were identical and
      only the operand words differed. `src/c4fc/tests/imgcmp.f` applies
      the code-resident patches to both images and compares the rest,
      which touches only words the loader overwrites anyway, so a
      difference in the patches, the data, the symbols or an instruction
      still shows. c4fc writes the post-optimisation address in both
      places; reproducing the staleness to win a `cmp` would have been
      copying a defect, which §"Risks" said the fallback exists for.
- [x] **F9** The closing loop. *Verified:* c4fc preprocesses and compiles
      `src/c4th/c4th.c` -- thirteen headers, thirty thousand tokens --
      into an image **byte-identical to c4lc's**, and the c4th that comes
      out passes the Forth-2012 CORE suite with a transcript identical
      to the pinned golden. *The Forth compiles the C compiler that
      compiles the Forth.*

      **The loop is a test that writes its own bug list.** Every rung
      before this one was verified against programs written to exercise
      the rung. c4th.c was written to be a Forth, and it named eight
      constructs the spikes had never used, in the order it met them:

      - **`int *is, *id;`** -- stars belong to the DECLARATOR, not to
        the base type. Parsing them with the type made the second
        declarator inherit the first's, so `int *a, b` declared two
        pointers. The same bug was in struct members and in globals.
      - **`return;`** with no value, which is what a `void` function
        does and is a `LEV` with nothing computed before it.
      - **the comma operator**, because `va_arg` is written with one,
        which means the whole of `stdarg.h` needs it.
      - **`(char *)p` as a type, not as nothing.** A cast emits no code
        and changes everything: it is what decides `LC` against `LI`
        and whether `p[i]` scales by one or by eight. Skipping the type
        was the F5 lesson arriving a second time.
      - **calling through a pointer** -- `JSRI` through a global,
        `JSRS` through a frame slot. What a call compiles to is decided
        entirely by the CLASS of the name being called, which is why
        those constants moved out of the parser and into `ast.f`.
      - **a function's name as a value**, which is `IMM <code address>`
        and not the `LEA` a variable would get. A threaded Forth's
        dictionary is a table of those, so there were 156 of them.
      - **local initialisers**, which are CODE: they run every time the
        block is entered, which is the whole difference between a local
        and a global.
      - **`char *s = "...";`** at file scope, where the string's bytes
        are laid down BEFORE the pointer's own word, because c4lc lays
        the data out in one pass over the declarations and meets the
        string while it is meeting that global.

      And `sizeof(name)`, which is answered from the symbol's byte size
      and so needed initialised arrays to record one.

      Along the way the preprocessor lost a wart. C recognises keywords
      in a phase AFTER macro expansion, which is why `#define int long`
      is legal and `#ifndef int` asks about a macro rather than about a
      type. So PPMODE now lexes every word as an `Id` and `pp.f`
      classifies them at the end -- which deleted the special case that
      mapped `If` and `Else` back to text for `#if` and `#else`, and
      made `src/tests/c4_jailbreak.c` compile through c4fc's own
      preprocessor rather than only through gcc's.

      The differential that found all of this is now the test:
      **nineteen whole programs preprocessed AND compiled by c4fc,
      byte-identical to c4lc handed the same source through `gcc -E`**
      -- `tests.c`, `global.c` and `c4lc_l2.c` among them.

      One number, since the premise that started this was speed:
      **c4fc compiles `c4th.c` in 0.5 s where c4lc takes 2.6 s**, both
      including their own preprocessing. That is not why to do this and
      it does not change the Track A case -- `gcc -O2` remains the
      ceiling -- but a compiler inside the machine that is not slower
      than the one outside it is worth writing down.

- [x] **L6** The tree passes, and `-O` matching `c4lc -O` byte for byte.
      `src/c4fc/tree.f`. *Verified:* nineteen programs, `c4th.c`, C4KE
      and C4DOS, each compiled with `-O` and **byte-identical to
      `c4lc -O`**; C4KE also boots and shuts down cleanly, and the `-O`
      c4th passes the Forth-2012 CORE suite.

      **T1, constant folding.** One `FOLD` method per node kind, sharing
      `FOLD1` with the peephole pass so the two cannot disagree about
      what `3 / 0` or `-1 >> 2` means. `&&` and `||` fold with c4's
      exact result semantics -- `a && b` is b's VALUE when a is truthy,
      not 1 -- and `if`, `while` and `for` with constant conditions lose
      the arm that never runs.

      **T2, dead function elimination**, which needs the whole program
      and c4fc generates each function as it parses it. So `-O` compiles
      the unit **twice**: the first pass exists only to build the call
      graph and is thrown away, the second skips every function nothing
      live can reach -- at the TOKEN level, so a dead function costs no
      symbol, no code and no string. Parsing twice is cheap here in a
      way it would not be in the Lisp: the first pass of C4KE costs
      0.4 s.

      Three things fell out of it, each a bug the differential named:

      **A string literal must not be allocated until it is emitted.**
      c4fc handed out data offsets while parsing; c4lc hands them out
      during code generation, which is after the tree passes. A message
      inside `if (0)` was still in c4fc's image, 94 bytes that nothing
      referenced. The literal now carries its bytes and gets its offset
      in `GEN`. The switch jump table moved for the same reason and to
      the same place.

      **`JSRI` and `JSRS` take an operand**, and the peephole decoder's
      list said only LEA..ADJ did. The operand word round-trips as if it
      were an instruction, so nothing looked wrong -- the PATCH on it was
      silently dropped, and a call through a function pointer went to
      whatever lives at address zero. c4fc had emitted neither opcode
      until F9.

      **A trailing `LEV` is needed when a LABEL lands at the end of a
      function.** `int f (int x) { if (x) return 1; }` ends with a LEV
      that only the taken arm reaches; the false arm branches past it
      and off the end of the function into whatever was compiled next.
      Judging by "the last thing emitted was a LEV" is wrong, and c4lc
      is right by accident of representation: its labels sit in the
      instruction list and break the chain. c4fc now tracks the highest
      address anything has branched to.

      One number: `-O` takes `c4th.c4r` from 217,298 bytes to 177,940,
      the same −18.1% `docs/c4lc-design.md` records for c4lc's own `-O`,
      and the resulting c4th runs c4fc **1.26x faster** under c4m.

- [x] **F10** Object mode, and C4IX. `-c` compiles one unit to a `.c4o`:
      what it cannot resolve it NAMES, and c4rlink resolves it later.
      *Verified:* **all twelve C4IX modules, each byte-identical to
      `c4lc -O -c`**, linked by c4rlink into a kernel byte-identical to
      the committed `c4ix.c4r`, which boots and shuts down cleanly.

      An unresolved reference is an opcode whose operand word is zero
      and whose patch carries the **symbol id in the type field**, where
      a whole-program image carries -1 or -2. The id is not known until
      every defined symbol has been numbered, so the patch is written
      with the placeholder type `-1000-k` and rewritten at the end --
      out of range of every real type, which is what lets the peephole
      passes carry it through untouched.

      The extern ids follow c4lc's order and c4lc's order is two walks,
      not one: every prototype first, then every extern datum. Deciding
      which prototype needs one means knowing whether the unit defines
      it further down, which is what the discovery pass already knows,
      so `-c` runs the same pass `-O` does.

      Five more gaps, each named by a real module:

      - **`__c4cc_make_va` from a PROTOTYPE.** Every C4IX module
        declares it and none defines it, and c4fc only recorded it at a
        definition -- so a variadic call in `boot.c` called through a
        null symbol and segfaulted. c4lc says "include stdarg.h"; c4fc
        now says the same rather than crashing.
      - **The variadic call path called `JSRF,` directly** instead of
        going through the class dispatch, so a variadic function
        declared in another unit was called as though it were defined
        in this one. Both paths are now one word, `CALL,`.
      - **Declarations in a NESTED block.** `COMPOUND` was "{ ... }
        with no declarations in it", which is not a thing C has. Every
        block takes them now, and the frame only ever grows -- which is
        also what c4lc does.
      - **A call to a function defined later with no prototype
        anywhere.** c4lc registers every defined function in a pre-pass;
        c4fc resolves names as it parses. The discovery pass supplies
        the same list, so `PREREGISTER` puts them in before the second
        pass reads a line. The discovery pass itself has to tolerate the
        unknown name -- it is the thing being discovered.
      - **`extern int x;` allocates nothing**, whether or not it becomes
        an extern symbol: if the unit defines `x` further down, that
        definition allocates it, at its own position. Allocating at the
        declaration put `sched_switches` first in the BSS instead of
        last.

- [x] **F11** `-mcisc` and `-mfuse`, the opcodes c4m does not have.
      *Verified:* every spike byte-identical to `c4lc` under `-mfuse`,
      `-mcisc -O` and both together; three c4or1k modules byte-identical
      as `-mcisc -O -c` objects, which is the build that actually uses
      it; and `tests.c` compiled with both **runs under c4mp** while c4m
      and plain c4 NAME the opcode they lack rather than execute rubbish.

      `-mcisc` is a codegen choice: `LXI`/`SXI` fold scale-add-load (and
      -store) into one opcode for `var[expr]` whose base is a plain
      variable of statically known 8-byte scalar element type. The
      conservatism is copied deliberately -- a base more complex than a
      bare variable falls back, because establishing its type there would
      mean evaluating it twice or duplicating inference codegen already
      does as a side effect.

      `-mfuse` is a peephole pass that runs ONCE after the fixpoint, so
      no other pass has to understand the fused forms and a fusion can
      never hide a fold from the round that would have followed. Longest
      window first, and the fused instruction keeps the operand AND the
      operand kind of the one it starts with -- an `IMM` of a data
      address fuses to a `PSHG` of the same address and the patch
      survives.

      It also found a latent limit that had nothing to do with either
      flag: **a block's statement array was a fixed 256 cells with
      nothing checking the count**, and `virtio9p.c` has a switch with
      twenty-five arms. Statements are gathered in a growable vector now
      and copied to the arena at their real size. `case` outside its
      switch's range is a named error rather than a wild store, too.

## What it costs to run

Measured on a 6-core box, `/usr/bin/time -v`, one process at a time.
Both compilers preprocess the source themselves except where noted.

**c4lc's memory is a knob, and the repo has it set wrong.** `c4sp -c N`
preallocates the cell pool -- roughly 32 bytes a cell -- and touches all
of it, so the number in the Makefile IS the resident set. The values
there are far above what the work needs, and the excess costs time as
well as memory: page faults on a pool nothing uses.

    C4IX, all twelve modules, -O -c, serial
      c4lc  -c 16000000  (as the Makefile builds it)   517 MB   6.65 s
      c4lc  -c   500000  (smallest that completes)      18 MB   3.40 s
      c4fc                                              23 MB   3.00 s

    C4KE, one unit, -O   (c4lc through gcc -E; c4fc alone)
      c4lc  -c 32000000  (as the Makefile builds it)  1010 MB   4.55 s
      c4lc  -c  1000000                                 34 MB   8.84 s
      c4fc                                              29 MB   5.30 s

    C4DOS, one unit, -O
      c4lc  -c 16000000                                506 MB   1.17 s
      c4lc  -c   250000                                 10 MB   1.46 s
      c4fc                                              23 MB   0.96 s

    ... and without -O, where c4fc pays for neither the second pass nor
    the peephole fixpoint:
      C4KE    c4lc -c 1000000   34 MB  3.52 s      c4fc  16 MB  0.87 s
      C4DOS   c4lc -c  250000   10 MB  0.49 s      c4fc  12 MB  0.18 s

c4fc's footprint is a constant, not a knob: an arena, a code buffer and
a patch buffer, all sized once and mostly untouched. Nothing has to be
guessed and nothing has to be tuned per input, which is the difference
that matters on a machine that has to fit the compiler as well as the
program.

**`c4lc` cannot preprocess C4KE at all** -- a function-like macro named
without an argument list is an error there and stands for itself here,
which is what C says -- so the C4KE row above has gcc in c4lc's pipeline
and nothing in c4fc's.

**The compiler compiled by itself.** `c4fc -O` takes `c4th.c4r` from
217,298 bytes to 177,940 (-18.1%, the same figure `c4lc -O` gets on
`c4sp.c`), both images pass the Forth-2012 CORE suite, and c4fc hosted
on the optimised one runs **1.25x faster** under c4m (2.00 s against
2.50 s compiling `spike6.c`). c4fc's own unoptimised image is exactly as
fast as the `c4cc`-built `c4th.c4r` it replaces. All three hosts -- and
native c4th -- produce the same image byte for byte.

## Fitting on a small machine

c4fc's memory was a set of guesses, and the biggest one was larger than
the whole of c4bb. Every buffer is now either **grown on demand** or
**sized from the program**, and the difference is not subtle:

    c4fc -O, peak RSS (64-bit; halve it for a 32-bit board)
      hello.c    ~30 MB  ->  11 MB
      c4th.c        -    ->  15 MB
      C4KE       ~38 MB  ->  20 MB

The arena is a **list of blocks**, because a bump allocator that never
frees can simply start another one — nothing moves, so every pointer
already handed out stays good. The code, patch, data and symbol-section
buffers double where their bounds check used to abort. The optimizer's
item buffers are **sized from the code length**, which is known exactly
by the time it runs: the old fixed quarter-million entries were 33 MB of
item buffers for a hello world.

Three things that fell out of doing it:

- **`D-ALLOT` and `ID-ALLOT` had no bounds check at all.** They bumped
  a cursor past the end of the block and wrote there. Growth is the
  check, and the data buffers grow ZEROED because a partially
  initialised array leaves the rest of itself to be read as zeros.
- **The symbol section was allocated at five cells a record and indexed
  at twelve.** Harmless while symbol counts stayed under a third of the
  capacity, which is exactly the kind of bug that waits.
- **The compile-time symbol table is the one buffer that may NOT move**,
  because it is the one that is pointed AT: an `n_var` node holds the
  symbol itself, and so do the forward-reference list, `VA-MAKE` and the
  switch record. It stays fixed and generous — eight thousand entries is
  under half a megabyte — and its abort stays an abort.

## On the breadboard

c4fc runs on c4th; c4th runs on c4m; c4bb is c4m in hardware. So the
compiler runs on the breadboard, and on 2026-08-26 it did:

    node src/c4bb/sim/cli.js -m 96 -d disk c4th32.c4r <the .f files> \
         -e ': GO 1 OPTIMIZE ! -c C4FC-INIT ... S" vfs.c" C4FC ; GO'

    a real C4IX module, -O -c, on the board:   5m29s
    the object c4lc -O -c writes at 32 bits:   byte-identical

`make c4th32.c4r` builds the 32-bit c4th (c4lc under c4sp32, the route
`c4ke32.c4r` takes); `make test-c4th-bb` is the cheap half of the check.
The five minutes are not a routine test.

**Two 64-bit assumptions had to come out first, and only a 32-bit
machine could have found either.**

- **The `.c4r` memsz field is eight bytes, not one word.** v3 puts the
  data segment's memory size in the eight padding bytes v2 left after
  the wordbits byte, and the field is eight bytes whatever the word size
  is. c4fc wrote one word and stopped — which at 64 bits IS the field,
  and at 32 bits left the header four bytes short, so every section
  marker after it was misread.
- **The `shl` pass was 64-bit only and did not say so.** c4opt's rule
  fires only on a multiply by 8 and emits a shift by 3, so at 32 bits —
  where a subscript scales by 4 — it does not fire at all. c4fc matched
  on `1 CELLS`, the *host's* word size, and still emitted the hardcoded
  3: every `p[i]` on the board became a multiply by eight. It now has
  c4opt's rule exactly, because byte-identity with c4lc is the bar at
  both word sizes.

**Where the time goes, and what would move it.** Nineteen of the 5m29s
is loading the compiler — 3,200 lines of Forth read and compiled through
the threaded interpreter, before a line of C is seen. Two things would
change the shape: `native.f` compiles hot Forth words to real C4 code
and is not switched on here, and c4th can save an image, which would
make the load one-time. Neither is done.

## Risks, and one thing deliberately left out

1. **Byte-identity may not survive every construct.** It survived all
   of F2 to F7 without a single decision having to be copied. It did NOT
   survive F8, and the reason is recorded there: c4opt leaves a stale
   address in a patched operand's code word, and the fallback bar
   ("identical once loaded") is what F8 uses.
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
