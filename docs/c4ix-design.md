# C4IX — a Linux-like operating system, compiled by c4lc

C4IX is a ground-up successor to C4KE: written against c4lc's extended
language (structs and friends), built as separately compiled objects
linked by c4rlink, optimized end to end by `c4lc -O`, targeting c4m
first with a degraded-but-working plain-c4 mode via the c4l loader.
The name is the user's, from an earlier rewrite attempt.

Everything here builds on docs/c4lc-design.md (the compiler) and
docs/internals.md §6.5 (the c4l loading system).

## 1. Why a rewrite

C4KE is written in the bare c4 subset: every kernel structure is an
int array indexed by enum offsets (`task[TASK_NICE]`), IO goes
straight to the host (PRTF prints to stdout, unredirectable), and the
whole kernel is one 3300-line translation unit compiled in one shot.
Each of those is a language problem, not an OS problem. Fix the
language, and the OS gets to be ordinary readable code.

## 2. The language: c4lc L7

The compiler grows past the c4 subset. c4cc stays where it is — the
oracle for the old subset — and gcc becomes the differential oracle
for everything new (same source, same output, both compilers).

### 2.1 Structs, unions, typedef

- `struct T { ... };`, `union T { ... };`, `typedef`, member access
  via `.` and `->`, `sizeof(struct T)` and sizeof of typedef names,
  pointers to structs (including in casts), arrays of structs,
  struct-typed globals and locals.
- NOT in L7: struct-by-value parameters, returns, or whole-struct
  assignment (use pointers and memcpy — kernels do anyway); bitfields;
  nested struct *values* as members (struct pointers as members are
  fine); multi-dimensional arrays.

**Type encoding** stays integer, extending c4's: `CHAR=0, INT=1`,
`+2` per pointer level. Struct types live at `1024 + 64*id +
2*ptrlevel` — 64 per struct leaves 31 pointer levels, and `ty >= 1024`
is the "is a struct type" test. The parser owns the struct registry
and typedef table (it must, to resolve `struct T` and typedef names
while parsing — the classic lexer-hack, solved by construction).

**Layout**: every member starts word-aligned; char array members
occupy `(n+7)/8` words; `sizeof(struct)` is a word multiple. Simple,
predictable, and never compared against gcc's layout — the oracle
compares program OUTPUT, not offsets.

**Semantics changes in codegen**: pointer arithmetic and indexing
scale by the ELEMENT SIZE (struct pointers step by the struct size —
real C semantics, verified by the gcc oracle; int*/char* keep their
old 8/1). A struct-typed name evaluates to its address, like arrays;
loading or assigning a whole struct is an error.

### 2.2 C99-flavored conveniences

- `do { } while (e);`
- Compound assignment `+= -= *= /= %= &= |= ^= <<= >>=`, desugared by
  the parser to `L = L op R`. Caveat, documented: the lvalue is
  evaluated twice, so `a[i++] += x` is wrong — the linter rule is the
  same one kernels already follow.
- Declarations anywhere in a block, with block scoping and
  NON-CONSTANT initializers (`int x = f(a);`). The AST makes this
  cheap: a pre-pass over the function body assigns every declaration
  a frame slot so ENT still reserves the whole frame up front;
  initializers become ordinary assignments at the declaration point;
  the symbol list save/restore at block boundaries gives scoping.
- `continue` (already works), `for` (already works, better than c4cc).

## 3. The toolchain: c4lc L8

- `c4lc -c file.c -o file.c4o`: compile one unit, leaving undefined
  externs as SYMBOL-typed patches (positive patch types) instead of
  exit-stubs, with a full symbol table (defined + extern entries).
  c4rlink already merges objects, resolves symbols, rebases segments,
  and has `-r` library mode for `.c4l` archives — it was built for
  this.
- Build shape: `c4lc -O -c` each module, `c4rlink` the objects plus
  `libc4ix.c4l` into the kernel image and into each userland binary.
- Full optimization everywhere: tree passes per module; the peephole
  passes run per module (post-link peephole is a later option).

## 4. The OS: C4IX

### 4.1 Principles

- **Syscalls own all IO.** Userland never executes PRTF/OPEN/etc
  directly: binaries run in protected mode, the VM traps privileged
  opcodes into the kernel (the mechanism C4KE proved), and userland's
  printf is a LIBRARY over `write(fd, buf, len)`. That single decision
  is what makes redirection, pipes, and ttys possible.
- **Everything is a struct.** Tasks, fds, vnodes, wait queues:
  ordinary c4lc structs.
- **Degraded plain-c4 mode.** Plain c4 has no traps and no cycle
  interrupt, so under `./c4 c4l.c c4ix.c4r`: cooperative scheduling
  (explicit yield), no protected mode, direct IO. The kernel detects
  the host via `__c4_info()` (0 under plain c4) and configures itself.
  c4m first; c4 is a supported degradation, not the design center.

### 4.2 Milestones

- **X0 — boot.** Kernel of 3+ objects linked by c4rlink, boots on c4m,
  prints via its own kprintf, runs one init task, clean shutdown.
  Also boots under `./c4 c4l.c` (degraded path proven early).
- **X1 — tasks.** Struct-based task table, scheduler (preemptive on
  c4m via cycle interrupt, cooperative fallback), fork-less spawn from
  .c4r images (loader in-kernel), wait/exit.
- **X2 — syscalls.** Protected mode on, trap-based syscall layer:
  write/read/open/close/spawn/wait/exit/yield/sbrk. Userland libc4ix
  (printf over write) as a `.c4l` library.
- **X3 — IO.** Per-task fd table, vnode layer: console, RAM files,
  pipes. `dup2`, and with it redirection.
- **X4 — shell.** c4ix-sh: argv parsing, `>` `<` `|` `&`, builtin cd/
  jobs enough to demo `cat file | wc > out`.
- **X5 — polish.** The C4KE test/bench suite ported; innerbench
  running under C4IX; boot-cycle and workload comparisons vs C4KE.

### 4.3 What gets reused

- c4m unchanged (traps, cycle interrupt, protected mode are enough).
- load-c4r's format knowledge; the in-kernel loader is new code
  against the same .c4r format.
- C4KE's hard-won lessons (documented in internals.md): trap design,
  the idle-task discovery dance (C4IX will pass the idle address
  explicitly instead), RAM-FS opcode interface if kexts are wanted.

## 5. Testing

- L7: gcc differential on every language feature (c4cc cannot compile
  structs; gcc can). The battery pattern from c4lc L3.
- L8: link a multi-object program, byte-compare behavior against the
  same source compiled whole; c4rlink's existing test-link extended
  with c4lc objects.
- C4IX: boot pins like test-c4lc's kernel pin; per-milestone
  functional tests (X3: redirect a program's output into a RAM file
  and read it back; X4: pipe two programs).

## 6. Status

- [x] L7 structs/typedef/unions + C99 conveniences (2026-08-03: full
      feature set gcc-differential-identical, plain and -O, pinned by
      src/tests/c4lc_l7.c in test-c4lc; struct parameters remain
      pointer-only, whole-struct assignment intentionally rejected)
- [ ] L8 objects + linking
- [ ] X0 boot
