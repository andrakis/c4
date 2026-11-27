# c4cxx

A translator from a C++ subset to C, whose output c4lc compiles. It exists
for Blood (andrakis/c4-Blood), whose game code is Watcom C++ 10.6 written as
"C with classes". The plan, its subset and milestones are in
[docs/c4cxx-plan.md](../../docs/c4cxx-plan.md).

It is plain C, so gcc builds it on the host (`make c4cxx`) and c4lc can build
it later.

    c4cxx [-E | -parse | -check-types] [-x c|c++] [-I dir] [-isystem dir] [-D name[=value]] [-U name] [-o out] file

- `-E` writes the preprocessed tokens.
- Without `-E` it parses the unit, types it, and writes it out as C (C++
  lowered to C).
- `-parse` stops after parsing.
- `-isystem dir` is `-I` for headers of C: in C++, their functions keep
  their names in C.
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

## State: M4 (2026-11-27)

M4 adds overloading:

- **Mangling** (plan section 3.4): every function of C++ linkage is named
  in C by its parameters' codes, so that each unit names it the same:
  `FindSector__iiiPi`, `Rect__C__iiii`, `Rect__op_andeq__R4Rect`,
  `CMenuTextMgr__GetFontInfo__iPcPiPi`. Destructors are `X__D`. `extern
  "C"` functions and `main` keep their names, as do the functions of
  headers found through `-isystem` (c4's libc, which has no `extern "C"`
  of its own), as an old C++ compiler took a system's headers.
- **Overload resolution** (plan section 3.3), for free functions, member
  functions and constructors. Each argument ranks as one of:
  - exact, with exact-but-`const`-added below it (`int&` beats
    `const int&`, `char*` beats `const char*`);
  - a string literal as `char *`;
  - a promotion;
  - a conversion;
  - `...`.

  Default arguments count. The best candidate must be at least as good on
  every argument and better on one; otherwise the call is an error that
  lists the candidates. Enumerators have their enum's type, so `evPost`
  on `COMMAND_ID` and on `CALLBACK_ID` are told apart. A string literal is
  `const char [N]`. An enum promotes to `int`, as C++ promotes it.
- **Constructors**, overloaded, are chosen by their arguments: in
  declarations, init lists, `X x = v` (a user's copy constructor
  included) and temporaries.
- **Member operators**: `a[i]`, `!a`, `a &= b`, `a == b`, `++a`, `a++`
  (`operator++ (int)`) and the rest call `X__op_idx`, `X__op_not`,
  `X__op_andeq`... when the operand's class has the operator. A reference
  result is the object (`gDudeInfo[11].x = 1`).
- **Temporaries** `X (args)` of a class without a destructor are built in
  a hidden local of the function, `*X__C__... (&__c4x_t0, args)`, which is
  an lvalue, so that a `const X&` takes its address (`curHandler (Error
  (module, line, buffer))`).
- **`inline`** functions are `static`, because C99's `inline` alone
  defines nothing outside the unit.

The tests (`test-cxx.sh`):

- **`t_over1.cpp`**: Blood's overloads, `evPost` on two enums,
  `sndStartSample (char *, ...)` and `(ulong, ...)` with default
  arguments, `FindSector` with 3 and 4 arguments, `LevelWarp ()`. With
  them, a set of `kind (...)` overloads, one call per rank, and overloaded
  and static members.
- **`t_over2.cpp`**: qtools' `Rect`, with three constructors (a copy
  constructor among them), `operator!` and `operator&=`. Also an
  `operator[]` returning a reference, qtools' `Error` as a temporary for
  a handler called through a pointer, and `operator++` before and after.
- **Refusals**: `e_ambiguous.cpp` and `e_nofit.cpp` must be refused, with
  the candidates listed.

All 6 programs print as g++ `-m32`'s do, through gcc and through c4lc, and
gcc confirms their types.

Blood: 13 of the 80 units now translate in full (qtools: debug4g, error,
getopt, gfx, helix, inifile, iob, key, misc, pqueue, qheap, qmouse,
textio), and each compiles with gcc and c4lc. The other 67 all stop at one
place, qtools' `resource.h`, whose `DICTNODE` derives from `CACHENODE`.
That is inheritance, M5.

## State: M5 (2026-11-27)

M5 adds inheritance, virtual functions, `new` and `delete`, and the
globals Blood has:

