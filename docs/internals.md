# C4 / C4M / C4KE internals

A working reference for the compiler and VM at the heart of this repo, and how
each layer above them extends it. Written from a full read of `c4.c`, `c4m.c`,
`src/c4ke/c4ke.c`, `src/c4cc/c4cc.c`, `load-c4r.c` and `src/c4lm/c4lm.c`.

Everything here was checked against the running code (`make c4 c4m c4cc`) rather
than inferred from comments.

Status (2026-09-30): brought current with the tree. File:line references
(`c4m.c:1135` and so on) date from the original read in early August 2026
and have drifted as the files grew; search for the named function or opcode
instead of trusting the number.

## 0. The layer cake

```
c4.c        original interpreter: compiler + VM in one file, ~535 lines
 └ c4m.c    same compiler + VM, extra opcodes, traps, multi-file input.
            Deliberately written in the C4 subset so `./c4 c4m.c ...` works.
    └ load-c4r.c   loads a .c4r image into memory and calls it as a C function
       └ c4ke.c    pre-emptive multitasking kernel, built on c4m's traps
          └ u0.h   user-side runtime; turns kernel traps into C functions
c4cc.c      separate, richer compiler (for/continue/varargs/attributes),
            emits through a handler table -> asm-c4r.c (.c4r)
```

Nesting really works: `./c4 c4m.c load-c4r.c -- c4ke.c4r` is c4 interpreting c4m
interpreting the kernel. It is slow but correct.

`c4.c` is upstream's file, unmodified apart from a portability shim (the
`INT_TYPE` word-size define at the top) and a little more `-d` output. It is
the reference every other layer degrades to, so nothing here changes it.

### 0.1 The rest of the tree

This document covers c4, c4m, C4KE, c4cc, c4lm and protected mode. The other
components have their own READMEs and design docs; [src/README.md](../src/README.md)
lists them all.

