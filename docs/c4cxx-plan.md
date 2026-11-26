# c4cxx: a C++-subset front end for Blood (plan, 2026-11-26)

The plan for `src/c4cxx`, written 2026-11-26 in helios `~/gate/c4cxx/`
(the survey scripts and their output, `survey/cxxsurvey.py`, `re-blood.txt`
and `choc-blood.txt`, are still there). Section 4 says what has been built
since. Times are in ACST.

**Recommendation: a translator, `c4cxx`, from C++ to C, written in C, whose
output c4lc compiles at 32 bits (`lc32`). Two C features go into c4lc
itself: bitfields, and multi-character constants read as gcc reads them.
c4cxx gets an oracle that c4lc could not have. g++ `-m32` compiles Blood
directly, and gcc `-m32` compiles c4cxx's C output. The two builds must give
the same per-frame checksums on the same demos.**

## 1. What Blood is written in: the survey, checked

Two trees, both cloned read-only in `~/gate/bloodsw/`:

- **Blood-RE** `blood/src`: 57,900 lines of `.cpp` and 6,250 of `.h`. It
  includes the editor (`edit2d`, `edit3d`, `bstub`) and the three network
  stacks (`heat*`, `mpath*`, `tenblood`). The engine, audiolib and MACT
  come as prebuilt Watcom libraries.
