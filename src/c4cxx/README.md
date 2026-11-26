# c4cxx

A translator from a C++ subset to C, whose output c4lc compiles. It exists
for Blood (andrakis/c4-Blood), whose game code is Watcom C++ 10.6 written as
"C with classes". The plan, its subset and milestones are in
[docs/c4cxx-plan.md](../../docs/c4cxx-plan.md).

It is plain C, so gcc builds it on the host (`make c4cxx`) and c4lc can build
it later.

    c4cxx [-E | -parse | -check-types] [-x c|c++] [-I dir] [-D name[=value]] [-U name] [-o out] file

- `-E` writes the preprocessed tokens.
- Without `-E` it parses the unit, types it, and writes it out as C (C++
  lowered to C).
- `-parse` stops after parsing.
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

## State: M3 (2026-11-27)

M3 adds classes without inheritance or `virtual`, lowered to C:

- **Classes** become C structs of their data members. A class or enum tag
  is a type name, printed as `struct X`.
- **Member functions** become `X__name (struct X *this, ...)`, declared
  after the struct. Inside them, a member's name is `this->name`. A call
  `obj.f (a)` is `X__f (&obj, a)`, and `p->f (a)` is `X__f (p, a)`. Those
  defined in the class body are `static` in each unit that sees the class.
- **Constructors** are `X__C`, which return `this`. A constructor builds the
  members in declaration order, from its init list or by their own
  constructors, then runs its body. **Destructors** are `X__D`: the body,
  then the members' destructors, last first. A class whose members need
  either gets one made.
- **Objects:** a constructor is called after the declaration (`X x (1)`,
  `X x = 5`). The destructors run at the end of the block, and before a
  `return`, `break` or `continue` that leaves it. A returned value is kept
  in `__c4x_r` first.
- **Globals** with constructors, and globals initialized by what C cannot
  initialize them with (`int Mouse::X = Mouse::rangeX >> 1`), are built in
  the unit's `static void __attribute__ ((constructor)) __c4x_init_<unit>`.
  Their destructors run in `__c4x_fini_<unit>`. These use c4's `.c4r`
  constructor and destructor lists (plan section 3.6).
- **Static members** are globals named `X__name`.
- **References** are pointers. Binding takes the address, and each use
  dereferences. A `const T&` bound to an rvalue gets `(T [1]) { v }`, an
  array of one that decays to the pointer. c4lc takes no scalar compound
  literal.
- **Default arguments** are filled in at each call.
- **`bool`** is `_Bool`, and `true`/`false` are 1 and 0.
- **A `const` at file scope** is `static`, as C++ makes it. Where C wants a
  constant (array sizes, case labels, enumerators, bit-field widths,
  initializers), its uses print as its value.
- **Anonymous unions and structs** in a class get names (`_u0`), and member
  access goes through them. c4lc has none.
- **Nested types and typedefs** go before the class, at file scope, under
  their own names. `X::Inner` reads as `Inner`.
- **`extern "C"`** is read in both forms, block and single declaration.

Names carry no parameter codes yet. M4 adds them (plan section 3.4).

Refused with a message, for M4 and M5:

- overloaded functions and members, member operators, and a class's
  temporaries `X (...)` (M4);
- base classes, `virtual`, `new` and `delete` (M5).

Refused with a message, as the plan's subset says:

- a `goto` where objects with destructors are in scope;
- a reference (not const) to an rvalue, and a const reference to a class
  rvalue;
- static locals with constructors or run-time initializers;
- arrays of objects with constructors or destructors, and objects with
  destructors declared in a `for`;
- local classes with member functions;
- templates, namespaces, `using`, exceptions, and the `*_cast`s.

The tests:

- **Programs:** `test-cxx.sh` runs each `tests/t_class*.cpp` three ways.
  g++ `-m32` builds the reference. c4cxx's C is then built by gcc `-m32`,
  and by c4lc to run on `c4m32`. All three must print the same, and gcc
  must confirm the types of the C (`-check-types`). The programs are a
  `Rect`, a stack, constructors and destructors writing to a log at
  `return`/`break`/`continue`, references swapping, `const Error&`, a
  reference return, anonymous unions, nested types, globals with
  constructors and run-time initializers. 4 of 4 pass, and the 5
  `tests/e_*.cpp` are refused with the message each expects.
- **Blood:** `blood-units.sh -parse`: all 80 of Blood's C++ units parse
  (Blood-RE's game and qtools, as c4-Blood's `host.sh` builds them). Blood's
  headers parse as C++ declarations, M2's deferred test.
- **The C corpora:** M1's byte-identical objects and M2's gcc-confirmed
  types still hold.

All of it is in `make test-c4cxx`.

Blood's units translated in full, 7 of 80 (qtools' getopt, helix, iob,
misc, pqueue, qheap and qmouse), compile with both gcc and c4lc. The rest
stop at what M4 and M5 bring:

- **Inheritance (M5):** nearly all of them, at one header, qtools'
  `resource.h` (`struct DICTNODE : CACHENODE`).
- **Overloading (M4):** overloaded functions (`credits.h`, debug4g, key,
  textio) and members (`inifile.h`, `gfx.h`), and a temporary
  `Error (...)` (error.cpp).

## Files

| file | what |
|---|---|
| `c4cxx.h` | tokens, files, the preprocessor's interface, utilities |
| `ast.h` | the syntax tree: `Spec` and declarator chains, statements, expressions |
| `lex.c` | text to preprocessing tokens: line splices (CR LF too), comments, pp-numbers, prefixed literals, C++'s `::` `.*` `->*` |
| `pp.c` | the preprocessor. Macros by Prosser's algorithm (hidesets). Stringize, paste, variadic (named, GNU comma). `#if` with `defined`. Includes found as c4lc finds them. `#pragma` passed through. `__c4_inttypes__` predefined, and `__cplusplus` for C++ |
| `parse.c` | C to the tree, with typedef names tracked by scope; K&R definitions, bit-fields, designated initializers, compound literals, attributes kept as their tokens. C++: class bodies, qualified names, constructors and init lists, references, default arguments, `X x (args)`, `extern "C"`, `new`/`delete`, functional casts |
| `types.h`, `types.c` | C's types, their layout (gcc -m32's), conversions, and their names as C |
| `sema.c` | scopes, tags, `#pragma pack`, declarations to types, expression types, constant folding. C++ (M3): classes and their scopes, member calls, constructors, destructors and their cleanups, references, default arguments, a unit's initializer |
| `test-types.sh` | the gcc oracle for types (`run-corpora.sh -T`) |
| `emit.c` | the tree to C: declarators from their chains, expressions with the parentheses precedence needs, `# N "file"` markers so that c4lc's `-g` lines are the source's |
| `main.c` | the driver |
| `test-same.sh`, `run-corpora.sh` | the c4lc oracle and its corpora (`make test-c4cxx`) |
| `c4odiff.py` | which functions and data differ between two objects |
| `test-cxx.sh` | C++ programs against g++ (gcc and c4lc), and what c4cxx refuses |
| `blood-units.sh` | Blood's C++ units through c4cxx (`-parse` by default) |
| `tests/` | the preprocessor's cases, with their output (checked against gcc -E); `t_*.cpp` programs and `e_*.cpp` refusals for `test-cxx.sh` |
