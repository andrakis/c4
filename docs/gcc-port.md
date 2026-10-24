# gcc for c4 — is porting gcc a better road than growing c4lc?

The question (2026-10-24): rather than keep growing c4lc one port at a
time, upgrade it until it can compile an old, C-only gcc, give that gcc a
backend that emits c4 VM code (`.c4r`), and use it as the native
compiler. The theory is that it would be faster and more complete than
c4lc, and make games and applications easier to port.

This file is the investigation, written before anything is built. Nothing
was downloaded and no compiler was changed. The numbers marked
*measured* were taken on helios for this file (§4.1). Facts about gcc
and the other compilers come from their release pages and sources online,
cited where they are used.

## The answer, short

1. **c4lc never needs to compile gcc.** gcc is built as a cross compiler on
   the Linux host first (x86 → c4), with the host's own gcc. That cross
   compiler then compiles gcc's sources into a `.c4r` for C4IX (a
   "Canadian cross"). So the "upgrade c4lc until it compiles gcc" step is
   not on the path. If it were, it would need bit-fields, `offsetof` in a
   static initializer and about a gigabyte of host memory per large unit
   (§3).
2. **gcc would not be much faster than today's `cc` inside C4IX.** C4IX's
   `cc` already runs **c4sc**, which is c4lc compiled to C4 code, not c4lc
   interpreted on c4sp. Measured on the same Doom unit (§4.1), a modern
   gcc does about **a third** of c4lc's work at `-O0`, **0.85×** at `-O1`
   and **1.5×** at `-O2`. "c4lc is slow" is true of c4lc on c4sp, which
   is 16-44× slower than c4sc (c4sc-design.md, M0). It is not true of what
   C4IX runs.
3. **gcc would be more complete**: gcc 3.4.6 has C99 bar a few corners,
   with GNU C on top. But its backend is the hard part. RTL wants
   registers, and the c4 VM has one, `a`. The precedents work, with
   16-32 fake registers kept in frame slots: picoJava and ZPU, each about
   3,600-3,800 lines (§2.3). Their record is mixed: both picoJava and
   m68hc11 were dropped from gcc, and gcc 3.4 has been frozen since 2006.
4. **Recommendation (§6):** for a faster, more complete compiler, the
   better first step is **TinyCC**, not gcc. It is LGPL, has full C99 bar
   complex numbers, compiles itself, and is several times faster than gcc
   `-O0`. Its backend interface is about 1,300 lines on i386, and it once
   had a stack-machine backend of its own (`il-gen.c`). Keep c4lc as the
   house compiler. Hold gcc 3.4.6 back as an optional host-side
   optimising cross compiler, and start it only if a measurement shows
   that the quality of the generated code is what limits the ports.

## 1. Which gcc