- **chocolate-blood** `source/blood`: 80 `.cpp` (50,900 lines) and 7,300
  lines of `.h`. Its `qtools/` adds 3,600 lines (the RFF resource files,
  heap, `Rect`, `IniFile`). It drops the editor and the network stacks. Its
  portable C layers `source/build`, `mact` and `audiolib` add 42,300 lines.
  It is the base for the port, as the Blood/SW note said. All counts below
  are for chocolate-blood (Blood-RE's are within a few).

Counts are from `cxxsurvey.py`, which strips comments and strings and uses
regexes. Each was then checked by hand against the source.

| feature | count | notes |
|---|---|---|
| `class` bodies | 78, plus 4 `struct`s with methods (`QAV`, `Seq`, `SEQINST`, `Error`) | 211 class/struct/union bodies in all |
| derived classes | 50, **all single inheritance** | one is `struct DICTNODE : CACHENODE` (a struct's default base is public). No virtual or private bases |
| `virtual` | 91, in 46 classes | three hierarchies: `LoadSave` (17 savers), `Widget` (GUI), `CGameMenuItem` (menus) |
| pure virtual | **3** | `CGameMenuItem::Draw`, `Widget::Paint`, `Widget::HandleEvent`. The survey's 5 included two `= 0;` stores |
| virtual destructors | 2 | `Widget`, `Container`. `delete pWidget` must dispatch |
| classes with constructors | 60 | many overloaded: `CGameMenuItemText()` and `CGameMenuItemText(const char*, int, int, int, int)`, with 38 constructor declarations in `gamemenu.h` |
| classes with destructors | 13 | |
| constructor init lists | 8 out of line, more in class bodies | `Rect(int a1, ...) : f_0(a1), f_4(a2), ...` |
| **global objects with constructors** | **180** | 17 `static XLoadSave myLoadSave;` savers that link themselves onto `LoadSave::head`; about 40 `CGameMenu`s and some 120 menu items built with arguments in `menu.cpp`; `gGameMenuMgr`, `gKillMgr`, `gChoke`, `gDemo`, `Redbook`... **Their order matters**: see §3.6 |
| inline member functions in class bodies | 81 | e.g. `LoadSave()`'s body in `loadsave.h` |
| static data members / static member functions | 19 / 12 | `LoadSave::head`, `LoadSave::hFile`, `LoadSave::LoadGame` |
| `new` / `delete` | 24 / 10 | `new CGameMenuItemText(...)`, `new IniFile(...)`, `new QHeap(...)`, and two array `new`s of plain types |
| **function overloading** | **about 22 real sets** | free functions: `evPost(..., COMMAND_ID)` / `evPost(..., CALLBACK_ID)` (**overloaded on two enum types**), `evKill`, `FindSector`, `CheckLink`, `actSpawnSprite`, `sfxPlay3DSound`, `sndStartSample(char*, ...)` / `(ulong, ...)`, `aiSetTarget`, `GibFX`, `credPlaySmk`, `LevelWarp(void)` / `(int, int)`, `monoLeftString`..., `FileLoad`, `FileRead`, `func_83444`... Members: `CMenuTextMgr::GetFontInfo`, `CSMKPlayer::PlaySMKWithWAV`, `EventQueue::Kill`, `Resource::Load`/`Lookup`/`Probe`/`Read`, `IniFile::Load`, `CWeather::Draw`. The survey's "none" was wrong |
| default arguments | about 30 declarations | `sfxPlay3DSound(SPRITE *, int, int a3 = -1, int a4 = 0)`, `sndStartSample(..., int nChannel = -1, BOOL bLoop = FALSE)`; mixed with overloading |
| references | **4 uses**, plus `Rect` and `DUDEINFOACCESS` | `gibCalcWallArea(int a1, int &a2, ... int &a8)`, `GameErrorHandler(const Error &err)`, `CGameMenuEvent &event` (2); `Rect(Rect &other)`, `Rect& operator &=(Rect&)`, `DUDEINFO& operator[](int)` |
| operator overloads | 3, all members | chocolate-blood only: `DUDEINFOACCESS::operator[]` (returns a reference, used as `dudeInfo[i].x`), `Rect::operator!`, `Rect::operator&=`. Blood-RE's `<<`/`>>` are in `tenblood.cpp`, which chocolate-blood drops |
| templates | **1** | chocolate-blood's `QPtr<T>` (`qtools/include/qheap.h`): a 32-bit offset with `operator=` and `operator T*()`, so that cache nodes stay 4-byte fields on a 64-bit host. On c4's 32-bit target it is just `T*`: a three-line patch, not compiler work |
| `const` member functions | 3 (`Rect`) | `const` on `this` is checked by no one; dropped |
| access specifiers | 73 `public:`, 1 `private:` | ignored (no checks) |
| nested types | 10 structs and 4 **anonymous unions** in classes and structs (`ai.h`, `gui.h` x2, `weather.h`) | c4lc has no anonymous members |
| `bool` / `true` / `false` | 3 / 86 | mostly `BOOL`/`QBOOL` typedefs (423) and `TRUE`/`FALSE` macros |
| declarations in `for` | 280 | standard scope. Five possible uses after the loop were checked and are all nested loops, so nothing relies on Watcom's old scope |
| `//` comments | 707 lines | |
| `extern "C"` | 39 | engine, MACT, audiolib, `gamedefs.h`, `function.h`. chocolate-blood links under MSVC, so the C/C++ boundary is already marked correctly |
| **bitfields** | **about 240**, in packed structs | `db.h`'s `XSPRITE` (56 bytes), `XSECTOR` (60), `XWALL` (24) under `#pragma pack(push, 1)`, read straight from the map file: `IOBuffer1.Read(pXSprite, sizeof(XSPRITE))`. Also `seq.h` 18, `gui.h` 9, `engine.h` 8, `controls.h` 31. **c4lc has no bitfields** (c4lc-design §14.13, "Not done") |
| **multi-character constants** | 19 | `'ttaM'`, `'Matt'`, `'DULB'` as keys for `dbCrypt` and file magic. **c4lc reads `'ab'` as its last character** (§5.1). gcc gives `0x7474614D` |
| absent | none | templates (bar `QPtr`), namespaces, `using`, exceptions (`try`/`catch`/`throw`), RTTI (`typeid`, `dynamic_cast`), the `*_cast`s, `mutable`, `explicit`, multiple or virtual inheritance, conversion operators (bar `QPtr`), free operator functions, `std::`, C++ library headers. The headers included are `string.h`, `stdlib.h`, `stdio.h`, `io.h`, `fcntl.h`, `ctype.h`, `dos.h`, `conio.h`, `sys/stat.h`, `stdarg.h`, `memory.h`, `time.h`, `math.h`, plus `Windows.h` and SDL3 under `_WIN32` and in the SDL files |
| Watcom-isms | `#pragma aux` twice (`cdrom.cpp`, DPMI) | Blood-RE has 13. chocolate-blood's `build/PRAGMAS.H` is C with `int64_t`, as Chocolate Duke3D's is |

What chocolate-blood is: one shallow commit from 2025-05-01, "fixes". Its
README says "heavily WIP", and it is not known to play. It is built from MSVC
projects (`msvc/*.vcxproj`). It selects no game version: Blood-RE's
`APPVER_BLOODREV` is gone.

**So the guess holds.** Blood is "C with classes", plus overloading,
default arguments, global constructors and a few references. Nothing
needs exceptions, templates or RTTI, which are the hard parts of C++.

## 2. Translator or c4lc: why a translator

1. **c4lc builds everything.** It compiles the C4IX kernel, the libc, every
   game and C4DOS. `C4LC_DIFF` and the byte-reproducible images pin its
   output. C++ in c4lc would mean classes in its parser and an overload
   resolver in its type pass, with lookup rules that differ from C's. Every
   image would then depend on the new code paths, and every build would pay
   for the bigger parser (docs/compiler-speed.md).
2. **c4fc.** `test-c4fc` holds c4fc byte-identical to c4lc on
   `C4FC_GEN_SWEEP` and `C4LC_DIFF`, and the C4IX gate runs it. c4lc-only
   features have a precedent: Puzzles' `_Bool` and others (§14.13, "It is
   c4lc only"). But C++ would be a second language that c4fc does not have.
   Either the two compilers drift apart for good, or c4fc (6,600 lines of
   Forth) gets classes too: about the same work again, in Forth. A
   translator leaves both compilers' languages as they are, except for the
   two C features in §3.9.
3. **An oracle.** c4cxx's output is ordinary C, so gcc `-m32` compiles it
   as well as c4lc does. g++ `-m32 -fno-exceptions -fno-rtti` compiles
   Blood itself. Helios has g++ 15.2. It has no 32-bit libstdc++, but Blood
   uses none: a six-line `operator new`/`delete` shim in C is enough. So:
   - g++ on the `.cpp` must equal gcc on c4cxx's `.c`. That tests the
     translator alone, at host speed, on the whole game.
   - gcc on c4cxx's `.c` must equal c4lc on the same `.c`. That tests
     c4lc, as `duke3d-host` did for Duke.
   Inside c4lc, a C++ bug and a codegen bug would look the same.
4. **The output can be read.** A wrong frame can be traced in C that names
   things after the source (`CGameMenu__Add`). c4cxx writes `#line` markers,
   which c4lc honours under `-g` (c4lc-pp), so source maps and `prof.mjs`
   point at the `.cpp` lines.
5. **Against it.** c4cxx needs its own C type checker: about half its
   code repeats what c4lc's sema already does. Builds take two passes. An
   error in c4cxx's output shows up as a c4lc message about generated
   code. These costs are real, but smaller than 1 and 2.

**Written in C**, not c4sp Lisp. It builds with gcc on the host, like
c4rlink, so it runs fast where Blood is built. It stays inside what c4lc
compiles, so it can later run on the board (a stretch goal, as
docs/compiler-on-the-board.md did for c4lc). It lives in the c4 repo at
`src/c4cxx/`, with `make c4cxx` and `make test-c4cxx`.

**Other routes, considered and not recommended:**

- **clang + llvm-cbe** (LLVM IR to C): no clang on helios, and installing
  LLVM needs the user's OK. Its output is flattened, untyped C that no one
  can maintain, and the IR's `i64`/vector idioms would test c4lc's corners.
  A one-day probe is possible if the user wants it.
- **cfront**: written in C++ itself, with an unclear licence.
- **Converting Blood to C by hand once**: about 58,000 lines. A partly
  scripted conversion would take weeks and give a fork that can never
  follow upstream fixes, and it has no oracle. The translator is about the
  same work and can be run again.

## 3. Design

### 3.1 Pipeline

    blood.cpp ── c4cxx (pp ─ lex ─ parse ─ sema ─ lower ─ emit) ──> blood.c
              ── c4lc lc32 -c -O ──> blood.c4o ── c4rlink32 ──> blood.c4r

- **Preprocessor**: c4cxx has its own, with c4lc-pp's semantics: `-I` and
  `-D`; includes looked up beside the includer; `defined`; `#elif`;
  variadic macros. It predefines `__cplusplus 199711L` and `__C4__`, and
  passes `#pragma pack` through to the output. It is needed because the
  parser needs the declarations of every header, the C ones included.
- **Lexer**: C++ tokens on top of C's: `::`, `.*`/`->*` (rejected), the
  keywords. Multi-character constants become integers, as gcc reads them.
- **Parser**: C declarations, statements and expressions (the subset c4lc
  takes, so that c4's libc headers parse), plus class bodies, qualified
  names, constructors, init lists, `operator`, references, default
  arguments, `new`/`delete` and `extern "C"`. Ambiguities are resolved by
  whether a name is a type, which the parser tracks as it goes (classes are
  types). This needs no backtracking for this subset: `T(x)` as a
  functional cast is read only when `T` is a type.
- **Sema**: scopes, types, constant folding, expression types, member
  lookup, overload resolution, implicit conversions (§3.3).
- **Lowering** to a C AST (§3.2 to §3.8), then **emit**: C text with
  `#line` markers, a `typedef struct X X;` for every class, struct, union
  and enum tag, and full prototypes (`f()` in C++ becomes `f(void)` in C;
  this matters to c4lc, which misbehaves when a call passes more arguments
  than the callee declares).

### 3.2 Classes, layout and the vtable

- A class becomes a C struct. A base class is the first member, named
  `_b`: `struct TextButton { struct Button _b; ... }`. An inherited member
  `p->x` becomes `p->_b._b.x`. A derived-to-base pointer is then a cast at
  offset 0, with no adjustment and no null check.
- A **polymorphic root** (the first class in a chain with a `virtual`)
  starts with `const struct Widget__vt *_vp;`. Derived classes reach it
  through `_b`. A class that adds `virtual` on top of a non-polymorphic
  base would need an offset. It is **rejected**: Blood has none.
- **vtable**: `struct Widget__vt` has one function-pointer field per slot.
  The base class's slots come first, in declaration order, and each class
  appends its new virtuals. A slot's `this` type is that of the class that
  introduced it. An override defined on `Derived *` is cast in the
  initializer: `{ (void (*)(Widget *, int, int, char)) Label__Paint, ... }`.
  A virtual destructor takes one slot, `void (*_dtor)(Widget *, int
  del)`: `del` says whether the object is freed afterwards (Itanium's
  deleting destructor).
- **Where a vtable is emitted**: Itanium's key-function rule. It goes in
  the unit that defines the class's first virtual that is neither inline
  nor pure, as `const struct X__vt X__vtbl`. A class whose virtuals are
  all inline (`HeadWidget`) gets a `static` copy in every unit that
  constructs one. Without RTTI no one compares vtables, so copies are
  harmless, and c4lc forbids two initialized definitions anyway (§13.1).
- **A pure virtual** slot points at `__c4x_pure`, which prints a message
  and aborts.
- **Layout against g++**: Itanium reuses a non-POD base's tail padding for
  the derived class's first members. The `_b` member does not. This
  matters only to code that reads or writes whole class objects as
  bytes. In Blood, only plain structs are (`SPRITE`, `XSPRITE`;
  `LoadSave` writes globals). M6 checks this by sizes, and the oracle
  compares behaviour, not memory dumps.
- **Anonymous unions and structs** in a class or struct get names (`_u0`),
  and member access through them is rewritten. c4lc never sees one.

### 3.3 Name lookup, expression types and overload resolution

c4cxx types every expression, because three things need the types:

1. **Member access**: `a.f()` and `p->f()` need the class of `a` or `*p`:
   which function it is, whether it is virtual, and which base holds `f`.
2. **Implicit `this`**: inside a member function, an unqualified name is
   looked up in block scopes, then in the class and its bases, then at
   file scope. A data member found in the class becomes `this->m` (through
   `_b` as needed), and a member function call becomes `X__f(this, ...)`.
   `Base::f(...)` is a non-virtual call to `Base__f`. `::name` is the file
   scope. `Class::static_member` and `obj.static_member` name the global.
3. **Overloads**: every candidate whose parameter count fits (default
   arguments included) is ranked argument by argument, using C++'s three
   ranks:
   - *exact*: identity, array-to-pointer, function-to-pointer, adding `const`;
   - *promotion*: `char`/`short`/`bool` to `int`, an enum to `int`, `float`
     to `double`;
   - *conversion*: between arithmetic types, derived pointer to base
     pointer, `T*` to `void*`, `0` to any pointer, pointer to `bool`.

   The best candidate must be at least as good on every argument and
   better on one. Otherwise the call is **an error that names the
   candidates**, never a guess. User-defined conversions are not
   considered: Blood's only one is in `QPtr`. Enum types stay distinct:
   `evPost(i, 3, 0, CALLBACK_ID_17)` picks the `CALLBACK_ID` overload, and
   `kCmdOn`'s enum picks `COMMAND_ID`. So enumerators carry their enum's
   type, and the integer promotions and usual arithmetic conversions
   follow C. Constructors and member operators are resolved the same way.

Types are C's types, plus classes, references and `bool`, and with
c4lc's sizes at 32 bits: `int`, `long` and pointers are 4 bytes, and
`long long` is 8. c4cxx does not need `sizeof` for anything except
constant folding (a `const int` array size): it computes struct sizes with
c4lc's (gcc's) rules, or leaves `sizeof` for c4lc.

### 3.4 Name mangling

A readable mangling, the same in every unit, for every function with C++
linkage. `extern "C"` names and `main` are left alone. Readability matters
because names show up in `prof.mjs`, source maps and the IDE explorer.

| what | C name |
|---|---|
| free function | `name__` + parameter codes: `evPost__iim11CALLBACK_ID`, `FindSector__iiPi` |
| member function | `Class__name__codes`: `CMenuTextMgr__GetFontInfo__iPcPiPi` (typedefs such as `QBOOL` code as the type they name) |
| constructor / destructor | `Class__C__codes` / `Class__D` (`Class__DD` is the deleting slot) |
| static data member | `Class__name`: `LoadSave__head` |
| vtable type / object | `Class__vt` / `Class__vtbl` |
| per-unit initializer | `__c4x_init_<unit>` |

The codes are Itanium's letters, kept short: `v i j c h a s t l m x y f d
b`, `P` for a pointer, `R` for a reference, `K` for `const`, and
`<len><name>` for classes and enums. `F...E` is a function type and `z`
the varargs. c4lc's identifiers allow no `$`, so `__` marks a mangled
name. A user identifier containing `__` is reported where it could
collide (Blood has `__dassert`, which is harmless). Mangling all C++
functions, not just the overloaded ones, gives every unit the same name
for a function no matter which declarations it sees.

### 3.5 Constructors, destructors, references, default arguments

- **Constructor** `X__C__codes(X *this, ...)`, in C++'s order:
  1. the base constructor: the init list's, or else the default one;
  2. `this->_vp = &X__vtbl`, so that virtual calls inside the constructor
     go to `X`'s own functions;
  3. the members in declaration order: from the init list, a class member's
     default constructor, or nothing;
  4. the body.

  A destructor runs the other way round. It sets `_vp` back to its own
  vtable first, then runs the body, the member destructors in reverse, and
  the base destructor. No user constructor and no virtual means no
  constructor at all (a plain struct).
- **Objects with destructors that go out of scope**: c4cxx emits the
  destructor calls at the end of the block and before each `return`,
  `break` and `continue` that leaves it. A `goto` out of a scope that holds
  such an object is rejected. Blood's locals with destructors are few, so
  this is a small check.
- **Temporaries**: allowed only for classes without destructors. A
  temporary becomes a hidden local (`$clN` style, named `_t0`). A by-value
  argument of a class with a user copy constructor (`Rect(Rect&)`) is
  built in a hidden local by that constructor.
- **References** become pointers. Binding takes the address, and each use
  dereferences. A `const T&` bound to an rvalue gets a hidden local. A
  function returning `T&` returns `T*`, and its call becomes `(*f(...))`,
  so `dudeInfo[i].at70` works through `DUDEINFOACCESS__op_idx`.
- **Default arguments** are filled in at each call from the declaration in
  scope.
- **`const int N = 8;` at file scope** is internal and a constant in C++.
  c4cxx folds it, so that `int a[N]` and `case N:` are valid C. It also
  emits `static const int N = 8;` for any other use.
- **`bool`** becomes c4lc's `_Bool` (0 or 1 on store, as C++ wants), and
  `true`/`false` become 1 and 0.
- **Member operators** (`operator[]`, `operator!`, `operator&=`) become
  `X__op_idx`, `X__op_not`, `X__op_andeq`, chosen by overload resolution
  when an operand has class type.

### 3.6 Global objects with constructors

c4's object format already has what this needs. A `.c4r` has
constructor and destructor lists (`c`/`d`). c4lc emits them for
`__attribute__((constructor))`. c4rlink appends each input's list in
command-line order (`c4rlink.c` ~375). C4IX's loader runs them before
`main`, deepest library first (`src/c4ix/loader.c`), and so does c4lm's
`load-c4r.c` (~710). So:

- Each unit with dynamic initialization gets one
  `static void __c4x_init_<unit>(void) __attribute__((constructor))`. It
  constructs that unit's objects in declaration order, as C++ requires. A
  matching `__attribute__((destructor))` destroys them in reverse.
  Destructors run when `main` returns but not after `exit()` (c4cc
  README). Blood's global destructors close files and stop the CD, and
  matter to no one on c4, so that is acceptable.
- **Order across units is link order.** Blood-RE depends on it: `LoadSave
  LoadSave::head(123)` must be built before the 17 `static XLoadSave
  myLoadSave;`, whose constructor reads `head.prev`. **chocolate-blood no
  longer does** (found in M0). `head` has a default constructor, and
  `main` links the 15 savers explicitly (`ChainLoadSave()`: `head.HookRoot()`,
  then `blood_hook()`, `view_hook()` and the rest, each `myLoadSave.Hook()`).
  The 180 global objects still need constructing before `main`, but in no
  particular order. A c4cxx test still checks that link order is kept,
  since C++ code in general may rely on it.
- **To check in M5**: that the bare machine and C4DOS's RUN run the `c`
  list too, not only C4IX and c4lm. If one does not, the fallback is
  `c4cxx --init-main`: the user's `main` is renamed, and a generated unit
  calls the initializers in link order before calling it.

### 3.7 `new` and `delete` on C4IX's libc

A small runtime, `src/c4cxx/libc4cxx.c`, compiled by c4lc and linked like
`libc4ix.c4l`:

- `new T(args)` becomes `T__C__codes(__c4x_new(sizeof(T)), args)`.
  Constructors return `this`, so the expression has a value.
- `__c4x_new(n)` calls `malloc(n ? n : 1)`. On failure it prints and
  aborts, as Watcom did.
- `delete p` becomes `if (p) { dtor; free(p); }`. When the destructor is
  virtual, it becomes `p->_vp->_dtor(p, 1)` (destroy, then free), so
  `delete pWidget` frees a `Container` properly.
- `new T[n]` for a type without a constructor or destructor is
  `__c4x_new(n * sizeof(T))`. For a class with a destructor, a one-word
  count goes before the array so that `delete[]` can destroy each element.
- Placement `new` and `operator new` overloads are not supported. Blood
  uses neither.

C4IX's `malloc` is the one Duke and Quake use, and Blood's allocations are
of the same kind. Its `QHeap` is its own pool inside one big block.

### 3.8 The exact subset

**Supported**:

- C as c4lc takes it, with standard `for` scope, `//`, `bool`;
- classes and structs with data and functions, defined inline or outside
  the class body;
- single public inheritance, `virtual`, pure virtual, virtual destructors,
  calls qualified as `Base::f()`;
- constructors (overloaded, with init lists), destructors, copy
  constructors written by the user;
- static data and function members;
- overloading of free functions, members and constructors;
- default arguments;
- references (as parameters, as return values, and `const T&` bound to
  rvalues);
- member operator functions;
- `new`, `delete`, `new[]`, `delete[]`;
- global and static objects with constructors;
- `extern "C"`;
- file-scope `const` constants;
- anonymous unions;
- multi-character constants;
- nested struct and enum definitions, which go to file scope with their
  names unchanged, as Watcom-era code expects.

**Rejected, each with a message**:

- templates, namespaces, `using`;
- exceptions, RTTI, the `*_cast`s;
- multiple or virtual inheritance, and a `virtual` added on top of a
  non-polymorphic base;
- conversion operators, free operator functions, operators on non-class
  types;
- pointers to members, placement `new`;
- a `goto` out of a scope with live destructors;
- temporaries of classes with destructors;
- ambiguous overloads;
- `mutable`, `explicit`;
- local classes with virtual functions.

Access checks (`private`, `protected`) and `const` correctness are not
checked: c4cxx assumes code that g++ accepts.

**Patched in the game repository, not supported** (about 10 lines):
`QPtr<T>` becomes `T*` under `__C4__`, and the DPMI `#pragma aux` in
`cdrom.cpp` goes under `#ifndef __C4__`.

### 3.9 What c4lc needs (C, not C++)

- **Bitfields**, laid out as gcc `-m32` does, including under
  `#pragma pack(1)`, where gcc packs the bits across bytes. Blood's maps
  are read into `XSPRITE`, `XSECTOR` and `XWALL`, so a different layout
  silently breaks every map. Reads shift and mask, sign-extending signed
  fields. `=`, compound assignments and `++`/`--` read, modify and write
  the field. `&` of a bitfield is an error. This is a c4lc feature, as
  `_Bool` was (c4lc only; c4fc refuses a bitfield with its own message).
  It also clears an item on the gcc-port list (docs/gcc-port.md). It is
  tested by `src/tests/c4lc_bf.c` against gcc at 32 and 64 bits, plain,
  `-O` and `-minline`. If the user wants c4lc left alone, c4cxx can lower
  bitfields to masks over byte arrays instead, at a cost in speed on
  `xsprite[]`.
- **Multi-character constants as gcc reads them** (`'ab'` is
  `('a'<<8)|'b'`). c4cxx's output is already integers, but Blood's C units
  would also need it. Today c4lc gives the last character (§5.1, a c4cc
  quirk). Whether c4lc changes or c4cxx alone does it is a small decision
  for M2b. Checking c4lc's own uses of it comes first.
- **Plain `char` unsigned**, as Watcom's was. Blood and its engine depend
  on it (found in B0):
  - `cache1d`'s `filegrp[]` holds 255 for "not in a group file", and a
    signed `char` reads it back as -1, so `kread` indexes group -1;
  - the fire effect sums `char` pixels into a table index.

  gcc has `-funsigned-char`. c4lc needs the same option: plain `char`
  gets `unsigned char`'s type code. It must cover c4lc's own `char`
  literals and string loads, and c4cxx's output must say `signed char`
  where code means it. c4fc refuses the option, as with `_Bool`.
- **What M0 found** in chocolate-blood's 35 C units (engine, MACT,
  audiolib). With header shims (`c4/inc`, `c4/c4inc` in c4-Blood), c4lc
  compiles 34 of them:
  - **c4lc's preprocessor does not take CRLF line endings.** A
    backslash-CR-LF line continuation breaks every multi-line macro
    (`mact/mathutil.c` compiles once its CRs are gone). The game repo now
    has LF line endings. Teaching c4lc-pp to treat `\` CR LF as a
    continuation, as gcc does, is a one-line fix worth making anyway.
  - **Anonymous struct members** (C11): `a_asm.c`'s `texcoord64_t` is a
    union of a `uint16_t`, an anonymous `struct { uint32_t l, h; }` and a
    `uint64_t`. c4lc reports "bad struct member name". Either c4lc learns
    them (c4cxx must lower them for C++ anyway, §3.2), or a_asm.c is
    patched; it has about 50 uses.
  - **C4IX's libc** lacks `div_t` (declared in `VES2.H`, never used),
    `UINT64_C` and the `int_fast*_t` types (`opl3.c`, which c4 does not
    need: the card has a real OPL). Shims for now.
  - **MSVC/Watcom library calls**: `stricmp`, `strcmpi`, `strupr`, `itoa`,
    `_makepath`, `_splitpath2`, `filelength`, `_msize`, `_dos_findfirst`
    and `_dos_findnext`, `kbhit`/`getch`, `min`/`max`. All are in
    `c4/c4blood.c` and `c4/inc/c4blood.h`, plain C for both gcc and c4lc.
  - No bitfields in the C units. The 240 bitfields are all in the C++
    game code, so c4cxx sees them first, and the bitfield item above
    stays as it is.

## 4. Milestones

Every milestone ends with tests in `make test-c4cxx`, all builds run under
`~/bin/c4-lock`. The test programs are small `.cpp` files in
`src/c4cxx/tests/`. Each is built two ways:
- g++ `-m32 -fno-exceptions -fno-rtti`, giving the expected output;
- `c4cxx`, then c4lc `lc32`, then c4rlink32, run under c4m32 (libjs, as
  `test-c4lc` runs).

The outputs must be the same. Most tests also go through `c4cxx` then
`gcc -m32`, to keep translator bugs apart from c4lc bugs.

| | what | tests / done when |
|---|---|---|
| **M0** probe (1-2 days) | (a) chocolate-blood's C layers (`source/build`, `mact`, `audiolib`) through c4lc `lc32 -c`: list the C gaps. (b) Blood's game code through g++ `-m32 -fno-exceptions -fno-rtti -fsyntax-only` with c4's headers: list what g++ needs (`Windows.h`/SDL under `__C4__`, `io.h`/`dos.h` in a `c4inc/` as Duke's) | two lists in `~/gate/c4cxx/` |
| **B0** reference Blood (can run alongside M1-M5) | chocolate-blood built natively, g++ `-m32`, with a null display and no sound (`blood-host.c`, as `duke-host.c`). Plays a demo from the shareware RFFs and checksums every frame (`blood-sum.c`, as `duke-sum.c`). **This also answers whether chocolate-blood plays at all.** If not, its game code is fixed against Blood-RE first. Needs the shareware data, which needs the user's OK (§6) | a demo plays to its end with checksums, the same on two runs |
| **M1** pass-through | pp, lexer, parser for c4lc's C, emit C again | every `src/tests/*.c` that c4lc takes: `c4cxx` then c4lc gives the same output as c4lc alone. chocolate-blood's C units pass through, and c4lc compiles them |
| **M2** C sema | scopes, types, enums with their own types, constant folding, expression types, implicit conversions; `-dump-types` | golden type dumps for the C tests; all of `blood/*.h` parse as C++ declarations (class bodies may still be skipped) |
| **M2b** c4lc | bitfields; multi-character constants (§3.9) | `c4lc_bf.c` against gcc, 32 and 64 bits, plain, `-O`, `-minline`. `test-c4lc`, `test-c4fc` and `C4LC_DIFF` unchanged: no existing image changes by a byte |
| **M3** classes without virtual | members, inline and out-of-line member functions, implicit `this`, static members, constructors (one each), destructors and scope exit, init lists, references, default arguments, `bool`, file-scope `const`, anonymous unions, `extern "C"`, class tags as types | `t_class*.cpp`: a `Rect`, a stack class, constructors writing to a log, destructors at `return`/`break`, references swapping, `const Error&` |
| **M4** overloading and mangling | overload resolution (§3.3), overloaded constructors, member operators, mangling (§3.4), ambiguity errors | `t_over*.cpp`: `evPost` on two enums, `sndStartSample(char*, ...)`/`(ulong, ...)` with default arguments, `FindSector` 3 and 4 arguments, `LevelWarp()`, `DUDEINFOACCESS[i].x = 1`, `Rect &= Rect`; one test that must fail as ambiguous |
| **M5** inheritance, virtual, `new`/`delete`, globals | base sub-objects, vtables and the key-function rule, virtual and pure virtual, virtual destructors, `Base::f()`, virtual calls in constructors; libc4cxx; per-unit initializers and link order | `t_virt*.cpp`: a Widget tree painted and deleted through base pointers; a `LoadSave` registry across three units whose link order is checked; `new`/`delete[]` counted by a malloc wrapper. One run of each under C4IX (node, as `test-c4ix`), on the bare machine, and in C4DOS: does each run the `c` list (§3.6)? |
| **M6** Blood translates | every chocolate-blood `.cpp`, `qtools` included, through c4cxx; c4lc compiles every output | **the oracle**: Blood built as c4cxx then gcc `-m32` gives B0's checksums frame for frame on every shareware demo. Class sizes compared against g++ (a generated `sizeof` table) |
| **M7** Blood on c4lc | c4cxx then c4lc `lc32` then c4rlink32, with a null platform, run in node | the same checksums as B0. The image fits the loader (16 MB cap; Duke's was 4.7 MB) |

After M7 the compiler is done. The rest is the game port (§5).

### 4.1 M0 and B0, as run (2026-11-26)

- **M0a** (c4lc on the C units): as §3.9 says, 34 of 35 compile.
- **M0b/B0** (g++ and gcc `-m32`): all 113 units (33 C, 80 C++) compile
  and link into `blood-host` with:
  - header shims (`io.h`, `dos.h`, `conio.h`, `direct.h`, `memory.h`,
    and an SDL3 header with only the atomics, the mutex and the delay);
  - `-nostdinc++`, plus a 30-line `cxxrt.cpp` for `new`/`delete`, because
    helios has no 32-bit libstdc++;
  - eleven small source fixes for what MSVC let through: `void main`,
    implicit declarations with `short` parameters, `const` mismatches
    between `SCRIPLIB.H` and `scriplib.c`, a `static` after an `extern`,
    a mixed pointer subtraction, and a variable the non-Windows path
    lost;
  - include names spelled as their files are (145 includes);
  - CRLF line endings turned to LF.
- The non-`_WIN32` paths (DOS's `_dos_findfirst`) are the ones built, as
  on c4. chocolate-blood's only `_WIN32` code is file search.
- **c4-Blood** (`git@github.com:andrakis/c4-Blood.git`, created by the
  user): `master` is chocolate-blood's 37 commits, and branch `c4` holds
  the work above, in `c4/` and four commits.
- **The data**:
  - Blood 1.11 shareware: `blood.exe`, 17,349,441 bytes, SHA-256
    `b0b07c03...4a485`, from classicdosgames.com. Its installer archive
    `SHARE.SHR` is PKWARE DCL-imploded, and `c4/tools/unshr.py` unpacks
    it. Its terms allow free distribution "provided that you include all
    the original files" and no charge.
  - The user's full game: helios `~/blood/full`, mode 700.
  - chocolate-blood is version 1.21 code (`gGameVersion = {21, 1}`). The
    shareware is 1.11.

### 4.2 B0's run (2026-11-26)

- **The host build runs, and is deterministic** (two 600-frame runs give
  checksum total `2dc953f0`). It took:
  - `-funsigned-char`;
  - opening files without regard to case (`tables.dat` is `TABLES.DAT`);
  - backslashes in DOS paths;
  - VESA `ScreenMode = 1` (mode 2 copies to `0xA0000`);
  - fixing two bugs MSVC hid: the fire effect's signed sum, and an
    80-byte error buffer.
- **The version gap is the real obstacle.** chocolate-blood is Plasma Pak
  1.21, with Blood-RE's version switches removed:
  - **shareware 1.11** has `SHARE000.ART` (tiles 0-4095), not
    `TILES###.ART`, and 1.11's QFN fonts rather than 1.20's tile fonts at
    4096 and up. With the art name patched in (not committed), the 1.21
    code reaches the main menu with its blood drips, but with no text;
  - **the user's full game is registered 1.00** (April 1997). Its RFF
    lacks QAVs 118-124, and Blood-RE does not cover 1.00 at all.
- **So, for a playable shareware on c4**, either:
  - (a) put Blood-RE's `#ifdef SHAREWARE` (about 30 places in 12 files)
    and its `APPVER_BLOODREV` cases (about 200) back into the c4 branch
    for BS111A (shareware 1.11 of 23 September 1997), keeping
    chocolate-blood's portable layers; or
  - (b) start c4's game code from Blood-RE itself, built for BS111A, and
    take chocolate-blood's portability changes (`replace.cpp`'s engine
    hooks, `qsound.cpp`, the SDL-to-platform calls) onto it.

  (b) gives exactly the shareware's behaviour, demos included, and every
  other version Blood-RE covers. It costs re-porting chocolate-blood's
  changes, which are mostly in a few files. For the full game, 1.00
  needs Monolith's official patches to 1.11 or 1.21, or the user's 1.21
  files.

### 4.3 Rebased on Blood-RE (2026-11-26, the user's choice)

c4-Blood's `c4` branch (39c44e9, pushed) now has **Blood-RE's game code**
(4d4869a, every version through `APPVER_EXEDEF`). It sits on
chocolate-blood's engine, MACT, audiolib and qtools, built with g++ `-m32`
by `c4/host.sh`.

- **Why not chocolate-blood's game code:** it is an older snapshot of the
  decompilation, fixed at 1.21. Blood-RE has since corrected things a
  demo's sync depends on, such as the order of `Random()` calls.
- **What runs:** the **shareware 1.11A** (BS111A) plays its attract demos
  behind the menu: the cemetery, pickups, TNT. Two runs agree (800 frames,
  `744cafc0`). Plasma Pak 1.21 (BP121) builds from the same tree. Sync
  against DOS Blood frame for frame is not yet checked (that needs a
  DOSBox run).
- **The port** (commit 919be4d) is mostly tools: `c4/tools/forscope.py`,
  `packall.py`, and `c4/tools/port/*.py`. The engine hooks and the clocks
  behind functions follow chocolate-blood's pattern.

**What this means for c4cxx.** Blood-RE is Watcom C++ 10.6 code, and it
relies on Watcom's semantics. Each of these broke the g++ build until
given, so c4cxx (and c4lc) must give the same. Section 3's subset gains:

| Watcom | c4cxx / c4lc |
|---|---|
| plain `char` unsigned | c4lc option, plain `char` = `unsigned char` (§3.9) |
| 1-byte structure packing by default (`PLAYER`'s `int at1`) | c4cxx emits `#pragma pack(1)` around Blood's own definitions; c4lc honours it (it does) |
| an enum is the smallest integer type that holds its values | c4cxx gives an enum-typed member, parameter and variable that type (`unsigned char`, `short`...): `GAMEOPTIONS` is 373 bytes in demos and saves |
| a bit-field takes a whole unit of its declared type, even when packed (MSVC's rule) | the c4lc bit-field layout of §3.9 follows this rule, not gcc's default: `INPUT` is 22 bytes |
| a `for` declaration lives to the end of the enclosing block | c4cxx's scope rule for `for`, as a Watcom mode (34 loops in Blood-RE); the g++ build hoists them in the source |
| implicit `int`, and a function that returns a value may fall off its end | c4cxx accepts implicit `int`; falling off returns garbage, harmlessly. g++ needed `void` for `CWeather` |
| the game's definitions win over the library's at link time | `NAME_replace` and hooks, in the source, as chocolate-blood does |
| global constructors run in link order | **Blood-RE depends on it again** (`LoadSave::head` before the 17 savers): §3.6's link-order rule stands; `loadsave` links first |
| `_MAX_PATH` 144 | `c4/inc/c4blood.h` |

The sizes to test against (gdb on the host build): `DEMOHEADER` 411,
`INPUT` 22, `GAMEOPTIONS` 373, `XSPRITE` 56, `XSECTOR` 60, `XWALL` 24,
`SPRITE` 44, `SECTOR` 40, `WALL` 32. c4cxx's M6 adds a generated `sizeof`
table compared with the host build's.

### 4.4 M1, done (2026-11-26)

`src/c4cxx` (see its README.md) has the preprocessor, the lexer, a C parser
and a C emitter, about 2,900 lines of C. `make test-c4cxx` holds c4cxx to
c4lc: each corpus unit must compile to the same object as c4cxx's output of
it does, both with `-E` and with parse-and-emit.

| corpus | units the same |
|---|---|
| c4's `src/tests` | 90 of 90 |
| chocolate-blood's C units | 32 of 33 |
| Duke Nukem 3D's units | 33 of 34 |

In the other two, c4lc's own `-P` is the odd one out: gcc -E agrees with
c4cxx.

- Duke's `menues.c`: c4lc stringizes a string literal without its quotes.
- `opl3.c`: still open. c4lc's token dump agrees with c4cxx, so the
  difference is in c4lc's compile path.

Both are c4lc fixes, apart from c4cxx.

Found on the way and fixed: an argument that was used and then stringized
came out expanded. Expansion relinked the argument's own tokens; c4cxx now
expands a copy.

### 4.5 M2, done (2026-11-26)

C's types and a semantic pass: `src/c4cxx/types.c` and `sema.c`, about
1,600 lines.

- Declarations resolve to types.
- Structs are laid out as gcc `-m32` lays them out, including
  `#pragma pack`, `packed`, `aligned` and bit-fields.
- Every expression is typed.
- Integer constants are folded.

The test was meant to be golden type dumps. A stronger one took its place:
gcc `-m32`, which types and lays out C exactly as c4lc's 32-bit target
does. `c4cxx -check-types` writes `_Static_assert`s of every expression's
type and constant value, every struct's size and member offsets, and every
global's size, and gcc must find them all true.

All three corpora pass: src/tests 83 of 83, chocolate-blood 33 of 33,
Duke 34 of 34. That is over 150,000 assertions in four of Duke's units
alone.

Where c4lc accepts what gcc would not, sema uses a fallback type and
asserts nothing:

- a pointer divided;
- a word called as a function;
- a `const int` used as a constant (as C++ does).

M2's other test, Blood's headers parsing as C++ declarations, moves to M3,
since it needs class bodies.

### 4.6 M3, done (2026-11-27)

Classes without inheritance or `virtual`, lowered to C: about 1,800 lines
added to `parse.c`, `sema.c` and `emit.c`.

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

The tests are the plan's, made a little stronger. Each
`src/c4cxx/tests/t_class*.cpp` (a `Rect`, a stack class, constructors
writing to a log, destructors at `return`/`break`/`continue`, references
swapping, `const Error&`, and more) is built three ways:

1. g++ `-m32` without exceptions, RTTI or its library;
2. c4cxx, then gcc `-m32`;
3. c4cxx, then c4lc, run on `c4m32`.

All three print the same for all 4 programs. gcc also confirms the types c4cxx gave the
C it wrote (`-check-types`). Five `e_*.cpp` are refused with the messages
they expect. `src/c4cxx/test-cxx.sh` runs them, in `make test-c4cxx`.

M2's deferred test passes: all 80 of Blood's C++ units parse (Blood-RE's
game units and qtools, as `host.sh` builds them). Translated in full, 7 of
them are qtools units, and each compiles with both gcc and c4lc. Nearly
all the rest stop at qtools' `resource.h`, whose `DICTNODE` derives from
`CACHENODE` (M5). Overloads (M4) stop seven more. So M4 and M5 are what
stand between M3 and Blood, as planned.

Found on the way:

- **c4lc rejects a scalar compound literal**, `(int) { x + 1 }`, with
  "struct used before its definition". Struct ones work
  (`src/tests/bbport/compound_lit.c`). c4cxx uses an array of one instead.
- **g++ `-m32` links with gcc here.** helios has no 32-bit libstdc++, and
  these programs need none, as `host.sh` found.

### 4.7 M4, done (2026-11-27)

Overloading and mangling, about 700 lines more in `sema.c`:

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

The tests are the plan's, through `src/c4cxx/test-cxx.sh`:

- **`t_over1.cpp`**: `evPost` on two enums; `sndStartSample (char *,
  ...)` and `(ulong, ...)` with default arguments; `FindSector` with 3 and
  4 arguments; `LevelWarp ()`; a call for each rank.
- **`t_over2.cpp`**: qtools' `Rect`, with its constructors,
  `operator!` and `Rect &= Rect`. Also `DUDEINFOACCESS[i].x = 1` (from
  chocolate-blood, since Blood-RE has no such class), and `Error (...)`
  as a temporary.

Both print as g++ `-m32`'s do, through gcc and through c4lc. Two programs
must fail, one ambiguous and one with no candidate that fits.

Blood: 13 of 80 units translate in full and compile with gcc and c4lc.
The other 67 all stop at qtools' `resource.h` (`struct DICTNODE :
CACHENODE`), inheritance, which is M5.

Not done, as the plan allows:

- **User conversions:** neither converting constructors nor conversion
  operators take part in overload resolution.
- **By-value arguments:** an argument passed by value is copied as a C
  struct, even where the class has a user copy constructor. Blood's one
  (`Rect (Rect &)`) copies the members as C would.
- **`__` in names:** a user's identifier containing `__` is not reported
  where it could collide with a mangled name.
- **`inline` outside the class:** a member function defined `inline`
  outside its class is defined in each unit that defines it, so a header
  doing that would define it twice at link time. Blood has none so far.

### 4.8 M5, done (2026-11-27)

Inheritance, virtual functions, `new` and `delete`, and globals, about
750 lines more in `sema.c` and the runtime `src/c4cxx/rt/libc4cxx.c`:

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

The tests are the plan's, through `src/c4cxx/test-cxx.sh`, which now takes
programs of several units (`tests/m_*/`, linked in their names' order) and
links the runtime in each build:

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

All print as g++ `-m32`'s do, through gcc and through c4lc.

Blood: 79 of the 80 units translate in full. The other, `tile.cpp`,
calls `tmpnam`, which C4IX's libc does not declare. gcc `-m32` compiles
all 79 (with `-fpermissive`: c4's libc declares `memset` and the like on
`char *`). c4lc compiles 17. The other 62 stop at what c4lc lacks, not at
c4cxx's output:

- **Bit-fields** (57 units, from `PICANM` in `build.h` on): c4lc has none,
  the plan's M2b.
- **Parameters of function type** (5 units): mact's `control.h` declares
  `CONTROL_CenterJoystick (void CenterCenter (void), ...)`.

Not done, as the plan allows:

- **Tail padding:** `_b` does not reuse a base's tail padding as g++ does
  for a non-POD base. Blood's classes have none to reuse so far: `Widget`
  ends on four chars, and `LoadSave` is packed. M6's `sizeof` table is the
  check.
- **Mixed pointer comparisons:** a comparison or `?:` of a base and a
  derived pointer gets no cast. c4lc takes it, and gcc only warns.
- **Runs under C4IX and C4DOS:** the test programs run on c4m only. That
  C4IX and C4DOS run a program's constructor list is read in their
  loaders, not run.
- **`tmpnam`:** C4IX's libc does not declare it, so `tile.cpp` stops. That
  is a fix in the libc or in c4-Blood's shim, not in c4cxx.

## 5. What is left for Blood after the compiler

The Duke port (`~/git/c4/games/duke3d`, `c4/` is 3,250 lines) shows what a
Build game needs. Blood needs the same set, in a game repository on
chocolate-blood's history with a `c4` branch, as `andrakis/c4-Duke3D` has:

- **Display** `blood-gui.c`: takes the display (`udisplay_take` on C4IX),
  `gui_blit8` of the 8-bit frame scaled with the palette, keys to Blood's
  scancodes, the mouse locked (`GUI_PTR_LOCK`, `GUI_EV_REL`) to turn and
  aim. Also `blood-c4ix.c` and `blood-bare.c`, as Duke's. It replaces
  chocolate-blood's SDL3 `build/system.c` and `video.c`, and fills the
  engine's mode list from the sizes the display shows, as Duke's
  VIDEO SETUP does.
- **Sound** `blood-snd.c`: `FX_` on the card's sixteen sample channels.
  Blood's effects are 8-bit RAW/WAV in `SOUNDS.RFF`, with 3D panning from
  `sfx.cpp`. `MUSIC_` goes through audiolib's MIDI player on the OPL, as
  ROTT and Duke do: Blood's songs are MIDI in `BLOOD.RFF`. This replaces
  chocolate-blood's `build/sound.c` and its software OPL3 (`opl3.c`, not
  needed with a real card).
- **Clock and checksums** `blood-sum.c` (B0 already has them), and
  `test-blood` on the shareware demos, as `test-duke3d`.
- **`c4inc/`**: `io.h`, `dos.h`, `conio.h`, `malloc.h`, as Duke's.
- **Blood's own parts**:
  - RFF archives, encrypted (`qtools/resource.cpp`: C++, so c4cxx);
  - `blood.ini` (`IniFile`);
  - **Smacker** cutscenes (`smkplay.cpp` calls `SmackOpen`, and
    chocolate-blood has no Smacker library). Stubbed first. Porting
    libsmacker (C, LGPL-2.1) is possible later;
  - **CD audio** (`CCDAudio`): stubbed;
  - **network**: one player, as Duke's stub.
- **Speed**: chocolate-blood's `build/` is not Duke's `Engine/src`.
  `PRAGMAS.H` has the same `int64_t` `mulscale`/`divscale`, which cost
  Duke 13-20% until they got 32-bit paths (`fixedPoint_math.c`). The same
  work is needed here, and so is the `ld16` cost. The inner loops
  (`vlineasm4`, `mvlineasm4`, `hlineasm4`, `slopevlin`, `tvlineasm`) are
  in `a_asm.c` and get browser accelerators (`accel.mjs`) as Duke's have.
  Duke's accelerators cannot be reused as they are, because the engines
  differ in detail. Duke was 7.1 M VM instructions a frame. Blood should
  be in the same range, plus its heavier game logic, measured first.
- **Images and the catalogue**: `c4.mk`, `c4-images.sh` (C4IX against libc
  plus `libc4cxx`, and whole with `libc-bare`), `c4ix.vfs.txt` with
  `#@app Games|Blood|blood`.
- **Risk**: chocolate-blood is unfinished. B0 tells early how far from
  playing it is. Blood-RE is the reference for the game logic, since it is
  a matching decompilation. NBlood, which plays, is the reference for
  behaviour.

## 6. The licence

- **The game code has no grant from its owner.** Blood (Monolith, 1997) is
  owned by Atari, and Nightdive's 2019 Fresh Supply is closed. Blood-RE
  and chocolate-blood are a decompilation of Monolith's binaries. The GPL
  notice in each file ("Copyright (C) 2018, 2022 nukeykt") is the
  decompiler's. It cannot license Monolith's code, of which the
  decompilation is a copy in another form. NBlood and its forks have been
  public since 2019 without a takedown, but that is the owner putting up
  with it, not a licence. chocolate-blood has no licence file at all,
  only `BUILDLIC.TXT`.
- **The engine** is Ken Silverman's Build (`BUILDLIC.TXT`: free for
  non-commercial use, with credit), as for Duke and SW. **audiolib and
  MACT** are Apogee/3D Realms' GPL releases (from the Duke and ROTT
  sources).