| Component | What it is | README |
|---|---|---|
| c4mp | c4m-compatible VM with no compiler: loads `.c4r` only, simulated SMP, all ten fused opcodes | [src/c4mp/README.md](../src/c4mp/README.md) |
| c4mpg | marked fork of `c4m.c` that checks every load, store and syscall buffer against a region table | [src/README.md#c4mpg](../src/README.md#c4mpg), [c4mpg-design.md](c4mpg-design.md) |
| c4bb | c4m as a microcoded 32-bit breadboard computer, simulated in JavaScript | [src/c4bb/README.md](../src/c4bb/README.md) |
| libjs | c4m as a direct JavaScript interpreter; boots C4KE and C4IX in Node or a browser Worker | [libjs/README.md](../libjs/README.md) |
| C4IX | the Unix-like second-generation OS (see §7.4 for its use of protected mode) | [src/c4ix/README.md](../src/c4ix/README.md) |
| C4DOS | single-tasking, trap-free DOS; the first rung of [the ladder](ladder-plan.md) | [src/c4dos/README.md](../src/c4dos/README.md) |
| c4sp / c4lc | a Lisp written in c4, and the optimizing C compiler written in it | [src/c4sp/README.md](../src/c4sp/README.md) |
| c4sc | c4lc's Lisp transliterated to C, so the compiler runs fast in the machine | [src/c4sc/README.md](../src/c4sc/README.md) |
| c4th / c4fc | Forth-2012 in c4, and a C compiler written in that Forth | [src/c4th/README.md](../src/c4th/README.md), [src/c4fc/README.md](../src/c4fc/README.md) |
| c4fp | IEEE binary32 and binary64 in software, what c4lc's `float` and `double` compile to (Part 9) | [src/c4fp/README.md](../src/c4fp/README.md), [c4-float.md](c4-float.md) |

---

# Part 1 — `c4.c`, the compiler

Four functions: `next()` (lexer), `expr()` (expressions), `stmt()` (statements),
`main()` (declaration parser + VM). There is no AST and no separate codegen pass.
The parser writes machine words into the text area `e` as it goes, and patches
jump targets afterwards by remembering pointers.

## 1.1 Memory pools

`main()` mallocs four 256 KB pools and never grows them:

| Pool   | Variable | Contents |
|--------|----------|----------|
| symbol | `sym`    | flat array of identifier records |
| text   | `e`, `le`| emitted instruction words |
| data   | `data`   | globals + string literals |
| stack  | `sp`     | VM stack (grows down from the top) |

`p` / `lp` point into a fifth pool holding the source text.

## 1.2 The symbol table

There is no struct type, so an identifier is 9 consecutive `int`s indexed by an
enum (`c4.c:53`):

```
Tk  Hash  Name  Class  Type  Val  HClass  HType  HVal      (Idsz = 9)
```

* `Tk` — token id: `Id`, or a keyword token (`If`, `While`, …)
* `Hash`/`Name` — hash and pointer into the source text (names are never copied)
* `Class` — `Num` (enum constant), `Fun`, `Sys` (builtin → opcode), `Glo`, `Loc`
* `Val` — enum value, code address, data address, or frame slot index
* `H*` — **shadow slots**. When a function's parameters and locals are declared,
  the global meaning of that name is saved into `HClass/HType/HVal`, and restored
  by the unwind loop at `c4.c:450` when the function body ends. That is the whole
  of C4's scope handling: one global namespace plus one save slot.

Lookup is a **linear scan** of the whole table (`c4.c:82-86`), comparing hash then
`memcmp` on the name. The hash is `tk = tk*147 + ch` folded with the length:
`tk = (tk << 6) + (p - pp)`.

Keywords and builtins are bootstrapped by *lexing a string* at `c4.c:363-368`:

```c
p = "char else enum if int return sizeof while "
    "open read close printf malloc free memset memcmp exit void main";
i = Char; while (i <= While) { next(); id[Tk] = i++; }        // keywords
i = OPEN; while (i <= EXIT) { next(); id[Class] = Sys; ... }  // builtins -> opcodes
next(); id[Tk] = Char;   // "void" is literally an alias for char
next(); idmain = id;     // remember main
```

So a "library call" like `printf` is not a call at all — it is a single opcode
emitted inline (`c4.c:167`).

## 1.3 The lexer, `next()`

Straight-line character dispatch. Points that matter in practice:

* `#` — **the entire line is skipped**. `#include`, `#define`, `#ifdef` are all
  silently ignored; there is no preprocessor.
* `//` comments only; `/* */` is not recognised.
* Numbers: decimal, `0x` hex, leading-`0` octal.
* Strings/chars share one branch. String bytes are written into the **data pool
  as they are lexed**, and `ival` becomes the pointer. A char literal sets
  `tk = Num`.
* Escapes: **only `\n` is translated.** `\t` lexes as `t`, `\0` as the character
  `'0'` (0x30), `\\` as `\`. This trips people up constantly.
* `src` mode (`-s`) prints each source line followed by the instructions emitted
  for it, by walking `le` up to `e`.

## 1.4 Types

```c
enum { CHAR, INT, PTR };   // 0, 1, 2
```

A type is an integer. Each `*` adds `PTR` (2). So `char*` = 2, `int*` = 3,
`char**` = 4. Tests are arithmetic: `ty > INT` means "is a pointer",
`ty - PTR` dereferences.

Pointer arithmetic scales by `sizeof(int)` when `ty > PTR`, and not at all when
`ty == PTR` (i.e. `char*`). Since there are no structs, "everything that isn't a
char pointer is word-sized" is exactly right.

There are no structs, unions, typedefs, floats, unsigned, or shorts.

## 1.5 Expressions — `expr(int lev)`

Precedence climbing. The trick is that the token enum is **ordered by
precedence**:

```c
Assign, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge,
Shl, Shr, Add, Sub, Mul, Div, Mod, Inc, Dec, Brak
```

`expr` parses a unary/primary term, then loops `while (tk >= lev)` consuming any
operator whose token id is ≥ the caller's level. Each binary operator recurses
with the *next* level up, which yields left-associativity for free.

Codegen shape for a binary op is always the same:

```
  <left>            ; result in register A
  PSH               ; push A
  <right>           ; result in A
  ADD               ; a = *sp++ + a
```

### lvalues via instruction rewriting

C4 has no notion of an lvalue node. Instead it looks at **the last instruction
emitted** and rewrites it:

* address-of (`&x`): the term ended in `LI`/`LC`, so back the emitter up one word
  (`--e`) and what remains is the address computation (`c4.c:202`).
* assignment (`x = v`): the last instruction is `LI`/`LC`; overwrite it with `PSH`
  so the address is pushed, evaluate the RHS, then emit `SI`/`SC` (`c4.c:229`).
* `++x` / `x++`: duplicate the load as `PSH` + `LI`, add/sub the element size,
  store, and for post-increment undo the adjustment in register A afterwards
  (`c4.c:213`, `c4.c:268`).

This is why `&`, `=` and `++` all report errors like "bad lvalue" — the check is
literally `if (*e == LC || *e == LI)`.

### short-circuit and ternary

`&&` emits `BZ` with a placeholder, parses the RHS, then patches the placeholder
to the current end. Same for `||`/`BNZ` and `?:` (`BZ` … `JMP` …). The pattern
`d = ++e; …; *d = (int)(e + 1);` is the backpatch idiom used everywhere.

## 1.6 Statements — `stmt()`

Only `if`, `while`, `return`, `{ }`, `;`, and expression statements. `if` and
`while` are the classic backpatch forms:

```
while:   a = e+1               ; loop top
         <cond>
         BZ  ->end
         <body>
         JMP a
  end:
```

No `for`, `do`, `switch`, `break`, `continue`, or goto. (`c4cc` adds `for` and
`continue`.)

## 1.7 Declarations — the loop in `main()`

Handles only file scope: base type (`int`/`char`/`enum`), then a comma list of
declarators.

* **Functions**: `id[Val]` = address of the first emitted word. Parameters are
  numbered `0..n-1`; then `loc = ++i`, and locals continue numbering upward.
  Local declarations are only accepted **immediately after `{`** — you cannot
  declare a variable in a nested block or mid-function.
  Body emits `ENT (i - loc)` … `LEV`, then the shadow-slot unwind runs.
* **Globals**: `id[Class] = Glo`, `id[Val] = (int)data`, and `data` advances by
  one word — *always one word, even for `char`, and there is no array support*.
  `char buf[100];` does not do what you want; C4 code allocates with `malloc`.
* **Enums** are the only way to get named constants (no `#define`). Plain `c4.c`
  rejects negative initialisers; `c4m` accepts them.
* Duplicate globals are an error in `c4.c` and deliberately allowed in `c4m`.

There is **no forward declaration and no linker** — a function must be defined
before use, and `JSR` targets are absolute addresses fixed at compile time.
(c4cc has since gained all of these: forward declarations resolve at write
time, c4rlink links, and initialized globals and arrays exist — see below.)

### c4cc additions (2026-08-02)

* **Initialized globals**: `int x = -5;`, `char c = 'A';`, enum constants,
  `char *s = "text";`, `int *fp = &func;` (the function must already be
  defined). Pointer initializers live in the data segment and are relocated
  by the two data-resident patch types added for them: a patch's ADDRESS was
  historically always a code-segment offset, so `.c4r` gained
  `C4R_PTYPE_DCODE` (-3, data word → code address) and `C4R_PTYPE_DDATA`
  (-4, data word → data address). Code-resident patches are written first,
  data-resident after.
* **Arrays**: `int a[10];` globally and locally, `char b[64];`,
  `int a[3] = {1, -2, RED};`, `char t[] = "abc";`. An `ATTR_ARRAY` symbol
  evaluates to the address of its storage instead of loading from it; local
  arrays reserve frame slots with the name at the lowest address so
  indexing ascends, and local char arrays pack bytes into words.
* **Local initializers**: constants, brace lists, `char t[6] = "ab"`,
  `char *p = "..."`, `int *fp = &func`. They compile to stores emitted
  right after `ENT`, so they re-run on every entry (C semantics: two
  same-depth calls reuse stack memory, and the fresh values must win),
  with zero-fill past the given elements. String templates live in the
  data pool; pointer-valued `IMM`s pick up relocation patches through the
  usual auto-detection.
* **`&array`** is a no-op (the name already evaluates to its address) and
  **`sizeof(array)`** returns total storage in bytes, kept in the symbol's
  `emit_Length` (shadow-saved like the other fields, so a local array can
  shadow a global one).
* **switch/case/default with a jump table** in the data segment (each entry
  a DCODE patch) plus `break` (loops too). Needs c4m's `JMPA` at runtime.
  `src/tests/test_switch.c` and `test_globals.c` are compared against gcc
  compiling the very same files.

---

# Part 2 — `c4.c`, the virtual machine

A stack machine with one accumulator, living in the `while (1)` loop at
`c4.c:483`. Dispatch is an if/else-if chain over the opcode integer.

## 2.1 Registers

| Reg | Meaning |
|-----|---------|
| `pc` | program counter, `int *` into the text pool |
| `sp` | stack pointer, grows **down** |
| `bp` | frame (base) pointer |
| `a`  | accumulator — every value passes through here |
| `cycle` | instruction counter |

Note `a`, `sp` and `bp` are **locals of `main()`**. Nothing running on the VM can
observe or modify them. That single fact is what `c4m` had to change, and what
blocks `c4lm` (see Part 7).

## 2.2 Encoding

One `int` per instruction. The first eight opcodes — `LEA IMM JMP JSR BZ BNZ ENT
ADJ` — are followed by a second `int` operand. Everything else is a bare word.
The test is literally `if (i <= ADJ)`.

## 2.3 Instruction set

```
LEA n   a = bp + n              load address of a frame slot
IMM n   a = n                   immediate / global address
JMP a   pc = a
JSR a   push pc+1; pc = a
BZ  a   if (!a) pc = a
BNZ a   if (a)  pc = a
ENT n   push bp; bp = sp; sp -= n     enter frame, n locals
ADJ n   sp += n                       pop n arguments
LEV     sp = bp; bp = *sp++; pc = *sp++    leave frame
LI      a = *(int *)a
LC      a = *(char *)a
SI      *(int *)*sp++ = a
SC      a = *(char *)*sp++ = a
PSH     *--sp = a
OR XOR AND EQ NE LT GT LE GE SHL SHR ADD SUB MUL DIV MOD
        a = *sp++ <op> a        (stack operand on the LEFT)
OPEN READ CLOS PRTF MALC FREE MSET MCMP EXIT      "syscalls"
```

Syscall opcodes read arguments straight off the stack without popping — the
caller's `ADJ` cleans up. `PRTF` is special: it reads `pc[1]`, the operand of the
**following `ADJ`**, to learn the argument count (`c4.c:527`).

## 2.4 Calling convention and frame layout

Caller: evaluate each argument left-to-right, `PSH` each, `JSR`, then `ADJ n`.
Return value is left in `a`. Callee starts with `ENT n`.

After `ENT`, with a function of `k` parameters:

```
        higher addresses
        ...
 bp+k+1  parameter 0        <- first (leftmost) parameter
 ...
 bp+2    parameter k-1      <- last parameter
 bp+1    return pc
 bp+0    saved bp           <- bp points HERE
 bp-1    local 0
 bp-2    local 1
 ...     sp
        lower addresses
```

Verified with `./c4 -s` on `int add(int a,int b){int c,d; …}`: `a` → `LEA 3`,
`b` → `LEA 2`, `c` → `LEA -1`, `d` → `LEA -2`.

**The key structural property, used by everything above:** `bp` is the address of
a `(saved_bp, return_pc)` pair, and `LEV` re-derives `sp` from `bp`. So a frame
pointer is a complete, self-describing continuation.

## 2.5 Bootstrapping `main()`

`c4.c:474-479` fabricates a call frame by hand:

```c
bp = sp = stack_top;
*--sp = EXIT;          // executed when main's return address is reached
*--sp = PSH; t = sp;   // t points at this PSH word
*--sp = argc;
*--sp = (int)argv;
*--sp = (int)t;        // "return address" -> the PSH word
pc = main;
```

When `main` returns, `LEV` sets `pc = t`, so the VM executes `PSH` (pushing the
return value from `a`) and then `EXIT`, which reads `*sp`. Two words of code
acting as an exit trampoline — a nice bit of minimalism, and the same trick C4KE
reuses to make tasks trap when they fall off the end of `main`.

---

# Part 3 — `c4m.c`, the extensions

`c4m` is `c4` plus roughly 1100 lines. It compiles natively **and** runs under
plain `c4`, which constrains it to the C4 subset (no structs, no arrays, locals
at the top of functions, `#` lines meaningless).

## 3.1 Compiler-side changes

| Change | Where |
|---|---|
| Multiple source files read into one buffer, in order | `c4m.c:1135` |
| Redefinition allowed; **last definition wins** (poor man's linking) | `c4m.c:1202` comment |
| `static`, `extern`, `__attribute__((…))`, `constructor`, `destructor` parsed and **ignored** | tokens at `c4m.c:178`, attribute skip at `c4m.c:1187` |
| Negative enum initialisers | `c4m.c:1173` |
| `&func` yields a function address (`Class == Fun` → `IMM val`) | `c4m.c:483` |
| Calling through an `int *`: `JSRI` for globals, `JSRS` for locals | `c4m.c:473-474` |
| Builtins can't be overridden (`if (id >= idmain)`) | `c4m.c:1207` |
| `-S` symbol dump, stack traces | `print_symbol`, `print_stacktrace` |

Function pointers need no declared signature — `int *f; f = (int *)&g; f(1,2);`
works, and passing the wrong number of arguments simply reads the wrong stack
slots. `c4cc` builds varargs on top of exactly this.

## 3.2 New opcodes, traps and settings — the full list

This is the one place that lists everything `c4m` adds over `c4`'s
instruction set, with when it arrived. Checked against the enums in
`c4m.c` (opcodes ~lines 339–390, `CONF_*` ~268, `TRAP_*` ~297) and
`git log -S` on 2026-09-30.

**How to read "arrived".** `c4m.c` began as `c4_multiload.c` (first
commit `c980cab`, 2022-08-26) and was renamed in `2e3bb59` (2023-07-29).
Until `9ccd20f` (2026-01-25) the extensions were *interleaved* with
c4's own opcodes (`JSRI`/`JSRS` after `JSR`, `RALC` after `MALC`,
`JMPA`/`TLEV` after `PSH`, everything else before `EXIT`), so an older
arrival date is when the **name and behaviour** appeared, not the
number. Since `9ccd20f` they are appended after `EXIT` (38), so c4's
own numbering is untouched and the numbers below hold.

### Opcodes

| # | Name | Builtin / emitted by | What it does | Arrived |
|---|---|---|---|---|
| 39 | `PUTC` | `putchar` | write one character | `9ccd20f` 2026-01-25 |
| 40 | `PUTS` | `puts` | write a string | `9ccd20f` 2026-01-25 |
| 41 | `RALC` | `realloc` | guest realloc via the size table | `74652ff` 2023-05-17 (in `c4_multiload.c`); its VM case was commented out and it fell through to `TRAP_ILLOP` until `63e274a` 2026-08-23 (§3.6) |
| 42 | `MCPY` | `memcpy` | copy bytes | `74652ff` 2023-05-17; off-by-one at both ends fixed `9d4c591` 2026-08-05 |
| 43 | `STRC` | `stacktrace` | print a stack trace from the symbol table | `e05052e` 2022-08-26 |
| 44 | `ITH` | `install_trap_handler` | install the trap handler, return the old one | `5b37f41` 2024-11-06 |
| 45 | `_OPC` | `__opcode` | opcode number by name (`"LEV"` → 8) | `5b37f41` 2024-11-06 |
| 46 | `_BLT` | `__builtin` | **named only**: no VM case, so it halts (no handler) or raises `TRAP_ILLOP`; the host-side `__builtin()` stub returns −1 (§3.6) | `5b37f41` 2024-11-06 |
| 47 | `_TRP` | `__c4_trap(type, param)` | raise any trap from code (does not mask the cycle interrupt) | `5b37f41` 2024-11-06 |
| 48 | `OPCD` | `__c4_opcode(args…, op)` | execute opcode `*sp`; one that takes an operand (incl. the fused ones) raises `TRAP_OPV` and becomes a harmless `C4CY` | `5b37f41` 2024-11-06 |
| 49 | `_JMP` | `__c4_jmp` | `pc = *sp++` — leave one handler for another without touching the frame | `5b37f41` 2024-11-06 |
| 50 | `_ADJ` | `__c4_adjust` | `sp += *sp` | `5b37f41` 2024-11-06 |
| 51 | `C4CF` | `__c4_configure(CONF_*, v)` | set a `CONF_*` value, return the old one (table below) | `5b37f41` 2024-11-06 |
| 52 | `C4CY` | `__c4_cycles` | the cycle counter | `5b37f41` 2024-11-06 |
| 53 | `TIME` | `__time` | milliseconds (native clock; `/proc/uptime` under plain c4; 0 when neither answers, §4.7) | `5b37f41` 2024-11-06 |
| 54 | `SIGH` | `__c4_signal(sig, handler)` | install the machine's handler for a signal (a POSIX one in native builds); raises `TRAP_PM_VIOLATION` in protected mode, where C4IX and C4KE give the task a handler of its own | `5b37f41` 2024-11-06 |
| 55 | `SIGI` | `__c4_sigint` | the SIGINT number (0 under plain c4) | `5b37f41` 2024-11-06 |
| 56 | `USLP` | `__c4_usleep` | sleep; raises `TRAP_PM_VIOLATION` in protected mode, where C4IX sleeps just the task | `5b37f41` 2024-11-06 |
| 57 | `INFO` | `__c4_info` | the `C4I_*` capability bits, plus `C4I_TRAPH` (0x400) when a handler is installed; raises `TRAP_PM_VIOLATION` in protected mode | `5b37f41` 2024-11-06 |
| 58 | `OPSL` | `__c4_ops_list` | the opcode-name string (`"LEA ,IMM ,…"`, 5 bytes a name) | `5b37f41` 2024-11-06 |
| 59 | `C4IV` | `__c4_invoke` | self-modifying stub rewritten to a `JMP` (§3.5) | `9ccd20f` 2026-01-25 |
| 60 | `FLT` | `__c4_float` | float sub-ops (`c4m_float.c`, `FLTI_*`) in native c4m; under plain c4 raises `TRAP_ILLOP` so a handler may emulate it. FTOI returns an integer (it returned a float's bits until F3), and a result's upper word is zero on 64-bit c4m | `9ccd20f` 2026-01-25 |
| 61 | `JSRI` | compiler: call through an `int *` global | `pc = **pc` | original c4m (`c980cab` 2022-08-26) |
| 62 | `JSRS` | compiler: call through an `int *` local | `pc = *(bp + n)` | original c4m (`c980cab` 2022-08-26) |
| 63 | `JMPA` | compiler: `switch` jumptables | `pc = a` | `5b37f41` 2024-11-06 |
| 64 | `TLEV` | trap return (the `tlev_instruction` word) | reload `pc/sp/bp/a/mode` from the handler's argument slots (§3.3) | `5b37f41` 2024-11-06 |
| 65 | `DBG` | none (reach it with `__c4_opcode`) | raise `TRAP_DEBUG` | `9ccd20f` 2026-01-25 |
| 66–69 | `CPUI CPUN CPUS CPUH` | — | c4mp's processor opcodes (SMP) | **named only** in c4m, `13a369d` 2026-08-23 |
| 70–75 | `CAS XCHG FADD CWAI CWAK IPI` | — | c4mp's atomics, the futex pair, a directed interrupt | **named only**, `13a369d` 2026-08-23 |
| 76–78 | `LXI SXI TRAW` | `c4lc -mcisc` (LXI/SXI) | c4mp's fused array-element load/store and raw-terminal toggle (c4or1k) | **named only**, `13a369d` 2026-08-23 |
| 79 | `LDL` | `c4lc -mfuse`, c4th | `a = *(bp+n)` | the probe `13a369d` 2026-08-23; the set settled at ten (79–88) in `bbb9ec3` 2026-08-24 |
| 80–86 | `LDG PSHL PSHG LEAP IMMP LIP ADDL` | `c4lc -mfuse` | the compiler-shaped seven (docs/fused-opcodes.md) | **named only** in c4m, `bbb9ec3` 2026-08-24; executed by c4mp, oisc4, c4bb, libjs |
| 87 | `STL` | `c4lc -mfuse`, c4th | `*(bp+n) = a` | `bbb9ec3` 2026-08-24 |
| 88 | `POPA` | `c4lc -mfuse`, c4th | `a = *sp++` | `bbb9ec3` 2026-08-24 |
| 89 | `FPU` | `__c4_fpu(r, b, a, op)` | IEEE single and double (F3, docs/c4-float.md): `sp[0]` the sub-op, `sp[1]` a, `sp[2]` b, `sp[3]` r; floats as bits, doubles by address. Announced by `C4I_FPU` (0x10000). c4m and c4mp run softfp.c compiled natively, libjs and the c4bb sim `src/c4bb/sim/fpu.js`; a c4m or c4mp built for the VM by c4lc lends its host's; plain c4, c4cc builds and oisc4 name it only and raise `TRAP_ILLOP` | F3 2026-10-22 |

A **named-only** opcode is one c4m knows the name of but does not
execute. With no trap handler installed it halts with "*NAME* (opcode
*n*) … is not an instruction this machine has"; with one, it raises
`TRAP_ILLOP` like any custom opcode. The names are there so every
mirrored table (c4mp, oisc4, c4bb, libjs) holds the same names at the
same numbers, the debug disassembler can name them, and `c4l` can say
what an image needs; `__opcode()` answers with a number for them, so a
guest must feature-test with `C4I_*` bits, not by name. Opcodes at or
above `INS_SIZE` (89) are the custom range: C4KE's start at `CO_BASE`
(128), §4.2.

### Traps (`TRAP_*`, the handler's first argument)

| # | Trap | Raised by | Arrived |
|---|---|---|---|
| 0 | `TRAP_ILLOP` | an opcode c4m does not execute — the custom-opcode hook; also `FLT` without float support | `5b37f41` 2024-11-06 |
| 1 | `TRAP_HARD_IRQ` | the cycle interrupt, parameter `HIRQ_CYCLE` (0). c4bb adds `1` (PIT) and `2` (mailbox) through the same handler (§4.7) | `5b37f41` 2024-11-06 |
| 2 | `TRAP_SOFT_IRQ` | `__c4_trap()` from user code | `5b37f41` 2024-11-06 |
| 3 | `TRAP_SIGNAL` | a POSIX signal (native builds only) | `5b37f41` 2024-11-06 |
| 4 | `TRAP_SEGV` | nothing today: its only raise site in `c4m.c` is commented out | `5b37f41` 2024-11-06 |
| 5 | `TRAP_OPV` | `OPCD` asked to run an opcode that takes an operand | `5b37f41` 2024-11-06 |
| 6 | `TRAP_PM_VIOLATION` | a syscall (or `INFO`, `USLP`, `SIGH`) in protected mode — see Part 7 | `9ccd20f` 2026-01-25 |
| 7 | `TRAP_DEBUG` | `DBG` | `9ccd20f` 2026-01-25 |
| 8 | `TRAP_MPG_VIOLATION` | **reserved**: raised only by c4mpg (docs/c4mpg-design.md), never by plain c4m | `d9184c0` 2026-09-06 |

### Settings (`CONF_*`, the first argument of `__c4_configure`)

| # | Setting | Meaning | Arrived |
|---|---|---|---|
| 0 | `CONF_CYCLE_INTERRUPT_INTERVAL` | fire `TRAP_HARD_IRQ` every *n* cycles; 0 = masked (§3.4) | `5b37f41` 2024-11-06 |
| 1 | `CONF_CYCLE_INTERRUPT_HANDLER` | the function that interrupt enters | `5b37f41` 2024-11-06 |
| 2 | `CONF_PRIVS` | **named only** (with `PRIV_KERNEL`/`PRIV_USER`): `C4CF` does not handle it, prints "c4m: C4CF issue" and ends the run with −100 | `5b37f41` 2024-11-06 |
| 3 | `CONF_TRAP_RESTORES_INTERVAL` | 1 = `TLEV` restores the interval saved at trap entry (§3.4) | `a18c38c` 2026-08-05 |

The `C4I_*` bits `INFO` reports are the enum just above the traps in
`c4m.c`. Two more come from c4bb rather than c4m, and a program asks
for them the same way: `0x800` (the interval timer, `BB_I_PIT` in
`include/c4bb_info.h`) and `0x2000` (the mailbox, `C4I_MBOX` in
`include/c4bb_mbox.h`); §4.7 uses both.

The fused opcodes split deliberately. `LDL n` (`a = *(bp+n)`), `STL n`
(`*(bp+n) = a`) and `POPA` (`a = *sp++`) are what a stack-machine code
generator needs to treat the frame as registers; c4th's native back end uses
exactly those three. The other seven only pay for c4cc's and c4lc's output, so
c4m names them but halts on them, and c4mp, oisc4, c4bb (in microcode) and
libjs execute all ten. They are emitted only on request, by the `fuse` pass
in c4opt (`c4lc -mfuse`, or `c4opt-run.lisp -mfuse` on any image), and plain
c4 refuses a fused image by name. See [fused-opcodes.md](fused-opcodes.md).

`__opcode("NAME")` (`OPSL`/`_OPC`) lets running code look opcode numbers up by
name at runtime, which is how C4KE stays decoupled from c4m's numbering.

## 3.3 Traps — the important part

`trap()` (`c4m.c:921`) synthesises a call frame for a handler, *in the running
program's own stack*, exactly matching the layout `ENT`/`LEV` expect. Handler
signature:

```c
void handler(int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc);
```

Stack built by `trap()`, low to high (after backing off `TRAP_OFFSET` = 15 words
so the handler can't scribble on the interrupted frame):

```
 sp -> [handler locals, sized by peeking at the handler's ENT n]
 bp -> saved bp        (points at itself; the handler's frame base)
       &tlev_instruction   <- "return pc": a word containing the TLEV opcode
       returnpc  \
       saved sp   |
       saved bp   |  these are the handler's ARGUMENTS
       saved a    |  -> the handler can assign to them
       saved mode |
       parameter  |
       trap type /
```

`pc` is then set to `handler + 2`, skipping the handler's own `ENT n` (the trap
already applied it).

The elegance: when the handler returns, `LEV` sends `pc` to `&tlev_instruction`,
a single global word holding the `TLEV` opcode. `TLEV` (`c4m.c:1535`) reads
`pc/sp/bp/a/mode` back **out of the argument slots**. Because those slots are the
handler's parameters, an ordinary C assignment such as `sp = ...` or
`returnpc = ...` inside the handler *changes where the VM resumes*.

That is the entire mechanism behind C4KE. A trap handler is a C function that can
rewrite the VM's registers by assigning to its own parameters.

Trap sources: the `TRAP_*` table in §3.2 lists every one, what raises
it and when it arrived. The one C4KE is built on is `TRAP_ILLOP`, the
custom-opcode hook; `TRAP_PM_VIOLATION` is the subject of Part 7.

## 3.4 The cycle interrupt

```c
__c4_configure(CONF_CYCLE_INTERRUPT_INTERVAL, n);
__c4_configure(CONF_CYCLE_INTERRUPT_HANDLER, (int)&handler);
```

The VM loop checks `if (interval && !(cycle % interval))` (`c4m.c:1314`) and
fires `TRAP_HARD_IRQ`. It then **sets the interval to 0**, so the handler must
re-arm it — which is exactly how C4KE's `critical_path_start/end` work
(setting the interval to 0 is the kernel's "disable interrupts"). The
interrupt never lands on a `TLEV`, since a half-finished trap return is not a
context either side could resume.

**`CONF_TRAP_RESTORES_INTERVAL` (2026-08-05).** A kernel can opt in with
`__c4_configure(CONF_TRAP_RESTORES_INTERVAL, 1)`. `TLEV` then restores the
interval that was in effect when the trap was taken, so the interrupt mask
becomes part of the trapped context and stays masked across the whole
handler, including its return. A handler that wants to choose the interval
for the context it resumes declares an eighth parameter ahead of the usual
seven:

```c
void handler(int interval, int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc);
```

Opting in also makes a zero interval mean "masked" for signals: a pending
signal is deferred (not dropped) until the mask lifts. Without the opt-in a
zero interval is indistinguishable from "preemption never enabled", so
signals are delivered regardless. C4IX opts in (`sched.c`); c4mp, c4bb and
libjs mirror it.

## 3.5 Helper builtins

* `__c4_opcode(args…, OP)` — execute an arbitrary opcode. The opcode number is
  the **last** argument because arguments are pushed left-to-right, so it lands
  at `*sp`. This is why every wrapper in `u0.h` lists its arguments in reverse.
* `__c4_jmp(addr)` — bare `JMP`; used to leave one trap handler and enter another
  without touching the frame.
* `__c4_adjust(n)` — move `sp` manually, so a handler jumped into via `__c4_jmp`
  can make room for its own locals.
* `__c4_invoke` / `C4IV` — self-modifying stub that rewrites its own first word to
  a `JMP`; only meaningful when c4m itself is being interpreted.

## 3.6 Caveats found while reading (verified)

1. **`realloc()` was broken under `c4m`** (fixed 2026-08-23 in `63e274a`,
   which also gave the guest allocator a size table; `make test-c4m-mem`
   pins it under native c4m and under plain c4). As first found: `RALC` is in the builtin list, so
   `realloc` compiles to opcode 41, but the VM case is commented out
   (`c4m.c:1496`). It falls through to `TRAP_ILLOP`; with no handler installed
   `trap()` prints "missed a trap" and execution continues with `a` unchanged —
   which holds the *size argument*. Confirmed:

   ```
   $ ./c4m ralc2.c
   before=1532551456
   c4m: missed a trap, no handler installed
   after=32
   ```

   The pointer was silently replaced by `32`. The fix implemented `RALC`.

2. **Protected mode is switched off in three independent places.** See
   [Part 7](#part-7--protected-mode) — it is now working again after one
   ownership bug was fixed.

3. `__builtin()` is a stub that always returns `-1` (the `__builtin`
   function in `c4m.c`). Use `__opcode("NAME")`.

4. `-p` / `-P` pool-size flags are noted as non-functional in the header comment
   and do parse, but they run before `poolsz` is used, so they *do* take effect —
   the comment looks stale.

5. `print_stacktrace` finds functions by scanning the symbol table for a `Fun`
   whose `Val` matches a decreasing `pc`, bounded to 0xFF words; it gives up with
   "couldnt find function entry" on larger functions.

---

# Part 4 — C4KE, and why the trap design matters

`src/c4ke/c4ke.c` (~4580 lines on 2026-09-30) is a pre-emptive multitasking kernel. There is no
inner VM: **every task is ordinary C4 code running on the one c4m interpreter**.
A "context switch" is just a trap handler assigning new values to its own
`a/bp/sp/returnpc` parameters before returning through `TLEV`.

## 4.1 Task representation

A task is an `int` array indexed by the `TASK_*` enum (`c4ke.c:275`). The parts
that matter: `TASK_REG_A/BP/SP/PC` hold the saved registers, `TASK_STATE`
(`STATE_RUNNING`/`WAITING`/`ZOMBIE`…), `TASK_NICE` for scheduling, `TASK_BASE`
for the stack (256 KB since 2026-08-28, §4.7), plus argv, signal handlers, mailbox and accounting.

## 4.2 Custom opcodes

Opcodes ≥ `CO_BASE` (128) are unknown to c4m, so they raise `TRAP_ILLOP`.
`trap_handler` (`c4ke.c:2000`) looks the opcode up in `custom_opcodes[]`, calls
`__c4_adjust` to make stack room using the handler's `ENT n` operand, and
`__c4_jmp`s into it. `install_custom_opcode` stores `handler + 2` (skipping
`ENT`), same convention as c4m's trap entry.

So `OP_SCHEDULE`, `OP_USER_SLEEP`, `OP_USER_START_C4R`, … are *syscalls
implemented as illegal instructions*. User code invokes them through `u0.h`:

```c
#define schedule()  __c4_opcode(OP_SCHEDULE)
#define exit(code)  __c4_opcode(code, OP_TASK_EXIT)
```

Opcode numbers are resolved by **name** at process start
(`__u0_ops_init`, `u0.h:184`) via `OP_REQUEST_SYMBOL`, so the kernel can
renumber freely.

## 4.3 The two switch paths

Both do the same six assignments; they differ only in how they are entered.

* **Cooperative** — `op_schedule` (`c4ke.c:1627`): save `a/bp/sp/returnpc` into
  the current task, pick a new one with `kernel_task_find()`, load its registers
  into the parameters, return. `TLEV` resumes the *other* task.
* **Pre-emptive** — `ih_cycle` (`c4ke.c:2354`): identical body, but entered from
  `TRAP_HARD_IRQ` when the cycle counter fires.

`critical_path_start/end` bracket kernel-structure updates by setting the cycle
interval to 0 and back — interrupt masking, one `__c4_configure` call.

## 4.4 Starting and ending a task

`start_task_builtin` allocates a `TASK_STACK_SIZE` stack (0xFFFF bytes until
2026-08-28, 0x40000 since, with a poisoned guard below it — §4.7) and builds the
same hand-made frame `c4.c` builds for `main`, but with a *custom opcode* as the
trampoline:

```c
sp = bp = stacktop - 6;          // c4 printf can read past the top; leave slack
*--sp = OP_TASK_FINISH;          // executed when main returns
*--sp = opcode_PSH; temp = sp;
*--sp = argc;
*--sp = (int)argv;
*--sp = (int)temp;               // return address -> the PSH word
t[TASK_REG_PC] = (int)entry;
```

When the task's `main` returns, it runs `PSH; OP_TASK_FINISH`, which traps into
`op_task_finish` — the kernel marks it a zombie and switches away. A task cannot
free its own stack while running on it, so `task_idle` reaps zombies later.

## 4.5 Programs: the `.c4r` format

`c4cc`+`asm-c4r.c` emit **C4 Relocatable** images: header, code, data, patch
list, symbols, constructor/destructor lists (format table at `asm-c4r.c:32`).
`load-c4r.c` reads one, relocates code/data addresses via the patch entries, and
then simply *calls the entry point as a C function* — loaded code is native VM
code, not re-interpreted. Constructors (like `__u0_init`) run before `main`,
destructors (like the `atexit` runner) after.

c4cc writes format version 2. c4lc and `c4rlink` write version 3, whose only
addition is a MEMSZ word in the old header padding, so uninitialized globals
become zero-filled BSS instead of bytes in the file. `load-c4r.c` reads both;
[src/c4cc/README.md](../src/c4cc/README.md) has the full layout.

## 4.6 The RAM filesystem

The kernel owns a flat table of named byte buffers, reachable from user
tasks through the `OP_VFS_PUT/GET/UNLINK/COUNT/NAME` opcodes (u0 wrappers:
`vfs_put`, `vfs_get`, ...) and consulted by the program loader **before**
the host filesystem. Because the VM has no write syscall, this is the only
write path that exists under the VM -- and it closes the loop: c4cc running
under C4KE renders its image into memory and stores it with `OP_VFS_PUT`,
and the kernel executes it straight from the table (`load-c4r.c` gained a
memory-source mode, `c4r_load_mem`). `make test-c4ke-ramfs` demonstrates
the whole cycle: compile inside C4KE, run from RAM, plus the same loop with
c4sp's Lisp optimizer producing the image.

Detecting the kernel without u0: probe `OP_REQUEST_SYMBOL` (fixed number
128), but only when `__c4_info()` reports `C4I_TRAPH` (0x400, "a trap
handler is installed") -- under bare c4m the probe would just print trap
noise, and the missed trap deterministically leaves 128 in the
accumulator, so any answer above 128 is a real kernel resolution.

## 4.7 The mailbox, wall-clock scheduling and task memory

Three things C4KE gained in August–September 2026, all visible mainly
on c4bb, the simulated board (docs/c4bb-design.md).

### The mailbox (ad57ad0, 2026-09-09)

`src/c4ke/extensions/c4ke_mbox.c` carries frames **between the host and
a task**, and **between tasks**. It is an ordinary kernel extension
(`kext_register("mbox", …)`), in the Makefile's extension list and in
c4bb's `build-images.sh`.

*The host side* is c4bb's `MBOX_*` device (docs/c4bb-design.md, "The
mailbox"). A host that boots with `boot(…, { mbox: bytes })` gets a
region of guest RAM holding two rings, host→guest in the first half and
guest→host in the second. A ring is `[cap][head][tail][data…]` in
words, head and tail monotonic counters; a frame is
`[len][type][seq][payload…]` (`len` counts the three header words) and
never wraps — a `-1` at the head means "skip to the next multiple of
cap". Both sides use plain loads and stores: the host through the
arena's typed array (`src/c4bb/sim/mbox.js`, `HostMailbox`), the guest
through `include/c4bb_mbox.h` (`mb_init`, `mb_poll`, `mb_done`,
`mb_send`, `mb_bell`). The registers are `MB_BASE` 0x1b4, `MB_LEN`
0x1b8 and `MB_BELL` 0x1bc: a guest write to the bell reaches the host's
`onDoorbell` (1 = "I wrote frames", 2 = "idle, you may stop me"), and
the host's `raiseMbox()` jams `TRAP_HARD_IRQ` with parameter 2 through
the cycle handler — to C4KE just one more tick, which is enough for the
scheduler to notice the frame. The device is announced by `C4I_MBOX`
(0x2000) in `__c4_info()` and never probed; on native c4m those
addresses are ordinary memory, so `mb_init` returns 0 without the bit.

*The kernel side.* The first task to receive or await host frames
becomes their **owner** (`mbx_host_owner`). Frames stay in the host
ring until that task takes them, so the scheduler can see them waiting.
Every task can also have an **inbox**: a ring of the same shape,
1024 words, allocated on first delivery into `TASK_MBOX` and freed with
the task (`kernel_clean_task`); its frames are
`[len][type][sender pid][payload…]`. When nothing is runnable the idle
task rings `MB_BELL_IDLE`.

The scheduler reads the host ring directly against the registers
(`mbx_host_pending` in `c4ke.c`) rather than through `c4bb_mbox.h`,
because innerbench compiles `c4ke.c` from source in c4m, which skips
`#include` — calling a function it never saw broke innerbench once
(docs/c4ix-desktop.md, "I: innerbench").

The opcodes sit at the top of the custom range (`MBX_OP_BASE` 248 =
`CO_BASE + CO_MAX - 8`), are registered by name with
`kext_register_symbol`, and a task resolves them through
`OP_REQUEST_SYMBOL` (`include/c4ke_mbox.h`, passed as a source after
`u0.h`, since c4cc has no `#include`):

| Opcode | Wrapper | Does |
|---|---|---|
| `OP_MBOX_INFO` | `mbox_info()` | 1 when the host mailbox is fitted |
| `OP_MBOX_SEND` | `mbox_send(type, payload, n)` | a frame to the host, then rings `MB_BELL_REPLY` |
| `OP_MBOX_RECV` | `mbox_recv(buf, max)` | the next host frame into `buf` → its length in words, or 0 |
| `OP_MBOX_AWAIT` | `mbox_await(timeout_ms)` | block until a host frame (owner) or an inbox message → 1; timeout → 0; 0 ms = never time out |
| `OP_MSG_SEND` | `msg_send(pid, type, payload, n)` | a frame into `pid`'s inbox → 1, or 0 (no such pid, or full); wakes it if it is awaiting |
| `OP_MSG_RECV` | `msg_recv(buf, max)` | the next inbox frame → its length, or 0; `buf[2]` is the sender |

`OP_MBOX_AWAIT` returns at once with 1 if something is already there.
Otherwise it parks the task in `WSTATE_MESSAGE` with
`TASK_WAITARG = kernel_last_time + timeout` (0 for never) and switches
away; `kernel_task_find_real` wakes it with 1 in `TASK_REG_A` when its
inbox count is non-zero or (for the owner) a host frame is pending, and
with 0 once the deadline passes. On a host without the mailbox (native
c4m) `OP_MBOX_INFO` answers 0 and the task-to-task opcodes still work.

**Tests.** `src/c4bb/tests/test-c4ke-mbox.mjs`, run by
`test-c4bb.sh`, boots C4KE on c4bb with a mailbox fitted and runs two
programs from `src/c4ke/bin/`: `mbecho.c` binds to the host mailbox and
echoes every frame (type | 256, each payload word + 1; a type-0 frame
ends it) while idling in the kernel between frames, pinned at 22 frames;
`mbpair.c` starts a copy of itself as a child, which sends its parent
three words with `msg_send`. **Known flaky:** it fails about one run in
four (docs/c4ix-desktop.md, "Found on the way, not fixed"): the
recorded symptom is C4KE's "Custom opcode not found: 1073741824" in the
mbpair output, and the test check that trips is "kernel shut down
cleanly". It fails as often with the C4KE source from before the
desktop work; the cause is not yet known.

### Scheduling on the wall clock (2574376, 2026-08-28)

The cycle interrupt (§3.4) fires after so many *instructions*, so C4KE
has to measure how fast the host runs and guess a count. c4bb also has
an interval timer: `C4KE_PIT_REG` (0x1a0, `BB_PIT` in
`include/c4bb.h`) ticks every *n* real milliseconds, 0 masks it, and it
raises the same `TRAP_HARD_IRQ` through the same handler, so all of
C4KE's scheduling works unchanged — only *when* differs.

At boot `main` calls `kernel_pit_probe(C4KE_PIT_MS)` (10 ms) unless
`-c` was given. The probe asks two questions. First `__c4_info()` must
report `C4KE_PIT_INFO` (0x800, `BB_I_PIT` in `include/c4bb_info.h`):
the register cannot be probed blind, because on every other host 0x1a0
is ordinary memory and a store there is a wild write. Then it arms the
interval, reads it back, masks it and reads that back; a machine that
answers anything else schedules on the cycle counter. A non-zero result
goes in `kernel_pit_ms`, which then also means "the PIT is ours to
touch", and:

* measurement is skipped (`enable_measurement = 0`) — the board's boot
  went from 726 ms and 2.047 G cycles to 3 ms and 76.5 k;
* `critical_path_start/end` mask by writing 0 / `kernel_pit_ms` to the
  register instead of the cycle interval. The PIT keeps its deadline
  across a mask (only a *different* interval restarts the countdown) —
  a change this kernel forced on the hardware, because a kernel masking
  on every trap would otherwise never be interrupted again;
* the boot prints "scheduling on the interval timer, every 10ms of real
  time" and arms the register just before scheduling starts.

Native c4m, a C4KE nested inside c4m on the board, and `-c nn` all
schedule on the cycle counter as before.

**Without the PIT** the interval comes from `measure_ips()`: wait for
`__time()` to change, then count cycles for 200 ms
(`KERNEL_MEASURE_QUICK`) and scale by 5, giving instructions per
second; the interval is a tenth of that, then halved (about 50 ms of
work). `-m` skips the measurement and uses `KERNEL_CYCLES_MIN`
(10000), which the same halving turns into 5000. `-c nn` asks for the
cycle interrupt by name, skips the measurement and the minimum check,
and is also halved: `-c 20000` schedules every 10000 cycles.

**With no clock at all**, `__time()` returns 0 for ever: under plain c4
c4m reads `/proc/uptime`, and when that cannot be opened it prints
"unable to open uptime file" once and latches 0 (`c4_time_host`). The
first loop of `measure_ips` then waits for time to move and never
returns, so such a boot needs `-m` (or `-c`). (On c4bb the C4DOS build
of c4m asks C4DOS's clock first, which is why the board no longer hangs
there — the comment on `c4_time` in `c4m.c`.) Anything that waits on
`kernel_last_time`, such as a sleep or an `OP_MBOX_AWAIT` timeout,
cannot expire while the clock reads 0.

### Task memory (a8a546b, 2026-08-28)

[task-memory.md](task-memory.md) is the full account. In short: building
C4IX under C4KE (`b4ke -f c4ix.b4k`) died with a false out-of-memory
because c4sc's recursive descent overran the 64 KB (0xFFFF) task stack
and wrote through the heap below it. The fix and what followed:

* `TASK_STACK_SIZE` is now 0x40000 (256 KB), with a 256-byte guard of
  poison words (`TASK_STACK_POISON`) below every stack, checked so an
  overrun is **reported** (`TASK_STACK_LOW`, `TASK_STACK_BROKEN`)
  instead of corrupting someone else's heap.
* The kernel tracks a task's own allocations and reclaims them when
  the task ends, exited or killed (`kernel_task_malloc` / `_free` /
  `_realloc` / `_release`). That works where `MALC`/`FREE` trap, i.e.
  for a task running protected; `make test-task-mem` pins it.
* With the compilers' own cleanup added, the whole C4IX build under
  C4KE went from needing 128 MB to fitting in 13 MB.

---

# Part 5 — `c4cc`, the enhanced compiler

`src/c4cc/c4cc.c` is a fork of the c4 front end with the back end abstracted
behind `c4cc_emithandlers[]`, an array of function pointers indexed by `EH_*`
(`c4cc.c:208`). `asm-c4r.c` fills that table in; the parser never
names an output format.

Over `c4.c`/`c4m.c` it adds:

* `for`, `continue` (`curr_continue` tracks the loop head)
* variadic functions — `ATTR_VARIADIC`; the arg count is pushed and
  `__c4cc_make_va` is called, backing `include/stdarg.h`
* comma/compound expressions: `a = (update(), value);`
* `extern` (unresolved functions become linker patches), `static`
  (kept out of the exported symbol table)
* `__attribute__((constructor))` / `((destructor))` with priorities, feeding the
  `.c4r` constructor/destructor segments
* a wider identifier record (`Idsz = 16`) carrying `Attr`, `emit_Val`,
  `emit_Length`, `ArgCount`
* error messages that print the offending statement and column

* `switch`/`case`/`default` (a jump table, needs c4m's `JMPA`), `break`,
  arrays (`ATTR_ARRAY`), initialized globals and local initializers — see
  "c4cc additions" in §1.7

Still not present: `struct`/`union`/`typedef`, `do`/`while`,
`unsigned`/`short`/`long`, and a preprocessor. Builds run a preprocessor
first: `gcc -E` on the host, as in
`gcc -E -DC4CC=1 -Iinclude src/tests/vararg.c | ./c4cc -o vararg.c4r -`, or
the tree's own [cpp](../src/c4dos/cpp.c) inside C4DOS and C4KE. For
structs and the rest, use c4lc ([c4lc-design.md](c4lc-design.md)).
[src/c4cc/README.md](../src/c4cc/README.md) has the current limits.

## Test programs

`src/tests/` is the best documentation of edge behaviour:
`test_customop.c` (traps and custom opcodes, heavily commented),
`test_trap.c`, `test_illins.c`, `test_signal.c`, `test_timekeeping.c`,
`vararg.c`, `test_extern*.c`, `c4_jailbreak.c`, `mandel.c`,
and the OISC experiment (`oisc-c4.c` compiler / `oisc-min.c` interpreter).

---

# Part 6 — `c4lm`, and how to unblock it

`src/c4lm/c4lm.c` aims at a DOS-like environment running under **plain c4**, so
none of c4m's traps, `__c4_opcode`, or register access exist. The README records
it as stuck: "Cannot task switch more than twice successfully. Maybe it's because
we have no access to the bp register."

That diagnosis is right, and the precise reason is visible in
`perform_task_switch1` (`src/c4lm/c4lm.c:217`):

```c
pts_bp = (int *)(&addr + 1);   // == this frame's bp   -> saved bp slot
pts_pc = (int *)(&addr + 2);   // == bp+1              -> return pc slot
...
*pts_bp = pts_task[TASK_REG_BP];
*pts_pc = pts_task[TASK_REG_PC];
```

The addressing is correct (`&first_local + 1 == bp`). **Two things break:**

1. **`sp` is left pointing into the outgoing task's stack.** `LEV` is
   `sp = bp; bp = *sp++; pc = *sp++` — it takes `sp` from the bp that was current
   *before* the pop. So after returning, `pc` and `bp` belong to the new task but
   `sp` is still `old_bp + 2`. Everything survives until the next `PSH` or `ENT`,
   which writes into the *other* task's stack. Hence "works twice, then dies."

2. **A never-run task cannot be entered this way at all.** `kstart_task`
   (`c4lm.c:376`) sets `TASK_REG_BP = bp` and `TASK_REG_PC = entry`, but `LEV`
   will do `sp = bp; bp = *sp++; pc = *sp++` — reading two uninitialised words
   from the fresh stack rather than jumping to `entry`. The push sequence it
   builds below `bp` is never reached, because `sp` is discarded.

## A fix that works, without sp/bp access

Both problems dissolve once you use the property from §2.4: **a frame's `bp` is
the address of a `(saved_bp, return_pc)` pair, and `LEV` rebuilds `sp` from it.**

* Fix (1) with a **double `LEV`**. Return into a *trampoline* — a function whose
  body is empty, so it compiles to `ENT 0; LEV` — entered at its `LEV`
  (`(int *)&trampoline + 2`). The first `LEV` installs the new `bp` (with `sp`
  still bogus); the trampoline's `LEV` then recomputes `sp = bp` and pops the real
  pair. After that, all three registers are consistent.
* Fix (2) by **faking the pair for a new task**. Reserve a word pair `X` in the
  new stack and fill in the frame the entry point expects:

  ```
  X[0] = 0                 // saved bp   (ENT will overwrite X[1], not this)
  X[1] = entry             // return pc  -> the trampoline LEVs straight into it
  X[2] = on_exit_address   // where the entry function's own LEV will land
  X[3] = arg 2             // after ENT, bp == X+1, so bp+2 == X+3
  X[4] = arg 1             //                          bp+3 == X+4
  ```

  and set `TASK_REG_BP = X`.

Since the switch clobbers the frame pair it saved, keep three words per task —
`(frame address, saved_bp, saved_pc)` — and write the saved pair back into the
incoming task's frame just before returning to it.

A complete, runnable proof of concept is in
[`src/tests/test_coop_switch.c`](../src/tests/test_coop_switch.c). It performs
repeated round-trip switches between two tasks plus `main`, using nothing but
`LEV`. Verified three ways, including as a compiled `.c4r` — i.e. c4lm's own
deployment path:

```
./c4m src/tests/test_coop_switch.c
./c4cc -o coop.c4r src/tests/test_coop_switch.c && ./c4m load-c4r.c -- coop.c4r
./c4 c4m.c load-c4r.c -- coop.c4r
```

```
main: switching to A
A: started with x=11 y=22
A: iteration 1
B: started with x=33 y=44
B: iteration 1
A: iteration 2
...
B: iteration 4
task_done: a task ran off the end. Exiting.
```

Note this needs `&function`, which plain `c4.c` rejects — but c4lm is *compiled*
by `c4cc` and only *run* under plain c4, where the address is already patched
into the image, so the constraint is satisfied.

**What this does and does not give you.** It gets c4lm cooperative multitasking
and process spawning, which is what the README says is missing. It cannot give
pre-emption: without c4m's cycle interrupt nothing can interrupt a task that
never calls `schedule()`. And a task blocked in `read()` still blocks the whole
world.

## 6.5 `c4l.c` — a `.c4r` loader for plain c4

The invoke-stub trick above became [`c4l.c`](../c4l.c), a ~150-line
loader in the strict c4 subset: `./c4 c4l.c program.c4r [args...]`
reads a modern `.c4r` (all four patch types, including the
data-resident `-3`/`-4`), runs constructors, enters `main` through the
stub, and runs destructors.

What it can run is bounded by plain c4's instruction set, which ends at
`EXIT`. An image runs only if its compiler emitted nothing above that: no
indirect call (`JSRS`), custom opcode (`OPCD`), `putchar` (`PUTC`), cycle
count (`C4CY`), jumptable `switch` (`JMPA`) or fused opcode. `scan_extended`
checks up front and refuses such an image by naming the instruction
("needs JMPA"), rather than letting c4 die later on a bare "unknown
instruction". Those images want c4m, which plain c4 can still provide:
`./c4 c4m.c load-c4r.c -- program.c4r`, one interpreter deeper.

`make test-c4l` pins that boundary: plain c4 runs `hello.c4r` through
`c4l.c`, refuses a `switch` image with "needs JMPA", and c4m runs the same
image through `load-c4r.c`.

**History (2026-08-03/04).** The first version (`5ba6241`) went further. It
taught `c4.c`'s VM to execute c4m's extended opcodes and turned c4m's
dispatch loop into a `switch`, which plain c4 cannot parse, so `run-c4` and
`test-c4` ran a compiled `c4m.c4r` through `c4l.c`. Nested interpretation got
1.79x faster (a 5M-iteration loop under the compiled c4m image: 74.4s to
41.6s). Both halves were reverted the next day in `29d6f57`:

* `c4.c` is back to unmodified. One of the added opcodes was a "clock" that
  advanced once per call, which is why C4KE's `top` under `make run-c4`
  redrew dozens of times a second.
* c4m dispatches through the original if-else chain again, so plain c4
  interprets `c4m.c` from source (`run-c4` is `./c4 c4m.c …` again). On the
  native build the switch gained nothing on innerbench (14.10s/13.55s
  if-else against 14.20s/14.21s switch); its only win was the nested case
  it broke.

Kept from that work: c4m's *compiler* still understands
`switch`/`case`/`default`/`break` (it compiles C that uses them, emitting a
jumptable dispatched with `JMPA`), and the two protected-mode fixes of
§7.4.

---

# Part 7 — Protected mode

Protected mode gives user tasks a real kernel boundary: a `PRIV_USER` task that
executes a syscall opcode traps into the kernel instead of calling libc directly,
so C4KE can route IO and allocation through its own layer.

## 7.1 How it is wired

| Layer | Mechanism |
|---|---|
| `c4m.c` | `mode` register (`MODE_UNPROTECTED`/`MODE_PROTECTED`). Each syscall opcode is guarded; in protected mode it raises `TRAP_PM_VIOLATION` instead of executing, and drops back to unprotected so the handler can run. `mode` is saved/restored by `TLEV` like any other register. |
| `c4ke.c` | Every task switch sets `mode` from `TASK_PRIVS` (`PRIV_KERNEL` → unprotected, `PRIV_USER` → protected). `trap_handler` forwards `TRAP_PM_VIOLATION` to `kernel_syscall_handler` via `__c4_jmp`. |
| `c4ke_pm.c` | Provides that handler. Either services the syscall immediately (`TASK_EXCLUSIVE`), or records it in the task's ext-data, parks the task in `WSTATE_SYSCALL`, and switches to a `kernel/io` dispatcher thread that performs it later and writes the result into `TASK_REG_A`. |

It is disabled at **three** independent points, all of which must be flipped:

1. `include/c4ke/config.h` — `#define CONFIG_ENABLE_PM 0` compiles the whole
   extension out. (Note `src/c4ke/include/config.h` is an identical stale copy;
   the build uses the one under `include/`.)
2. `c4ke_pm.c:pm_constructor` — `kernel_pm_support = 0; // TODO: was __c4_info() & C4I_PROT;`
3. `c4m.c` — every `if (mode == MODE_UNPROTECTED)` guard around the syscall
   opcodes is commented out, so `TRAP_PM_VIOLATION` can never fire.

Re-enabling (1) and (2) alone does nothing, because (3) means the trap is never
raised. Also note that `c4ke.c4r`'s make rule depends only on `$(C4CC)`, so
edits to the kernel or its headers do **not** trigger a rebuild — `rm c4ke.c4r`
first or you will keep running the old image.

## 7.2 The crash, and the fix

With all three enabled, `make test` died with `free(): invalid pointer`, and so
did something as small as `./c4m load-c4r.c -- c4ke.c4r hello`.

Tracing the VM showed the faulting `FREE` was executing inside `c4r_free()` in
`load-c4r.c`, freeing `c4r[C4R_HEADER]` — a field holding garbage because the
whole C4R structure had already been freed.

Root cause, in `kernel_clean_task` (`c4ke.c:1399`):

```c
if ((p = (int *)t[TASK_C4R])) c4r_free(p);
```

`start_task_builtin` gives every builtin task `t[TASK_C4R] = kernel_c4r` — the
kernel's *own* module, owned by the `loadc4r_run()` that started the kernel, not
by the task. So cleaning any builtin task frees the kernel's C4R, and when
`loadc4r_run()` later runs its own `c4r_free(module)` it double-frees.

This bug predates protected mode, but nothing triggered it: the only other
builtin task is `kernel/idle`, which is never cleaned. Protected mode adds
`kernel/io`, which *does* exit (on SIGTERM at shutdown) and therefore *is*
cleaned — taking the kernel's C4R with it.

Fix — only free a C4R the task actually owns:

```c
if ((p = (int *)t[TASK_C4R]) && p != kernel_c4r) c4r_free(p);
```

With that one line, protected mode survives `make test` (8 consecutive runs),
`make test-alt`, and `make test-massive` (20 concurrent processes), all with a
clean shutdown. `test_args`, `reverse` and `puts` return the same non-zero codes
with and without PM, and `test_signal` hangs either way — all pre-existing.

## 7.3 Still open

* **Cost.** The `kernel/io` dispatcher is a `schedule()` spin loop, so it burns a
  scheduling slot continuously; in `make test` the kernel's share of cycles went
  from ~72% to ~85%. Parking it on `await_message()` and waking it when a syscall
  is queued is the obvious improvement — the `sleep`/`await` calls are already
  there, commented out.
* **Coverage gaps.** `pm_dispatch_run` implements OPEN, READ, CLOS, PUTC, PRTF,
  MALC, FREE, INFO. `PUTS` is commented out, and `STRC` is looked up in
  `pm_start` but never handled, so an unhandled opcode reaching the dispatcher
  kills the task. The c4m guards for `STRC`, `EXIT` and `ITH` were therefore left
  disabled; `C4CF` must stay unguarded because C4KE calls it from protected mode
  by design.
* **`pm_start` frees `argv` outside the `if` that allocates it**, so a failed
  `malloc` would free an uninitialised pointer.

## 7.4 What C4IX does with the same hardware (2026-08-03)

C4IX (docs/c4ix-design.md) turns protected mode on at X2, and the
comparison is instructive because the VM support is identical — only
the kernel design differs.

* **The `EXIT` guard is now enabled** in `c4m.c`. It was commented out
  along with `STRC` and `ITH`; C4IX needs it, because a protected task
  calling `exit()` would otherwise halt the entire VM instead of
  ending one task. C4KE is unaffected (`CONFIG_ENABLE_PM 0` means it
  never runs protected) and `make test` confirms it.
* **No dispatcher thread.** C4KE parks the syscall in the task's
  ext-data and switches to a `kernel/io` thread that services it later
  — the design that costs it a scheduling slot and ~13% of cycles
  (§7.3). C4IX services the syscall inline in the trap handler and
  assigns the result to the handler's own `a` parameter, which `TLEV`
  loads. The handler is already the context-switch path, so a syscall
  that needs to block just parks the task in `TS_WAITING` and lets the
  switch at the end of the handler do its normal job.
* **`PRTF` is emulated rather than refused.** The trapped `returnpc`
  points at the `ADJ` that always follows `PRTF`, so `returnpc[1]` is
  the pushed-argument count — enough to reconstruct the call and route
  it through the kernel's own `write`. That is what lets an unmodified
  binary's `printf` be redirected, which was the whole point of Part 8.
* **Plain c4 gets a syscall layer too**, without any trap machinery:
  the loader writes `&sys_dispatch` into a `__c4ix_systable` global it
  finds in the image's symbol section, and the userland library calls
  through it. Same dispatcher, same numbers, different door. (Removed
  in 2026-10 with the rest of the plain-c4 path, which no host reached:
  plain c4 runs C4IX only through c4m.c. docs/c4ix-cpu.md, step 10.)

### A c4m fix this uncovered: `TRAP_ILLOP` must drop to unprotected

Every trap site in `c4m.c` sets `mode = MODE_UNPROTECTED` before
running the handler — except the `default:` case that raises
`TRAP_ILLOP`. That is the one C4KE and C4IX both use for
custom-opcode syscalls.

It never mattered for C4KE, which compiles protected mode out
(`CONFIG_ENABLE_PM 0`), so `mode` is already unprotected. C4IX runs
user tasks protected for real, and the consequence is vicious: a
syscall from a protected task enters the kernel's trap handler
**still protected**, so the first `PUTC` or `MALC` inside the kernel
raises a *second* trap on top of the first. The handler is not
re-entrant — its scheduler state lives in globals — so the nested
invocation corrupts the switch the outer one was performing. The
symptoms are all downstream: interleaved kernel output, a kernel task
apparently "running protected", tasks resuming with garbage program
counters, and a sensitivity to timing so acute that adding one
`printf` moves or hides the failure.

`mode = MODE_UNPROTECTED` after the `trap()` call fixes it, and
matches every other site. C4KE is unaffected — verified by `make
test`. C4IX additionally keeps a re-entry guard in its handler
(`if (sched_intrap) return;`), because neither the `TRAP_ILLOP` site
nor `_TRP` disables the cycle interrupt, so a hard IRQ can still land
in the window before the handler masks it.

### Since (added 2026-09-30)

This section describes C4IX as of X2–X3 in August 2026 and is kept as
that record. C4IX has grown a good deal since — built inside C4KE on
the machine that runs it, `SYS_SLEEP`, a lazy disk, compilers running
inside it, a Windows 95-style desktop with a terminal emulator, and
since then a root archive it rebuilds itself from, shared libraries,
CPU accounting and an idle task; the plain-c4 door described above is
gone.
The "Since X6" list at the end of section 6 of
[c4ix-design.md](c4ix-design.md) names each change and its commit, and
[c4ix-desktop.md](c4ix-desktop.md) is the desktop's design and tracker.

---

# Part 8 — The printf family (redirection groundwork)

C4 renders formatted output with the **PRTF opcode**, which writes straight to
the host's stdout. Nothing in the program can see or divert those bytes, so
redirection (`>`) and pipes (`|`) are impossible. Breaking that coupling means
formatting into a buffer first — which is what `vsnprintf` is for.

## 8.1 Why the old attempt failed

Two independent reasons, both now fixed.

**1. `size_t` silently became two parameters.** `src/c4lm/include/stddef.h`
defined `size_t` as `INT_TYPE`, i.e. `long`. C4CC has no typedefs and no `long`
keyword, and its parameter parser treats any unknown identifier as a parameter
*name*:

```c
static int vsnprintf (char *str, size_t size, char *format, va_list args)
// after cpp:      (char *str, long size, char *format, int *args)
// c4cc sees:       str, long, size, format, args   -- FIVE parameters
```

Hence `WARNING argument count mismatch in call to 'vsnprintf', expected 5
arguments, 4 given`, and — worse than the warning — `format` and `args` inside
the function referred to the wrong stack slots, so it could never have worked.
Under C4CC these types must expand to `int`.

**2. The wrappers had never been compiled.** They sat behind
`STDLIB_EXPERIMENTAL` / `EXPERIMENTAL_STDIO`, which nothing defined, so several
straightforward errors went unnoticed: `vprintf` referenced an undeclared `ap`
instead of `args`; `#define vprintf(f,ap)` expanded to `format` rather than its
own parameter `f`; `vfprintf` passed uninitialised `dest`/`stream_avail` to
`vsnprintf`; and `sprintf` carried a bogus `size` parameter.

## 8.2 What is there now

`src/c4lm/include/stdio.h` implements the family on a single formatting core:

```
vsnprintf  <- the only formatter; everything else calls it
  vsprintf, snprintf, sprintf          produce a buffer
  vfprintf <- measures, sizes a buffer, formats, then calls
    __c4_stdio_write(stream, text, len)     <- the one place bytes leave
      fprintf, vprintf, printf
```

`vsnprintf` supports `%d %i %u %x %X %o %c %s %p %%`, the flags `-` `0` `+`
`' '` `#`, field widths, `.precision`, and `*` for either. Length modifiers
(`l`, `ll`, `h`, `hh`, `z`, `t`, `j`) are parsed and ignored, since every C4
value is one word. No floating point — C4 has no float type. It follows C99:
at most `size-1` characters plus a nul, returning the length the result *would*
have had, so `vsnprintf(0, 0, fmt, args)` measures.

Two details worth knowing, both forced by C4 having **no unsigned type**:

* `%x`/`%o` mask off the sign extension after each shift
  (`v = (v >> 4) & (smask >> 3)`), so negative values print their true bit
  pattern.
* `%u` divides by 10 using a logical shift plus a remainder fix-up, and the
  signed `%d` path never negates — so `INT_MIN` converts correctly.

## 8.3 Redirection

`__c4_stdio_write()` is the single choke point. It honours a capture buffer:

```c
cap = malloc(256);
__c4_stdio_capture_start(cap, 256);
fprintf(stdout, "redirected %s #%d", "output", 1);
n = __c4_stdio_capture_stop();      // cap now holds the bytes
```

Capture is plain data rather than a callback on purpose: calling through a
function pointer needs `JSRI`, which plain C4 does not have. An OS with real
streams replaces the body of `__c4_stdio_write` and everything above it follows.

`printf` itself is only replaced when `C4_PRINTF_OVERRIDE` is defined. That is
opt-in because it changes every `printf` in the program — gaining redirectability
and losing PRTF's speed and its 7-argument cap. The override works because C4CC
lets a user function shadow a builtin, and `__c4_stdio_write` is *defined before*
the shadowing declaration, so its own `printf("%s", text)` still binds to the
builtin opcode instead of recursing.

## 8.4 Testing

`src/tests/test_vprintf.c`, 31 assertions (32 with the override). Expected
values were cross-checked against glibc's `snprintf` so the tests are not merely
agreeing with the implementation.

```
gcc -E -P -Isrc/c4lm/include -DC4CC=1 -D__c4__=1 -D__c4cc__=1 \
    src/tests/test_vprintf.c | ./c4cc -o test_vprintf.c4r -
./c4m load-c4r.c -- test_vprintf.c4r
```

Passes under `c4m` and under **plain `c4`** via c4lm's `boot.c`, with and
without the override — confirming the generated code uses only original-C4
opcodes. Clean under valgrind (no errors, no leaks).

```
gcc -E -P -Isrc/c4lm/include -I. -D__c4cc__=1 -DPURE_C4=1 \
    src/c4lm/load-c4r.c > boot.c
./c4 boot.c test_vprintf.c4r
```

## 8.5 Not done

* **C4KE's copy is still a stub.** `include/stdio.h` has the same
  `vsnprintf` placeholder plus its own bugs (`fclose` is missing a closing
  brace) behind `STDLIB_EXPERIMENTAL`. The implementation here drops in, but
  C4KE also wants `__c4ke_stream_dest`/`OP_STREAM_DEST` wired up, which is a
  separate piece of work.
* **Three copies of the c4lm headers** exist — `c4lm/include/`,
  `c4lm/src/include/` and `src/c4lm/include/` — and are byte-identical. All
  three were updated together; picking one and deleting the others would remove
  a standing trap.
* No `%f`, and `%n` is deliberately unimplemented.

## 8.6 Building c4lm

The README's command line is stale (`src/load-c4r.c` does not exist). What
works:

```sh
# the loader, for plain C4. Note -P: 'gcc -E -C' keeps /* */ comments, which
# C4's lexer cannot parse -- it only understands //. Without -P the very first
# licence comment in stdc-predef.h fails with "7: bad global declaration".
gcc -E -P -Isrc/c4lm/include -Isrc/c4lm -I. -D__c4cc__=1 -DPURE_C4=1 \
    src/c4lm/load-c4r.c > boot.c

# the kernel
gcc -E -P -Isrc/c4lm/include -Isrc/c4lm -I. -DC4CC=1 -D__c4__=1 -D__c4cc__=1 \
    src/c4lm/c4lm.c | ./c4cc -o c4lm.c4r -

./c4 boot.c c4lm.c4r
```

---

# Part 9 — Floating point in pure C4

C4 has no float type, and `c4m`'s native `FLTI_*` instructions only work when
c4m is compiled natively — under plain c4 the `FLT` opcode traps. So there was
no way to do floating point in C4 code itself.

(Since 2026-10, c4lc — and so c4sc and C4IX's `cc` — has real `float` and
`double`: IEEE binary32 and binary64 in software, correctly rounded with
subnormals, compiled to calls into `src/c4fp/softfp.c`. That is the route
for new code; [c4-float.md](c4-float.md) tracks it. This part describes the
older `c4_float.h` library, which needs no compiler support and still
serves code that c4cc or plain c4 compiles.)

[`include/c4_float.h`](../include/c4_float.h) implements IEEE-754 **binary32**
entirely in integer arithmetic. A value is carried in an ordinary `int` as its
32-bit bit pattern, which is the same representation `c4m_float.c` uses, so the
library is also a reference for what the native instructions should compute.

```c
int x, y;
x = f32_from_string("3.75", 0);
y = f32_from_string("1.5", 0);
f32_to_string(f32_div(x, y), buf, 4);   // "2.5000"
```

## 9.1 Why binary32

A C4 word is 64 bits. Two 24-bit significands multiply to 48 bits, which fits
with room to spare for guard bits, and division can scale the numerator by
2^27 without overflow. binary64 would need 106-bit intermediates — 128-bit
multiply emulation — which is a different project.

## 9.2 How it works

Each operation unpacks sign/exponent/significand, restores the hidden bit, and
works on a **27-bit significand**: 24 value bits plus guard, round and sticky.
`f32_round27` then rounds to nearest, ties to even, and `f32_finish` handles
rounding overflow and over/underflow.

The pieces that are specific to C4 having no unsigned type:

* `f32_shr_sticky` ORs everything shifted out into bit 0, so the sticky bit
  survives alignment.
* Bit-pattern comparisons mask with `0x7FFFFFFF` rather than relying on
  unsigned ordering.
* All the shifts that would sign-extend are masked.

The two normalisation shifts are the easy things to get wrong, and both were
wrong first time:

* **Multiply**: the product of two 24-bit significands lands in
  `[2^46, 2^48)`, so it reduces to the 27-bit form by shifting right 20 (or 21
  when the leading bit is at 47). Using 19/20 makes every product come out
  twice too large.
* **Divide**: scaling by `2^27` puts the quotient in `(2^26, 2^28)`. A quotient
  `>= 2^27` means the result is `>= 1.0` and shifts down by one; anything
  smaller only decrements the exponent. Incrementing the exponent instead
  doubles every quotient.

## 9.3 What it supports

| | |
|---|---|
| Arithmetic | `f32_add` `f32_sub` `f32_mul` `f32_div`, correctly rounded |
| Sign | `f32_neg` `f32_abs` |
| Compare | `f32_cmp` (returns 2 for unordered), `f32_eq/lt/le/gt/ge` |
| Convert | `f32_from_int` `f32_to_int` (truncates toward zero) |
| Text | `f32_from_string` (decimal, with exponent), `f32_to_string` (`%f` style) |
| Classify | `f32_is_nan` `f32_is_inf` `f32_is_zero` |

Signed zero, infinity and NaN follow the IEEE rules — `inf + -inf`, `inf * 0`,
`0 / 0` produce NaN; `x / 0` produces a signed infinity; `-0 + -0` stays `-0`.

**Subnormals flush to zero.** That is the library's one deliberate departure
from IEEE-754. NaN payloads and sign are not preserved either; a canonical
quiet NaN is produced, which is why the tests treat any NaN as matching any
other.

## 9.4 Testing

[`src/tests/test_float.c`](../src/tests/test_float.c) runs **3365 cases**, all
compared bit-for-bit against native C `float` arithmetic. The expected values
are generated by [`src/tests/genfloat.c`](../src/tests/genfloat.c), a native
tool, into `float_cases.h`:

```sh
gcc -O2 -o genfloat src/tests/genfloat.c && ./genfloat > src/tests/float_cases.h
```

Cases are carried as a compact data string rather than as generated calls —
3300 emitted call sequences overflow c4cc's 256 KB text pool, while the same
data as text is 112 KB in the data pool.

Passes under `c4m`, under **plain `c4`**, and as a C4KE process; clean under
valgrind.

## 9.5 Merging into c4m

The library is self-contained, so it can stay a library. If it is worth having
as instructions, `c4_float_instruction()` in `c4m_float.c` is the place: the
`FLTI_*` cases would call these functions instead of using the host FPU, which
would make floating point work under plain c4 and give identical results on
every host. Note that `include/c4m_float.h`'s 64-bit constants are currently
wrong — they carry a `0xcf8ece20` prefix in the high word, left over from
reinterpreting a 32-bit float through a 64-bit union.

An FPU opcode that c4lc's soft-float helpers use when the host has one is
step F3 of [c4-float.md](c4-float.md). A c4m built for the VM (as `/bin/c4m`
in C4IX) already defines `c4_float_instruction` as a macro that lends its
guest the outer machine's `FLT`.