gcc has been written in C++ since 4.8 ("GCC now uses C++ as its
implementation language", https://gcc.gnu.org/gcc-4.8/changes.html), so the
candidates are the last C releases. Dates are from
https://gcc.gnu.org/releases.html. Sizes are from the directory listings at
`https://ftp.gnu.org/gnu/gcc/gcc-X/`, as rounded by the OCF mirror
(https://mirrors.ocf.berkeley.edu/gnu/gcc/). The exact byte counts are in
each directory's `.sig` files, or in the `sha512.sum` files on
https://sourceware.org/pub/gcc/releases/, to be taken when a download is
approved.

| | 2.95.3 | 3.4.6 | 4.2.1 | 4.7.4 |
|---|---|---|---|---|
| released | 2001-03-16 | 2006-03-06 | 2007-07-18 | 2014-06-12 |
| what to fetch | `gcc-core-2.95.3.tar.gz`, 8.5 MiB (no `.bz2`; the full tree is `gcc-everything-2.95.3.tar.gz`, 12.3 MiB) | `gcc-core-3.4.6.tar.bz2`, 12.7 MiB (full `gcc-3.4.6.tar.bz2` 26.9 MiB; testsuite 2.5 MiB) | `gcc-core-4.2.1.tar.bz2`, 18.2 MiB (full 42.0 MiB) | `gcc-4.7.4.tar.bz2`, 79.1 MiB. There is no core split: the last one is 4.6.4 |
| licence | GPLv2+ | GPLv2+ | GPLv2+ (the last; 4.2.2 moved to GPLv3+) | GPLv3+ with the GCC Runtime Library Exception 3.1 |
| host compiler | K&R C was enough | ISO C90: "K&R C compilers will not work" (https://gcc.gnu.org/gcc-3.4/changes.html) | ISO C90 | ISO C90 for stage 1 and a cross build. Stages 2-3 of a bootstrap use C++ by default (`--enable-build-poststage1-with-cxx`) |
| host libraries | none | none | none for C (GMP/MPFR from 4.3: https://gcc.gnu.org/gcc-4.3/changes.html) | GMP ≥ 4.3.2, MPFR ≥ 2.4.2, MPC ≥ 0.8.1 linked into cc1 (4.7.4 `install.texi`) |
| C it accepts | C89 + GNU C; no mixed declarations, no `for (int …)`, no `_Bool` | C99 except a few corners (complex arithmetic, some VLA cases), + GNU C | C99, + GNU C | C99, much of C11 |
| optimiser | RTL only | RTL only, the last before tree-SSA (4.0) | tree-SSA + RTL | tree-SSA, GIMPLE, LTO, plugins |
| how big | gcc-2.96 is 984 kSLOC in all, 790k of it C (Wheeler, https://dwheeler.com/sloc/redhat71-v1/redhat71sloc.html) | not found; between the two either side | GCC 4.0.0 is 2.46 MSLOC, 1.08M of it C (https://gcc.gnu.org/ml/gcc/2005-04/msg01545.html) | larger again |

No source online gives a cc1-only line count for these releases. A
sloccount over `gcc/*.c` and `gcc/c-*.c` settles it once a tarball is
here. Mirrors: ftp.gnu.org and the GNU mirror network,
https://gcc.gnu.org/mirrors.html (13 mirrors in 8 countries), and
sourceware.org.

**What each needs from the host's libc** (from memory, to check against
the tarball): stdio with `vfprintf`, string, ctype, `malloc`/`realloc`,
`qsort`, `strtol`/`strtoul`, `getenv`, `atexit`. Obstacks, hash tables,
`xmalloc` and a portable C `alloca` come with libiberty. The libiberty
`alloca` is built on `xmalloc` and reclaims "eventually during subsequent
calls" (https://gcc.gnu.org/onlinedocs/libiberty/Functions.html), so no
compiler support is needed. 3.4's garbage collector (ggc-page) prefers
`mmap` and falls back to `malloc`. The driver `gcc` runs `cpp`/`cc1`/`as`/`ld`
through `pexecute`: `vfork`, `execvp` and `waitpid`. C4IX has `vfork`,
`execve` and `waitpid` since M7c, but not `execvp` (busybox-port.md §4.1).

**The pick, if gcc at all: 3.4.6.**
- 2.95.3 is smaller and its porting manual is simpler, but its C is too
  old for the ports. OpenTyrian declares in `for` and mixes declarations
  with code throughout (c4lc-design.md §14.11).
- 4.7.4 is three times the code, is GPLv3, needs GMP, MPFR and MPC built
  for the target as well, and takes tens of MB to compile a large unit.
  It brings nothing for a stack target: the hook a register-less target
  needs, `TARGET_NO_REGISTER_ALLOCATION` (nvptx), arrived in gcc 5, which
  is C++ (§2.3).
- 4.2.1 is the GPLv2 fallback if the licence ever matters. It is tree-SSA
  too, with 3.4's backend problems and 4.7's size.

## 2. The backend

### 2.1 What the machine offers

The c4 VM has one register, `a`, and `sp`, `bp` and `pc`. An operation
takes its left operand from the stack and its right one from `a`:
`a = *sp++ OP a`. The parts a backend uses:

| | does | gcc's view |
|---|---|---|
| `IMM n`, `LEA n` | `a = n`, `a = bp + n` | constants, frame addresses |
| `LI`, `LC`, `SI`, `SC` | load through `a`; store `a` through the address popped | `mem:SI`, `mem:QI` |
| `PSH` | `*--sp = a` | push |
| `ADD … MOD`, `EQ … GE`, `SHL`, `SHR` | `a = pop OP a` | arithmetic, `scc` |
| `JMP`, `BZ`, `BNZ` | jump, branch on `a` | `jump`, `cbranch` |
| `JSR`, `ENT n`, `LEV`, `ADJ n` | call, make a frame of n words, return, drop n words | `call`, prologue, epilogue, popping arguments |
| `LDL n`, `STL n`, `POPA` | `a = *(bp+n)`, `*(bp+n) = a`, `a = *sp++` | **a register in a frame slot**: one instruction to read it, one to write it |
| `MCPY`, `MSET`, the syscalls | block move, clear, `OPEN`, `READ` and the rest | `movmem`/`setmem`, builtins |

`LDL`, `STL` and `POPA` are the three fused opcodes that c4m runs too.
They exist "to treat the frame as registers" (fused-opcodes.md, "Why three
in c4m"). They are exactly what an RTL backend needs, and every machine
C4IX runs on has them: c4m, c4mp, c4bb, libjs and oisc4. Only plain c4
lacks them.

### 2.2 A machine description for c4

This is the picoJava and ZPU recipe (§2.3), fitted to `LDL`/`STL`.

- **Registers.** Hard register 0 is `A`, the accumulator, alone in a class
  of its own (constraint `a`). Then come 16-32 **soft registers** `r1…rN`,
  which are frame slots `bp-1 … bp-N` (constraint `r`), then `SP`, `BP` as
  the frame pointer, and a fake argument pointer eliminated to
  `BP + 2`. `ENT` reserves the slots with the locals.
- **The soft registers cost nothing to save.** Every frame has its own
  slots, so a callee's `r3` is not the caller's `r3`. Mark them all
  call-saved and have the prologue save none of them. gcc then keeps
  values in them across calls for free. The m68hc11's soft registers sat
  at fixed addresses in page 0 instead, and had to be saved and restored
  around calls and interrupts. Fixed addresses would also have to be
  saved by C4IX at every task switch.
- **A three-address insn** `r0 = r1 + r2` prints as `LDL r1; PSH; LDL r2;
  ADD; STL r0`. That is five instructions, about what c4lc emits for
  `x = y + z` with locals under `-mfuse`. When the result's only use is
  the next insn, `final` checks the `REG_DEAD` note, leaves the value in
  `a` and drops both the `STL` and the `LDL`. picoJava did the same for
  its outgoing-argument registers: "writing one emits nothing". This
  small "stackifier" is what decides code quality. LLVM's WebAssembly
  backend has a whole pass for the same job.
- **Memory operands** are an address in a soft register, or a frame or
  global address: `LDL p; LI`, or `LEA n; LI`. A store pushes the address
  first: `LDL p; PSH; LDL v; SI`.
- **Calls** must match c4lc's ABI, or libc4ix cannot be called (see
  below). Arguments are pushed left to right, so the first one sits
  highest; gcc lays this out with `ARGS_GROW_DOWNWARD`, as PA does. The
  call is `JSR f; ADJ n`, and the `call` pattern already carries the
  argument bytes as an operand. The result comes back in `A`. An
  indirect call is c4lc's sequence: return label, `PSH`, target in `a`,
  `JMPA` (c4lc-design.md, M7b).
- **Narrow and wide types.** Bytes go through `LC`/`SC`. Shorts, unsigned
  division and unsigned compares go through small helpers, as c4lc does
  them (§14.2). `DImode` and floating point go through libcalls: libgcc's
  `__divdi3` and friends, and soft-float `__adddf3` and friends, or the
  FPU opcode (c4-float.md F3).
- **The ABI to match** is c4lc's, so that gcc's objects link with
  libc4ix.c4l and the rest of C4IX:
  - argument order;
  - a `long long` is two words, high pushed first (§14.7);
  - a struct is returned through a hidden last argument (§14.11);
  - varargs follow C4IX's own convention: `va_start` reads a word below
    the last named parameter (`libc.h`, `__c4cc_make_va`,
    `__c4ix_uva_adj`).

  Getting this wrong fails silently, so the first tests are calls across
  the two compilers both ways.
- **Output.** gcc writes assembler text, and there is no c4 assembler in
  C today: c4cc's `asm-c4r.c` is a compiler driver. Two choices:
  - print c4lc's labelled instruction-list form and assemble it with
    `c4r.lisp`, which also brings c4opt's peephole and `-mfuse` for free;
  - write a `.c4o` writer in C, about 600-1,000 lines, against
    `c4r.lisp`'s format. Inside C4IX it has to be this one.

  The linker is c4rlink, which C4IX already has.

**Size.** The `.md`, `.h` and `.c` come to about 3,500-4,500 lines on the
pj/ZPU scale. The `.c4o` writer adds 1,000, and crt0, libgcc glue and the
configure fragments a few hundred more. That is **L–XL**: weeks to a first
"hello", and months to pass the torture suite. The known risk is
**reload**. An accumulator that every insn clobbers, plus addresses that
need a register, is how the m68hc11 got its "spill failure" reports. The
cure is to keep `A` out of the allocator except through explicit `a`
constraints, and to give reload enough soft registers.

The payoff is gcc's whole optimiser: CSE, combine, loop optimisation,
inlining and scheduling. Also its testsuite: `gcc.c-torture/execute`
runs on c4m as it stands.

### 2.3 Precedents

| port | gcc | machine | registers | size | what happened |
|---|---|---|---|---|---|
| picoJava `pj` (Chamberlain, 1999) | 2.95–3.2 | operand stack | 32 fake general registers, rewritten as offsets from `vars` (local slots); `FIRST_PSEUDO_REGISTER` 128 | pj.c 1,286 + pj.h 1,294 + pj.md 980 ≈ **3,560** | removed between 3.2.3 and 3.3 (https://github.com/gcc-mirror/gcc/tree/releases/gcc-3.2.3/gcc/config/pj) |
| ZPU (Zylin, Harboe) | 3.4.2 | stack | R0-R31, with R8-R31 reached by stack-relative `loadsp`/`storesp`; fake FP/AP eliminated | zpu.c 2,537 + zpu.h 739 + zpu.md 506 ≈ **3,780** | never upstream; "well tested"; code about 80% the size of Thumb-2 (https://github.com/zylin/zpugcc) |
| m68hc11/hc12 | to 4.6 | accumulator D, index X/Y | soft registers `_.d1`-`_.d32` in page 0, default 4 (https://www.gnu.org/software/m68hc11/m68hc11_gcc.html) | ≈ **15,100** in 3.4.6 | obsoleted in 4.6, removed 2012: no maintainer for 4 years, and a 2003 change had made its code "much larger and slower" (https://gcc.gnu.org/legacy-ml/gcc/2012-03/msg00338.html) |
| gcc-6502 | 6.0 trunk | A, X, Y | zero-page pseudo-registers r0-r7, s0-s7, e0-e31 | ≈ **6,550** | work in progress: 62,283 passes, 4,165 unexpected failures (2014; https://github.com/puppeh/gcc-6502) |
| nvptx (Schmidt, 2014) | 5+ | virtual ISA | pseudos left unallocated, through `TARGET_NO_REGISTER_ALLOCATION` | — | upstream. The hook is gcc 5, which is C++, so it is not available to 3.4 |
| moxie (Green) | 4.4+ | 16 real registers | — | ≈ **1,530** | upstream. It is the size of an ordinary small port, for scale |
| JVM: egcs-jvm (Waddington, ~1999) | egcs | stack | — | — | never upstreamed. RMS opposed it as a loophole for proprietary backends |
| NestedVM | 3.3.6 | MIPS → JVM by translating binaries | — | — | works, Apache 2.0. This is the translate-the-output route of §5 |
| wasm (Pip Cet, 2017; an RFC, 2026-05) | — | stack | — | RFC ≈ 2,800 | the 2017 port "makes some decisions that negatively affect performance"; the 2026 RFC was welcomed by the Steering Committee on 2026-06-15 |

The lesson: an old-gcc port to this VM is 3.5-4k lines on the pj/ZPU
model. It works, and it is never pretty. The ports that died died of
maintenance, not of being impossible. A c4 port would be the only user of
its gcc version, forever.

## 3. The c4lc gap — what compiling gcc with c4lc would take

This section is not on the recommended path (see the answer, point 1). It
is here because the question was asked, and because each item also gates
some program the ports might meet. Probes were run on helios against the
`smp` tip (`d4c7f93`), with c4lc on native c4sp:

| construct | where gcc needs it | c4lc today | size |
|---|---|---|---|
| **bit-fields** | `rtl.h`'s `rtx_def` (code : 16, mode : 8, and a dozen 1-bit flags); `tree.h`'s `tree_common` (some thirty flags). Every unit includes both | **rejected**: "expected semicolon after struct member" (probe; busybox-port.md §3.1 `bitfield`) | M: parse, layout as gcc's, read-modify-write |
| enum bit-fields (`ENUM_BITFIELD`) | the same structs | `system.h` makes them `unsigned int` unless `__GNUC__`, so they reduce to the row above | — |
| **`offsetof` in a static initializer** | `rtl.c`'s `rtx_size[]`: `RTX_HDR_SIZE + (sizeof FORMAT - 1) * sizeof (rtunion)`, with `RTX_HDR_SIZE` = `offsetof (struct rtx_def, u)` | **rejected**: `(int)&((struct hdr *)0)->u` is "not a constant" (probe). `sizeof (struct …)`, `sizeof "str"` and `?:` in the same initializer fold (probe) | S–M |
| X-macro `.def` files included many times | `rtl.def`, `tree.def`, `machmode.def`, `insn-*` | works (probe: an enum, a name table and a `sizeof FORMAT - 1` table from one `.def`) | — |
| K&R definitions with `PARAMS` | everywhere in 2.95; leftovers in 3.4 | works (M13; probe) | — |
| unions, struct assignment, struct by value, `goto`, function-pointer tables, aggregate initializers, big sparse `switch` | everywhere; `insn-output.c`, `lang_hooks` | done: M7b, M9, M10, M12 | — |
| `alloca` | 2.95 often, 3.4 less | `__builtin_alloca` is undefined (probe). libiberty's C `alloca` needs no compiler support | none (libiberty), or S for a real one |
| `setjmp`/`longjmp` | 2.95's float-trap handling in `toplev.c` (from memory: to check) | no `setjmp.h` in C4IX's libc (busybox-port.md §3.4). The VM has `__c4_jmp` and `__c4_adjust` | S–M, libc |
| `<stdarg.h>` | diagnostics (`error`, `warning`, `fatal`) | works, with C4IX's `va_list`. `va_arg(ap, double)` steps one word on a 32-bit host (session notes, ports scan), which gcc's diagnostics do not use | S to fix |
| `long long` / `HOST_WIDE_INT` | for a 32-bit target, a 32-bit `long` host is enough in 2.95/3.4 | `long long` is two words on 32-bit since M8. `%lld` does not print it yet | — |
| host `double` | 3.4's `real.c` is software. `timevar` and some heuristics use `double` | software `float`/`double` (c4-float.md F2) | — |
| GNU C in gcc's own source | none: it is behind `GCC_VERSION`/`__GNUC__`, since stage 1 had to build with vendor compilers | c4lc does not define `__GNUC__` | — |
| a configured `auto-host.h` | `configure` cannot run under c4 | write it by hand, as Busybox's `.config` is vendored (busybox-port.md §6) | S |
| **memory and time** | the largest units are `combine.c`-sized, plus 10k-odd preprocessed lines of headers each (estimate) | *measured*: c4lc on native c4sp compiled c4sc's generated `c4gen_gen.c` (17,116 lines) in **51 s using 1.1 GB**. The same compile segfaulted once while a second one ran beside it under `make -j2`, and passed alone | fits the host, hours for all of cc1; **never inside C4IX** (64 MB arena) |

So c4lc would need about four things to compile cc1 on the host: bit-fields, `offsetof`
constants, `setjmp`, and a hand-written `auto-host.h`. **That makes it the
cheapest of the known gaps, not the hard part.** The hard part is §2, and
§2 does not need c4lc.

**c4fc** cannot compile a line of gcc. It has no `typedef` at all
(ports-feasibility.md §4.1), and gcc is typedefs throughout, the same as
the games. Like the game work since M9, anything gcc-shaped is c4lc only.
The byte-identity rule stays for what c4fc can express (busybox-port.md
§8).

## 4. Running it inside C4IX

### 4.1 Measured: how much work a compile is

All of this was measured on helios on 2026-10-24.
- The x86 instruction counts are `valgrind --tool=lackey` (perf has no
  counters on this VM), net of each program's start-up.
- c4m's rate comes from a counting loop: 1.30e9 instructions in 3.49 s,
  **about 370M instructions a second**.
- `c4sc-host` is c4lc compiled to C by c4sc and then by `gcc -O2`.
  `c4sc.c4r` is the same C compiled by c4lc `-O`, so the two are one
  program on two machines.

| job | x86 instructions | c4 instructions | wall |
|---|---:|---:|---:|
| c4lc (`c4sc-host`), `sched.c` -O -c (1,592 lines; 1,922 preprocessed) | 554M | — | 0.29 s |
| c4lc (`c4sc.c4r` on c4m), the same | — | ≈ 1.5e9 | 4.05 s (+2.19 s to load the 3.5 MB image) |
| c4lc (`c4lc.lisp` on native c4sp), the same | — | — | 2.54 s |
| c4lc (`c4sc-host`), Doom's `p_enemy.c` -O -c (2,008 lines; 6,505 preprocessed with C4IX's headers) | **888M** | — | — |
| gcc 15.2 `cc1 -m32 -O0`, the same text | **296M** | — | — |
| gcc 15.2 `-O1` | 755M | — | — |
| gcc 15.2 `-O2` | **1,375M** | — | — |

The first two rows give the **expansion: one x86 instruction of `gcc -O2`
code is about 2.7 c4 instructions of c4lc `-O` code**, for the same
compiler doing the same job. Applied to the rest, with libjs at about
150M instructions a second (106-190M in libjs-design.md):

| compiler, on `p_enemy.c` inside C4IX | c4 instructions | c4m | libjs |
|---|---:|---:|---:|
| c4sc, today's `cc` | ≈ 2.4e9 | ≈ 6.5 s | ≈ 16 s |
| a gcc-15-sized gcc, `-O0` | ≈ 0.8e9 | ≈ 2.2 s | ≈ 5 s |
| a gcc-15-sized gcc, `-O2` | ≈ 3.7e9 | ≈ 10 s | ≈ 25 s |
| c4lc interpreted on c4sp | 16-44× c4sc's (c4sc-design.md M0) | minutes | minutes |

Two cautions, pulling opposite ways:
- gcc 3.4 does much less than gcc 15 (no tree-SSA), so its `-O2` could
  well cost half of gcc 15's or less. That is not measured: it needs a
  download.
- gcc's own code, compiled by a new soft-register backend, will at first
  be worse than c4lc's tree code, so the factor 2.7 is a floor.

Either way, **gcc inside C4IX is 2-4× faster than c4sc at `-O0` and about
level at `-O2`**. It is not the order of magnitude the theory hoped for.
For scale, the native-code road already measured: `cpp32` + `c4cc32`
compile C4KE's 5,513 lines in 0.18e9 instructions
(compiler-on-the-board.md). That is about 33k a line, against c4sc's
~800k a line on `sched.c`. A one-pass compiler written as C is the kind
that is fast here, and gcc is not one.

### 4.2 cpp, cc1, as and ld in C4IX

- **cpp** is inside cc1 from 3.3 on (cpplib), so it is not a separate
  image.
- **cc1.** 3.4's i386 cc1 is a few MB of x86 text (an estimate: no
  binary is here). The ratio busybox-port.md §8 measured, 3.2× from
  x86-32 text to a `.c4r`, puts a small-target cc1 at **8-11 MB**. That
  is over **C4IX's 4 MB image cap**, which still applies: `C4R_BUF_MAX`,
  `src/c4ix/loader.c:37`. Today's c4sc is 3.56 MB at 64 bits and 2.1 MB
  at 32.
  - Raising the cap is small: the loader already sizes its buffer from
    the file (c4ix-design.md, "Memory").
  - Its working memory is tens of MB on large units. The one figure
    found is 20-25 MB for `combine.c` at `-O0 -g` on 2007 mainline
    (https://gcc.gnu.org/pipermail/gcc-regression/2007-April/010120.html).
  - With a 10 MB image, that crowds the default 64 MB arena: run it with
    `?mem=128`.
- **as**: the `.c4o` writer of §2.2. Small.
- **ld**: c4rlink, already in C4IX.
- **The driver**: either C4IX's `cc` learns to run `cc1` (it already runs
  c4sc and c4rlink), or gcc's `gcc.c`, which needs `execvp` in libc.

So yes, it could run, after the cap is raised and with a bigger arena. It
would be the largest program C4IX has run, by a factor of five.

## 5. The alternatives

| compiler | licence | C | backend shape, and a c4 backend | size | in C4IX |
|---|---|---|---|---|---|
| **TinyCC** (https://github.com/TinyCC/tinycc) | LGPL 2.1 | C99 bar complex; many GNU extensions; K&R definitions; bit-fields; compiles itself (https://bellard.org/tcc/tcc-doc.html) | one pass over a value stack (`vtop`). A backend is `load`, `store`, `gen_opi`, `gfunc_call`, prologue and epilogue: `i386-gen.c` is 1,327 lines. **`il-gen.c` (657 lines) was a stack-machine backend** for .NET CIL, with three "registers" on the evaluation stack. It has rotted since 2003, but it shows the shape. A c4 backend is soft registers in frame slots as in §2.2, without reload to fight. **M** (1.5-2.5k lines + a `.c4o` writer) | `tccgen.c` 9k lines; the whole compiler is tens of thousands | small and fast. Its home page benchmarks it several times faster than gcc `-O0` (not measured here). Its image should fit under 4 MB |
| **chibicc** (https://github.com/rui314/chibicc) | MIT | most of C11; **no K&R definitions** (Rise of the Triad uses them), no complex, no inline asm | `codegen.c`, 1,595 lines, **is already an accumulator machine**: an expression goes to `%rax`, and operands are `push`ed and `pop`ped. A c4 backend is close to transliteration. **S–M** (~1k lines) | ~9k lines | never frees memory, which hurts in a 64 MB arena; no optimiser. slimcc (MIT, C23) is the maintained fork |
| **lcc** (https://github.com/drh/lcc) | not free for products: "You may not sell lcc or any product derived from it…" | C89 | **`bytecode.c` (282 lines) emits postfix stack code.** Quake III's QVM was "derived from the bytecode interpreter target of LCC" and maps nearly 1:1 onto c4 (https://icculus.org/~phaethon/q3mc/q3vm_specs.html). **S** | small | C89 only, and the licence |
| pcc | BSD | C99 since 2007; last release 2014 | register machine, x86 backends | medium | possible; little activity |
| cproc + QBE | ISC / MIT | C11, some C23; needs an outside preprocessor | QBE is SSA with a register allocator over a fixed register file. A stack VM fights it. **L** | ~13k (QBE) | — |
| gcc 3.4.6 (§2) | GPLv2+ | C99 + GNU C | RTL; soft registers; reload. **L–XL** | hundreds of thousands | 8-11 MB image; over the cap |
| clang → wasm → c4 (host only) | Apache 2.0 | C17/C23, `-O2` | wasm is a stack machine with locals, which map onto `a`/`PSH` and `LDL`/`STL`. But every pointer is an offset into linear memory: each load and store adds a base, and libc4ix's pointers are not wasm's, so a wasm libc comes along too. **M–L** | — | no compiler inside C4IX |

## 6. Recommendation

Two different things were hoped for from gcc. They have different
answers.

- **A faster compiler inside C4IX.** gcc gives 2-4× at `-O0` and nothing
  at `-O2` (§4.1), in an image over the cap. A one-pass compiler compiled
  to native VM code is what gives the order of magnitude: the
  `cpp`+`c4cc` row of §4.1 is that kind, at ~25× c4sc per line.
  **TinyCC** is that kind, and complete.
- **A more complete C, so ports stop finding compiler gaps.** M9 to M13
  were one port each. TinyCC's C99 + GNU C + K&R covers everything those
  ports had to add, and also what is still missing: bit-fields, `typeof`,
  statement expressions, VLAs, `case` ranges. gcc 3.4 covers a little
  more, at three times the backend effort, with reload as the risk, and
  as an orphaned fork. clang → wasm covers the most, on the host only.

So: **port TinyCC first. Keep c4lc as the house compiler. Hold gcc 3.4.6
back behind a measurement.** c4lc stays what builds C4IX, C4KE and the
Lisp, with c4sc, c4fc parity and the self-hosting story. It no longer has
to grow for every port, because a port that hits a gap goes through tcc.

### Milestones

| | what | done when | size |
|---|---|---|---|
| **T0** | Measure before building. Fetch the TinyCC release (asking first, with name, source and size). Build it natively on helios. Run it over Doom's units and C4IX's libc headers. Count its instructions per compile as in §4.1, and its peak memory | a table beside §4.1's: tcc against c4lc and gcc on the same units. Go/no-go | S (a day) |
| **T1** | A cross tcc on Linux (x86 → c4): `c4-gen.c` with soft registers in frame slots via `LDL`/`STL`/`POPA`, c4lc's ABI (§2.2's list), and a `.c4o` writer for c4rlink | c4lc's `src/tests/c4lc_*.c` battery passes against gcc's goldens on c4m and libjs, at 32 bits. Calls between tcc and c4lc objects work both ways, varargs included | M (2-3.5k lines) |
| **T2** | The ports through it: Wolf3D, Doom, OpenTyrian and Rise of the Triad built by tcc, against c4lc's builds | demo frames identical; fps and image sizes in a table | S–M |
| **T3** | tcc in C4IX: the cross tcc compiles tcc into `tcc.c4r`, and `cc -tcc` (or `tcc` itself) on the disk | `cc hello.c` and C4IX's own userland built inside C4IX by tcc; compile times against c4sc | S–M |
| **T4** | Code quality: run c4opt's peephole and `-mfuse` over tcc's output (it can emit c4lc's instruction-list form), or a `REG_DEAD`-style stackifier in `c4-gen.c` | raycast/Doom fps against c4lc `-O -mfuse` | M |
| G0 | *Only if T2/T4 show the generated code is the bottleneck:* fetch `gcc-core-3.4.6.tar.bz2` (12.7 MiB, asking first). sloccount cc1. Build a host x86 cc1 to size the image | cc1's lines, x86 text and peak memory on a large unit | S |
| G1-G4 | `config/c4/` on the pj/ZPU model (§2.2); libgcc glue; the `gcc.c-torture/execute` suite on c4m; the ports at `-O2` | torture pass rate; fps against tcc and c4lc | L–XL (4-6k lines, months) |
| G5 | Canadian cross: cc1 for C4IX | after raising `C4R_BUF_MAX`; with `?mem=128` | M |

## 7. Loose ends found on the way

- `make -j2 c4sc-host c4sc.c4r c4m` in a fresh worktree failed once. c4lc
  on c4sp segfaulted on `src/c4sc/c4gen_gen.c` while another compile ran
  beside it. Run alone, it passed: 51 s, 1.1 GB. That looks like memory
  pressure on helios (3.4 GB) and not a compiler bug, but it is not
  proven.
- `c4sc.c4r` (64-bit) is 3.56 MB, 0.44 MB under C4IX's 4 MB image cap.
  The next large addition to c4lc may push the 64-bit image over. The
  32-bit one, which C4IX actually runs, is 2.1 MB.