- **The shareware data** (Blood 1.11 shareware) has its own licence file,
  which must be read before anything ships, as Duke's `LICENSE.DOC` and
  ROTT's `VENDOR.DOC` were. It has not been looked up or fetched, and
  fetching it needs the user's OK.
- **What that means**:
  - **c4cxx is c4's own code** and is clean whatever happens to Blood.
    Its tests are small programs written for it.
  - **Blood's translated code may never be shipped.** That means: no push
    of a c4-Blood repository, no Blood on the Pages disks, and no `games/`
    submodule. It could still be built on helios from the clones, for the
    user alone.
  - **That is the user's decision, and it should be made before M6.** Up
    to M5 nothing depends on Blood. Without Blood, c4cxx is only worth
    building if another "C with classes" program is wanted. None has been
    surveyed.

## 7. Size and effort

**c4cxx, in C:**

| part | lines (estimate) |
|---|---|
| lexer, including multi-character constants | 600 |
| preprocessor (c4lc-pp's semantics) | 1,100 |
| parser, C and the C++ subset, to an AST | 3,800 |
| types, scopes, expression typing, constant folding, overload resolution | 3,500 |
| lowering (classes, vtables, constructors and destructors, scope exits, references, default arguments, `new`/`delete`, initializers, anonymous unions) | 2,500 |
| C emitter with `#line`, mangling | 900 |
| driver, diagnostics | 400 |
| **total** | **about 12,800** (10,000 to 15,000) |
| libc4cxx | about 80 |
| tests (`.cpp` programs, golden outputs, harness) | about 3,000 |
| c4lc bitfields and multi-character constants (Lisp, plus its tests) | about 400 + 300 |

For scale: c4lc is 11,100 lines of Lisp, c4fc 6,600 lines of Forth, and
c4rlink about 2,400 lines of C. c4cxx is a C front end with C++ on top,
so it is about c4lc's size.

**Effort, in this project's session terms.** c4lc went from feasibility
(2026-08-03) to the full C of the ports in about two months, without an
oracle. With g++ as the oracle and c4lc as the back end already there:

| span | estimate |
|---|---|
| M0 | 1-2 days |
| B0 | 2-5 days. Unknown until tried: chocolate-blood may need real fixes |
| M1-M2 | 1-1.5 weeks |
| M2b | 2-3 days |
| M3-M5 | 2-3 weeks |
| M6-M7 | 1-2 weeks. Blood's 80 units will find what the small tests missed |
| **compiler done (M7)** | **about 5-8 weeks** |
| the game port (§5) after M7 | 2-4 weeks: platform 1 week (Duke's first play took 2 days); sound, Smacker stub, menus 1 week; speed and accelerators 1-2 weeks |
| **Blood playing well on c4** | **about 7-12 weeks in all** |

The largest risks, in order:
1. chocolate-blood does not play (B0);
2. overload or lookup corner cases in 50,000 lines (M6, but the oracle
   finds each one);
3. bitfield layout under `pack(1)` (M2b, tested against gcc);
4. speed (as Duke's: measured, then fixed).

## 8. Next steps, if the user goes ahead

1. The user decides on Blood's licence (§6): build only for themselves on
   helios, ship, or drop Blood.
2. The user OKs fetching the Blood shareware archive (its URL and digest
   to be found first, as for `3dsw12.zip`).
3. M0 and B0 run first. If B0 finds chocolate-blood far from playing, the
   rest is re-planned before any compiler code is written.
