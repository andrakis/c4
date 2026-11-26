# c4cxx

A translator from a C++ subset to C, whose output c4lc compiles. It exists
for Blood (andrakis/c4-Blood), whose game code is Watcom C++ 10.6 written as
"C with classes". The plan, its subset and milestones are in
[docs/c4cxx-plan.md](../../docs/c4cxx-plan.md).

It is plain C, so gcc builds it on the host (`make c4cxx`) and c4lc can build
it later.

    c4cxx [-E | -check-types] [-x c|c++] [-I dir] [-D name[=value]] [-U name] [-o out] file

- `-E` writes the preprocessed tokens.
- Without `-E` it parses the unit, types it, and writes it out as C.
- `-check-types` adds `_Static_assert`s of what c4cxx worked out: every
  expression's type (and constant's value), every struct's layout, every
  global's size. gcc `-m32` then confirms them.
- A `.cpp`, `.cc` or `.cxx` file is C++ (`__cplusplus`); `-x` says which.

## State: M1 (2026-11-26)

M1 is the C half of the front end: the preprocessor, the lexer, a C parser
(as c4lc takes it) and a C emitter. The C++ comes in M3 to M5.

The test is c4lc itself. For each unit of a corpus, c4lc compiles it as it
is, and compiles c4cxx's output; the two objects must be the same bytes. It
passes for both `-E` and the parse-and-emit path:

| corpus | units | the same |
|---|---|---|
| c4's `src/tests` (those c4lc compiles alone) | 90 | 90 |
| chocolate-blood's C (engine, MACT, audiolib) | 33 | 32 |
| Duke Nukem 3D (engine, game, audiolib, c4 platform) | 34 | 33 |

Two units are counted apart. In both, c4lc's own `-P` disagrees with gcc's
preprocessor, which agrees with c4cxx (`test-same.sh` checks this every
time):

- **Duke's `menues.c`**: c4lc stringizes the argument of
  `assert(stricmp(kind, "*.MAP") == 0)` as `stricmp ( kind , *.MAP ) == 0`,
  losing the string literal's quotes. C keeps them, escaped.
- **chocolate-blood's `opl3.c`**: `OPL3_Generate4Ch` gets two more locals
  under c4lc's `-P`. Its token dump (`c4lc-ppdump.lisp`) agrees with
  c4cxx's, so the difference is in c4lc's compile path. It is not yet
  found.

`src/tests/c4lc_lex_sample.c` is left out. c4lc turns a backslash-newline
inside a string into a newline character, a c4cc quirk; C removes it, as
c4cxx does.

## State: M2 (2026-11-26)

M2 adds C's types (`types.c`) and a semantic pass (`sema.c`):

- **Declarations:** every declaration resolves to a type.
- **Structs:** structs and unions are laid out as c4lc's 32-bit target
  lays them out, which is gcc `-m32`'s layout, including `#pragma pack`,
  `__attribute__((packed))`, `aligned(N)` and bit-fields.
- **Enums:** enumerators get their values.
- **Expressions:** every expression is typed by C's rules: the integer
  promotions, the usual arithmetic conversions, pointer arithmetic, members
  through anonymous members, constants typed by value and suffix, and string
  literals as `char` arrays.
- **Constants:** integer constants are folded for array sizes, bit-field
  widths, enum values and case labels. Arrays without a length take it from
  their initializer, through brace elision.

The oracle is gcc `-m32`: `test-types.sh` compiles `-check-types`'s output,
and every assertion must hold.

| corpus | units gcc confirms |
|---|---|
| c4's `src/tests` (those c4lc and gcc both compile) | 83 of 83 |
| chocolate-blood's C | 33 of 33 |
| Duke Nukem 3D | 34 of 34 |

That is over 150,000 assertions in four of Duke's units alone. The
byte-identical objects of M1 still hold.

c4lc accepts some things gcc does not. sema gives them a fallback type and
does not assert them:

- a pointer in division, or added to a pointer;
- a call through a word holding a function's address;
- `static const int M = ...; int t[M];`, which is a constant to c4lc as
  it is in C++.

## Files

| file | what |
|---|---|
| `c4cxx.h` | tokens, files, the preprocessor's interface, utilities |
| `ast.h` | the syntax tree: `Spec` and declarator chains, statements, expressions |
| `lex.c` | text to preprocessing tokens: line splices (CR LF too), comments, pp-numbers, prefixed literals, C++'s `::` `.*` `->*` |
| `pp.c` | the preprocessor. Macros by Prosser's algorithm (hidesets). Stringize, paste, variadic (named, GNU comma). `#if` with `defined`. Includes found as c4lc finds them. `#pragma` passed through. `__c4_inttypes__` predefined, and `__cplusplus` for C++ |
| `parse.c` | C to the tree, with typedef names tracked by scope; K&R definitions, bit-fields, designated initializers, compound literals, attributes kept as their tokens |
| `types.h`, `types.c` | C's types, their layout (gcc -m32's), conversions, and their names as C |
| `sema.c` | scopes, tags, `#pragma pack`, declarations to types, expression types, constant folding |
| `test-types.sh` | the gcc oracle for types (`run-corpora.sh -T`) |
| `emit.c` | the tree to C: declarators from their chains, expressions with the parentheses precedence needs, `# N "file"` markers so that c4lc's `-g` lines are the source's |
| `main.c` | the driver |
| `test-same.sh`, `run-corpora.sh` | the c4lc oracle and its corpora (`make test-c4cxx`) |
| `c4odiff.py` | which functions and data differ between two objects |
| `tests/` | the preprocessor's cases, with their output (checked against gcc -E) |
