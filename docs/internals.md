# C4 / C4M / C4KE internals

A working reference for the compiler and VM at the heart of this repo, and how
each layer above them extends it. Written from a full read of `c4.c`, `c4m.c`,
`src/c4ke/c4ke.c`, `src/c4cc/c4cc.c`, `load-c4r.c` and `src/c4lm/c4lm.c`.

Everything here was checked against the running code (`make c4 c4m c4cc`) rather
than inferred from comments.

## 0. The layer cake

```
c4.c        original interpreter: compiler + VM in one file, ~535 lines
 └ c4m.c    same compiler + VM, extra opcodes, traps, multi-file input.
            Deliberately written in the C4 subset so `./c4 c4m.c ...` works.
    └ load-c4r.c   loads a .c4r image into memory and calls it as a C function
       └ c4ke.c    pre-emptive multitasking kernel, built on c4m's traps
          └ u0.h   user-side runtime; turns kernel traps into C functions
c4cc.c      separate, richer compiler (for/continue/varargs/attributes),
            emits through a handler table -> asm-c4r.c (.c4r) or asm-js.c
```

Nesting really works: `./c4 c4m.c load-c4r.c -- c4ke.c4r` is c4 interpreting c4m
interpreting the kernel. It is slow but correct.

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

## 3.2 New opcodes

Appended after `EXIT` so the original numbering is untouched:

```
PUTC PUTS RALC MCPY STRC        putchar/puts/realloc/memcpy/stacktrace
ITH  _OPC _BLT _TRP OPCD        install_trap_handler, __opcode, __builtin,
                                __c4_trap, __c4_opcode
_JMP _ADJ C4CF C4CY TIME        __c4_jmp, __c4_adjust, __c4_configure,
                                __c4_cycles, __time
SIGH SIGI USLP INFO OPSL        signals, sigint, usleep, __c4_info, opcode list
C4IV                            __c4_invoke
FLT                             float (traps if unsupported)
JSRI JSRS JMPA TLEV DBG         indirect calls, jump-via-A, trap return, debug
```

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

Trap sources:

| Trap | Cause |
|---|---|
| `TRAP_ILLOP` | unknown opcode — the custom-opcode hook |
| `TRAP_HARD_IRQ` | cycle interrupt (`HIRQ_CYCLE`) |
| `TRAP_SOFT_IRQ` | `__c4_trap()` from user code |
| `TRAP_SIGNAL` | POSIX signal (native builds only) |
| `TRAP_SEGV`, `TRAP_OPV` | debugging aids |
| `TRAP_PM_VIOLATION` | syscall in protected mode — **see caveat below** |
| `TRAP_DEBUG` | `DBG` opcode |

## 3.4 The cycle interrupt

```c
__c4_configure(CONF_CYCLE_INTERRUPT_INTERVAL, n);
__c4_configure(CONF_CYCLE_INTERRUPT_HANDLER, (int)&handler);
```

The VM loop checks `if (interval && !(cycle % interval))` (`c4m.c:1314`) and
fires `TRAP_HARD_IRQ`. It then **sets the interval to 0**, so the handler must
re-arm it — which is exactly how C4KE's `critical_path_start/end` work
(setting the interval to 0 is the kernel's "disable interrupts").

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

1. **`realloc()` is broken under `c4m`.** `RALC` is in the builtin list, so
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

   The pointer is silently replaced by `32`. Either implement `RALC` or remove
   `realloc` from `c4m_builtins`.

2. **Protected mode is switched off in three independent places.** See
   [Part 7](#part-7--protected-mode) — it is now working again after one
   ownership bug was fixed.

3. `__builtin()` is a stub returning `-1` (`c4m.c:345`).

4. `-p` / `-P` pool-size flags are noted as non-functional in the header comment
   and do parse, but they run before `poolsz` is used, so they *do* take effect —
   the comment looks stale.

5. `print_stacktrace` finds functions by scanning the symbol table for a `Fun`
   whose `Val` matches a decreasing `pc`, bounded to 0xFF words; it gives up with
   "couldnt find function entry" on larger functions.

---

# Part 4 — C4KE, and why the trap design matters

`src/c4ke/c4ke.c` (~3650 lines) is a pre-emptive multitasking kernel. There is no
inner VM: **every task is ordinary C4 code running on the one c4m interpreter**.
A "context switch" is just a trap handler assigning new values to its own
`a/bp/sp/returnpc` parameters before returning through `TLEV`.

## 4.1 Task representation

A task is an `int` array indexed by the `TASK_*` enum (`c4ke.c:275`). The parts
that matter: `TASK_REG_A/BP/SP/PC` hold the saved registers, `TASK_STATE`
(`STATE_RUNNING`/`WAITING`/`ZOMBIE`…), `TASK_NICE` for scheduling, `TASK_BASE`
for the 64 KB stack, plus argv, signal handlers, mailbox and accounting.

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

`start_task_builtin` (`c4ke.c:1810`) allocates a 0xFFFF-byte stack and builds the
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

---

# Part 5 — `c4cc`, the enhanced compiler

`src/c4cc/c4cc.c` is a fork of the c4 front end with the back end abstracted
behind `c4cc_emithandlers[]`, an array of function pointers indexed by `EH_*`
(`c4cc.c:208`). `asm-c4r.c` and `asm-js.c` fill that table in; the parser never
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

Not yet present (the "more keyword support" gap): `struct`/`union`/`typedef`,
`do`/`switch`/`break`, `unsigned`/`short`/`long`, real arrays, and a
preprocessor — `gcc -E` is currently used as the preprocessor, as in
`gcc -E -DC4CC=1 -Iinclude src/tests/vararg.c | ./c4cc -o vararg.c4r -`.

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