- **Inheritance** (plan section 3.2): one public base, which in C is the
  class's first member, `_b`: `struct TextButton { struct Button _b; ...
  }`. A base's member `p->x` is `p->_b._b.x`. A pointer to a derived class
  converts to its base's by a cast at offset 0, in arguments,
  assignments, initializers, returns and references, and overload
  resolution ranks that as a conversion. A base's members are the derived
  class's, unless its own of the same name hide them. `Base::f ()` calls
  the base's function, never through the vtable.
- **Constructors and destructors** in C++'s order: the base (the init
  list's constructor, or its default one), `_vp`, the members, the body.
  A destructor sets `_vp` back, runs the body, destroys the members last
  first, then the base.
- **Virtual functions:** the polymorphic root's first member is `const
  struct X__vt *_vp`. `struct X__vt` has a function pointer per slot, the
  base's slots first, each typed with `this` of the class that introduced
  it. An override is cast to its slot's type in the vtable. A call of a
  virtual function, unless qualified, goes through the vtable:
  `((const struct Panel__vt *) p->_b._b._vp)->Paint__iic ((struct Widget
  *) p, x, y, c)`. An object expression that is not simple goes into a
  hidden local first. A virtual destructor takes the slot `_dtor`. A pure
  virtual's slot holds `__c4x_pure`. Virtual calls from constructors and
  destructors reach the class's own functions, as in C++.
- **Where the vtable goes** (Itanium's key-function rule): the unit that
  defines the class's first virtual function that is neither pure nor
  inline defines `X__vtbl`, and every other unit declares it `extern`. A
  class without one (Blood's `Widget`, `HeadWidget`, and the savers with
  all their functions in the class) has a `static` copy in each unit.
- **`new` and `delete`** (plan section 3.7), on the runtime
  `rt/libc4cxx.c`, which is C for c4lc and gcc:
  - `new T (args)` is `T__C__codes ((struct T *) __c4x_new (sizeof
    (struct T)), args)`.
  - `new T[n]` of a class with a constructor or destructor builds each
    element in `__c4x_new_vec`. When the class has a destructor, it keeps
    the count before the elements.
  - `delete p` runs the destructor, through the vtable when the
    destructor is virtual (`__c4x_delete_v`, given the slot's index), and
    frees the storage.
  - `delete[]` destroys the elements last first.
- **Globals:**
  - A global array of class objects is built element by element in the
    unit's initializer and destroyed, last first, after `main`.
  - A struct whose initializer list holds values known only at run time
    (Blood's `AISTATE`s) is assigned from a compound literal in the
    initializer.
  - The units' initializers run in link order. c4rlink appends the
    units' constructor lists in command-line order, and C4IX's loader,
    C4DOS's `dosload.c` and c4m's `load-c4r.c` all run the list before
    `main` (read in their source; run here only on c4m).
- **Also:**
  - Default arguments add up over a function's declarations (Blood's
    headers have them, its definitions not).
  - `c ? COMMAND_ID_1 : COMMAND_ID_0` has the enum's type, so `evPost`'s
    overloads are told apart.
  - A static member's definition outside its class is external, const
    or not, and a `static` written on it (Watcom took that, g++ does not)
    is dropped.
  - Constructors and destructors are no longer entered in their class's
    scope, where they hid the class's name inside its member functions.

Refused, as the plan's subset says: multiple inheritance, virtual base
classes, a class with virtual functions whose base has none, local or
unnamed classes with virtual functions, `new T[n] (...)`, `new T (...)` of
a type without a constructor, and arrays of class objects other than
globals of one dimension without an initializer.

The tests (`test-cxx.sh`): a program is a `tests/t_*.cpp`, or a
directory `tests/m_*/` of units linked in their names' order. Each way of
building links the runtime: `tests/rt-gxx.cpp` (`new`, `delete` and the
pure virtual handler, since this host has no 32-bit C++ library) for g++,
and `rt/libc4cxx.c` for c4cxx's C. c4lc compiles each unit with `-c`, and
`c4rlink32` links them.

- **`t_virt1.cpp`**: a Widget tree in the shape of Blood's `gui.h`,
  painted and deleted through base pointers. It has pure virtuals, a
  virtual destructor in the class, a key function outside it, a class
  whose virtuals are all inline, overrides two levels down, `Base::f ()`
  from an override, virtual calls from constructors and destructors, and
  each kind of derived-to-base conversion.
- **`t_virt2.cpp`**: `new` and `delete` of each kind. qtools'
  `DICTNODE : CACHENODE`, `new[]` and `delete[]` of a class with a
  constructor and destructor and of one with a constructor only, `delete`
  of 0, and a class whose base and member both have destructors.
- **`t_virt3.cpp`**: Blood's globals. `AISTATE`s whose lists hold a seq
  client registered by another global's initializer, a global array of
  menu items, default arguments from a declaration, `evPost` on a `?:`,
  and a static const member defined outside its class.
- **`m_loadsave/`**: Blood-RE's `LoadSave` registry across three units,
  packed as Blood was built. The head is built first in its unit, and
  every saver's constructor joins the list in link order. There are key
  function vtables in their units, and static ones.
- **Refusals**: `e_multiple.cpp` and `e_polybase.cpp`.

All 10 programs print as g++ `-m32`'s do, through gcc and through c4lc,
and gcc confirms their types. All 7 refusals are refused.

Blood: 79 of the 80 units translate in full. The other, `tile.cpp`,
calls `tmpnam`, which C4IX's libc does not declare. gcc `-m32` compiles
all 79 (with `-fpermissive`: c4's libc declares `memset` and the like on
`char *`). c4lc compiles 17. The other 62 stop at what c4lc lacks, not at
c4cxx's output:

- **Bit-fields** (57 units, from `PICANM` in `build.h` on): c4lc has none,
  the plan's M2b.
- **Parameters of function type** (5 units): mact's `control.h` declares
  `CONTROL_CenterJoystick (void CenterCenter (void), ...)`.

## State: M6, first part (2026-11-27)

All 80 of Blood's C++ units translate, and both gcc `-m32` and c4lc
compile every one (`blood-units.sh -compile`, now in `make test-c4cxx`).
What it took:

- **In c4lc and c4fc** (their own commits; c4lc-design §14.15 and §14.16):
  - bit-fields, by gcc's layout and by MSVC's under the new
    `-mms-bitfields`, which Blood's host build uses (`PICANM`, `XSPRITE`
    and the rest);
  - a local's initializer list that sets bit-fields (`EVENT evn = { a1,
    a2, 0, 0 }`);
  - pointers to arrays, `byte (*gammaTable)[256]` and its cast;
  - `extern AISTATE handIdle;` under `-c`, of a struct the unit never
    defines.
- **In C4IX's libc** (its own commit): `tmpnam`, which `tile.cpp` calls.
- **In c4cxx:**
  - `-fshort-enums`, `-mms-bitfields` and `-funsigned-char`. These are
    the layouts Blood's host build gives g++, as Watcom had them, so that
    what c4cxx works out itself (an array's length from a `sizeof`)
    agrees with gcc's.
  - A parameter of function type (mact's `control.h`, `void
    CONTROL_CenterJoystick (void CenterCenter (void), ...)`) is written as
    the pointer C makes it, which c4lc takes.
  - A bit-field narrower than `int` promotes to `int` in arithmetic, and
    an assignment to one, or its `++`, keeps its bit-field mark (gcc gives
    such an expression no `typeof`).

Tests (`test-cxx.sh`):

- **`t_bits.cpp`**: an `XSPRITE`-shaped struct and a class setting its
  bit-fields through pointers, compound assignments, `++` and `--`; and
  parameters of function type.
- **`w_watcom.cpp`**: Watcom's layouts. Enum sizes (`GAMEOPTIONS`), an
  `INPUT`-shaped packed struct, a double aligned to 8, array lengths from
  `sizeof`, unsigned plain `char`. A `w_*.cpp` program has
  `-fshort-enums -mms-bitfields -funsigned-char` in g++, c4cxx and gcc,
  and no c4lc run, since c4lc has no `-fshort-enums` or
  `-funsigned-char` yet.

All 12 programs print as g++ `-m32`'s do.

Still M6: the oracle. Blood built from c4cxx's C by gcc `-m32` must play
B0's demos with B0's checksums, frame for frame, and the class sizes must
be checked against g++'s.

## State: M7, done (2026-11-27)

**Blood built by c4 alone plays a demo as the host build does.** Every
C++ unit through c4cxx and then c4lc (`lc32`), every C unit through c4lc,
linked by c4rlink32 with the bare machine's libc, runs in node (libjs, the
bare machine, the game's directory as its drive) with c4-Blood's null
platform (`c4/blood-host.c`). It plays the shareware 1.11A's first demo
with the checksums of c4-Blood's host build (B0, g++), frame for frame:
4000 frames, about 15 minutes of game time, total `46c8dd68`. The image
is 4,030,508 bytes, a quarter of the loader's 16 MB. Built and played in
about five minutes. `src/c4cxx/blood-c4lc.sh` is the whole of it, and
`make test-c4cxx-blood-c4lc` runs it (c4-Blood and `BLOOD_RUN`, as
`test-c4cxx-blood`). The steps:

1. **B0:** c4-Blood's `c4/host.sh`.
2. **The 33 C units** host.sh builds (chocolate-blood's engine, MACT and
   audiolib, fframe and crc32 in C) and c4/'s three (`c4blood.c`,
   `blood-host.c`, `smacker.c`) through c4lc against C4IX's libc headers,
   with host.sh's flags: `-funsigned-char` everywhere, and Watcom's layouts
   (`-fshort-enums -mms-bitfields`) for Blood's own.
3. **The 81 C++ units** (Blood's 80 and `c4/netnone.cpp`) through c4cxx
   against the same headers, with Watcom's layouts, then c4lc.
4. **The link:** loadsave first (section 4.10), the rest, `rt/libc4cxx.c`
   for `c4/cxxrt.cpp`, and `.libc-bare`.
5. **The demo:** both play `-nodemo -playback BLOOD000.DEM -sum 1`, and
   the two lists of checksums must be the same.

What it took, besides c4cxx as M6 left it:

- **c4lc and c4fc:** `-funsigned-char` and `-fshort-enums`, Watcom's char
  and enums (c4lc-design 14.17). c4fc's character constants are their
  byte, 0 to 255, as c4lc's are (c4th's `C@` sign-extends: `'\xff'` was
  -1).
- **c4lc:** anonymous struct and union members (`a_asm.c`'s texture
  coordinates); a function declared static stays static when its
  definition does not say so (two units' `MoveToCeil`); `extern` on a
  definition (`a_asm.c`) defines it (c4lc-design 14.17).
- **C4IX's libc:** `malloc.h`, which the engine and MACT include; and on
  the bare machine, `O_RDWR` without `O_CREAT` or `O_TRUNC` opens a file
  that exists, to read. Blood opens its art and demos so. Before, it was
  a new file to write, on drive 1.

Two things of the bare machine, not of the compiler, the test works
around:

- **Its drive has no listing,** so `opendir` finds nothing there. c4/'s
  `c4blood_open` looks for a name without regard to case through it (the
  engine opens `tables.dat`; the file is `TABLES.DAT`), and Blood finds its
  demo loop with `_dos_findfirst`, which c4/ makes of it. So the drive is
  the game's files under their names and in lower case, and the demo is
  named on the command line, for both programs. A demo loop on c4 wants a
  listing (a register of the drive's), or C4IX's file system.
- **A file is read whole into memory when it opens** (`bare.c`), and
  `SHARE000.ART` is 16 MB: the run takes 256 MB (`-m 256`).

And `printf`'s `%llu` prints 0 in the bare libc, which the `t=` field of
blood-host's lines shows; the test compares the lines without it.

After M7 the compiler is done (section 4). The rest is the game port
(section 5).

## M6, done (2026-11-27)

**The oracle passes.** Blood built from c4cxx's C by gcc `-m32` plays the
shareware 1.11A's demos (BS111A) with the checksums of c4-Blood's host
build (B0, g++), frame for frame: 4000 frames, about 24 minutes of game
time across the demos, every frame's checksum the same, total `4e6389c4`
(800 frames give B0's `744cafc0`). Each struct and class c4cxx writes
has g++'s size, unit by unit. `src/c4cxx/blood-oracle.sh` is the whole
of it, and `make test-c4cxx-blood` runs it. It needs c4-Blood and
`BLOOD_RUN`, a directory of the game's files with `BLOOD.CFG`, and it
takes minutes. The steps:

1. **B0:** c4-Blood's `c4/host.sh`.
2. **Every C++ unit through c4cxx:** all 80 of Blood's and
   `c4/netnone.cpp`. c4cxx reads the host's headers (glibc's and gcc's,
   after c4-Blood's own) as g++ reads them: with Watcom's layouts
   (`-fshort-enums -mms-bitfields -funsigned-char`) and gcc's
   `__GNUC__` and `__i386__`, so that glibc takes the paths it takes for
   g++. Then gcc `-m32` compiles each with host.sh's flags, and with
   `-fno-common`, since C++ has no common symbols.
3. **The link,** as host.sh links: `loadsave` first (Blood-RE builds
   `LoadSave::head` in a global constructor that its savers link onto),
   host.sh's C objects, and `rt/libc4cxx.c` in place of `c4/cxxrt.cpp`.
4. **The demos:** both programs play them, every frame's checksum
   printed (`-sum 1`), and the two lists must be the same.
5. **The sizes:** with gdb, each struct and class c4cxx defined in a
   unit, in g++'s object for that unit and in c4cxx's.

What it took in c4cxx, besides the first part:

- **glibc's and gcc's headers** in C++:
  - `__builtin_va_list` (a type of its own: `Ty`'s new `cname` names it
    for the checks);
  - `__builtin_va_arg (ap, T)` and `__builtin_offsetof (T, m)` (new
    nodes, since both take a type);
  - other `__builtin_` functions called undeclared;
  - `wchar_t` (gcc `-m32`'s `long`);
  - `_Float32` to `_Float64x`;
  - exception specifications (`throw ()`, `noexcept`), which mean nothing
    with no exceptions;
  - asm labels (`__REDIRECT`);
  - `extern "C++"` inside `extern "C"`, as a stack of linkages.
- **A declaration directly in `extern "C"`** is `extern` ([dcl.link]).
  Before, key.h's `extern "C" volatile byte keystatus[256];` was a
  definition in every unit; common symbols had hidden that.
- **A class with no data** is one byte, as in g++ (`_e`, for `CFX` and
  `CCheatMgr`).

One thing B0 relies on: Blood calls `CheckLink` with sector -1 and reads
`gUpperLink[-1]`, which is `gLowerLink`'s last element in both builds
once c4cxx's C has no common symbols. With them, the linker put
`baseSprite` there, and a warp marker seemed to exist.

Test: `g_gnu.cpp` (the builtins, `extern "C++"`, `throw ()`, an asm label,
`wchar_t`, `extern "C"`'s declaration). A `g_*.cpp` runs through g++ and
c4cxx with gcc, not c4lc.

Not done, for M7: c4lc has no `-fshort-enums` or `-funsigned-char` yet, so
Blood through c4lc does not yet have Watcom's layouts. c4lc compiles
every unit (`blood-units.sh -compile`), but nothing links or runs that
yet.

## Files

| file | what |
|---|---|
| `c4cxx.h` | tokens, files, the preprocessor's interface, utilities |
| `ast.h` | the syntax tree: `Spec` and declarator chains, statements, expressions |
| `lex.c` | text to preprocessing tokens: line splices (CR LF too), comments, pp-numbers, prefixed literals, C++'s `::` `.*` `->*` |
| `pp.c` | the preprocessor. Macros by Prosser's algorithm (hidesets). Stringize, paste, variadic (named, GNU comma). `#if` with `defined`. Includes found as c4lc finds them. `#pragma` passed through. `__c4_inttypes__` predefined, and `__cplusplus` for C++ |
| `parse.c` | C to the tree, with typedef names tracked by scope; K&R definitions, bit-fields, designated initializers, compound literals, attributes kept as their tokens. C++: class bodies, qualified names, constructors and init lists, references, default arguments, `X x (args)`, `extern "C"`, `new`/`delete`, functional casts |
| `types.h`, `types.c` | C's types, their layout (gcc -m32's), conversions, and their names as C |
| `sema.c` | scopes, tags, `#pragma pack`, declarations to types, expression types, constant folding. C++ (M3): classes and their scopes, member calls, constructors, destructors and their cleanups, references, default arguments, a unit's initializer. M4: mangling and overloads. M5: bases, vtables, `new` and `delete` |
| `rt/libc4cxx.c` | the runtime c4cxx's C calls: `__c4x_new`, `__c4x_delete`, their arrays, `__c4x_pure` (outside `src/c4cxx/*.c`, which is c4cxx itself) |
| `test-types.sh` | the gcc oracle for types (`run-corpora.sh -T`) |
| `emit.c` | the tree to C: declarators from their chains, expressions with the parentheses precedence needs, `# N "file"` markers so that c4lc's `-g` lines are the source's |
| `main.c` | the driver |
| `test-same.sh`, `run-corpora.sh` | the c4lc oracle and its corpora (`make test-c4cxx`) |
| `c4odiff.py` | which functions and data differ between two objects |
| `test-cxx.sh` | C++ programs against g++ (gcc and c4lc), and what c4cxx refuses |
| `blood-c4lc.sh` | M7's test: Blood built by c4 alone plays a demo in node as the host build does (`make test-c4cxx-blood-c4lc`) |
| `blood-oracle.sh` | M6's oracle: Blood from c4cxx's C by gcc against c4-Blood's host build, frame for frame, and the sizes (`make test-c4cxx-blood`) |
| `blood-units.sh` | Blood's C++ units through c4cxx (`-parse` by default; `-compile` with Watcom's layouts, then gcc and c4lc) |
| `tests/` | the preprocessor's cases, with their output (checked against gcc -E); `t_*.cpp` programs, `m_*/` programs of several units, `e_*.cpp` refusals and `rt-gxx.cpp` (g++'s runtime) for `test-cxx.sh` |
