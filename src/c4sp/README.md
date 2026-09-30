# c4sp — a Lisp interpreter written in C4, and c4lc, the C compiler written in it

c4sp is a Lisp interpreter modelled on alisp, written in the C subset this
repo's compilers accept. The same source builds natively with gcc and as a
`.c4r` image that runs under c4m, inside C4KE, under C4DOS and on c4bb. It has
cons cells in a growing arena, a mark-and-sweep collector that scans the C4
stack conservatively, a CEK evaluator with `call/cc`, a recursive reference
evaluator (`-R`), binary32 floats, and a REPL.

Its main job in this repo is to run **c4lc**, a C compiler written in c4sp
Lisp ([`lisp/`](lisp/)). c4lc goes beyond c4cc: structs, unions, `typedef`,
`do`/`while`, compound assignment, block-scoped declarations, its own
preprocessor, object files for `c4rlink`, and tree and peephole optimization.
It builds C4IX, C4DOS, c4mp, c4or1k, c4tui and `c4sp.c4r` itself.

## Build

| target | produces |
|---|---|
| `make c4sp` | native interpreter (`gcc -O2`; safe because the collector spills registers with `setjmp` before scanning) |
| `make c4sp.c4r` | the image, compiled by c4lc `-O` after `gcc -E -DC4SP_DOS=1` |
| `make c4sp32` | native interpreter built with `gcc -m32`; c4lc run under it emits 32-bit images |
| `make c4sp32.c4r` | 32-bit image for c4bb |
| `make c4ke-lc.c4r`, `c4sp-lc.c4r`, `c4m-lc.c4r` | optional c4lc `-O` builds of the kernel, the interpreter and c4m; not part of the default build |

## Run

```
c4sp [-p] [-R] [-i] [-c ncells] [file.lisp] [args...]
```

| flag | effect |
|---|---|
| `-p` | parse only: print the canonical written form |
| `-R` | use the recursive reference evaluator instead of the CEK machine (no `call/cc`) |
| `-i` | interactive REPL, after running `file.lisp` if one is given |
| `-c ncells` | first block of the cell arena (default 65536; it grows) |

Arguments after the file are read as Lisp values and bound to `argv`.

```
./c4sp src/c4sp/lisp/fac.lisp 12
./c4m load-c4r.c -- c4sp.c4r src/c4sp/lisp/fac.lisp
./c4m load-c4r.c -- c4ke.c4r c4sp.c4r src/c4sp/lisp/fac.lisp
```

### c4lc

```
./c4sp -R src/c4sp/lisp/c4lc.lisp [flags] in.c out.c4r
```

| flag | effect |
|---|---|
| `-O` | tree passes (constant folding, dead branches, dead functions) then the c4opt peephole passes |
| `-c` | compile one unit to an object (`.c4o`); undefined prototypes become symbols for `c4rlink` |
| `-P` | run c4lc's own preprocessor instead of expecting `gcc -E` output |
| `-I dir`, `-D name` | include path and predefined macro; either implies `-P` |
| `-conforming` | real C escape sequences (`\t`, `\xHH`, `\NNN`) instead of c4cc's table |
| `-mcisc` | emit c4mp's fused array-element opcodes (`LXI`/`SXI`); the image runs only under c4mp |
| `-mfuse` | run c4opt's fuse pass ([fused-opcodes.md](../../docs/fused-opcodes.md)); the image does not run on plain c4. Implies `-O` |
| `-minline` | inline small leaf functions ([inline-small-functions.md](../../docs/inline-small-functions.md)). Implies `-O` |
| `-minline-why` | `-minline`, and report the decision for every function |

`-R` is how the Makefile runs c4lc (`C4SPLC := ./c4sp -R`): c4lc uses no
`call/cc`, and the recursive evaluator is 1.63x faster on the C4IX build. The
output is the same either way.

Multi-module programs are compiled one unit at a time with `-c` and then
linked with `c4rlink` (see [../c4cc/README.md](../c4cc/README.md)); this is
what the Makefile's `c4lc_compile_par` does for C4IX, c4or1k and c4mp. For
example, the C4IX kernel:

```
./c4sp -R src/c4sp/lisp/c4lc.lisp -O -c -I src/c4ix src/c4ix/boot.c .c4ix_boot.c4o
...                                            # one per module
./c4rlink .c4ix_boot.c4o ... -o c4ix.c4r
```

## Test

None of these is part of `make test`.

| target | checks |
|---|---|
| `make test-c4sp` | reader round trip, every sample against `tests/expected/`, native vs `c4sp.c4r` under c4m, CEK vs `-R`, floats, the REPL, c4sp inside C4KE, `.c4r` byte round trip |
| `make test-c4sp-opt` | c4opt: optimized images behave the same (factorial, c4cc itself, switch, globals, the tail-call pass) |
| `make test-c4sp-deep` | seval evaluating seval evaluating fac under c4m (about a minute) |
| `make test-c4lc` | c4lc stages L0–L11: token and AST goldens, the differential battery against c4cc, `-O`, structs, objects and linking, the preprocessor, bootstrap (c4lc compiles c4sp and C4KE), compiling inside C4KE. Slow |

`test-c4sp` uses `hello.c4r` and `test_float.c4r` from the repo root, which
the default `make` copies there.

## Layout

| path | what |
|---|---|
| [`c4sp.c`](c4sp.c) | driver: flags, REPL, `main` |
| [`include/cell.h`](include/cell.h) | cell layout and type tags |
| [`include/gc.h`](include/gc.h) | arena, allocator and collector |
| [`include/cells.h`](include/cells.h) | constructors, list helpers, environments |
| [`include/atoms.h`](include/atoms.h) | interned atoms |
| [`include/read.h`](include/read.h) | reader and printer |
| [`include/stdlib.h`](include/stdlib.h) | builtins |
| [`include/eval.h`](include/eval.h) | recursive evaluator (`-R`) |
| [`include/cek.h`](include/cek.h) | the CEK machine |
| [`lisp/`](lisp/) | c4lc, the `.c4r` codec, the optimizer, and sample programs — see [lisp/README.md](lisp/README.md) |
| [`tests/`](tests/) | expected outputs — see [tests/README.md](tests/README.md) |
| [`bench/`](bench/) | the M0 measurement for compiling the Lisp — see [bench/README.md](bench/README.md) |

## Hosts

- **Native** (`make c4sp`, `make c4sp32`): reads and writes host files.
- **Under c4m** (`./c4m load-c4r.c -- c4sp.c4r ...`): runs, but bare c4m has
  no write syscall, so c4lc can compile but not save its output there.
- **Inside C4KE**: `file:write` uses the kernel's RAM filesystem, so c4lc
  compiles inside the kernel and the kernel runs the result from memory
  (`test-c4lc`, L5).
- **Under C4DOS**: the image is built with `-DC4SP_DOS=1` and reads and writes
  the DOS RAM disk through the C4DOS API table.
- **c4bb**: `c4sp32.c4r` is the 32-bit image. The fast way to compile on the
  board is c4sc's compiled c4lc ([../c4sc/README.md](../c4sc/README.md)).
- **Word size**: c4lc emits images of the word size of the interpreter running
  it, so c4lc under `c4sp32` writes 32-bit images and under `c4sp` 64-bit ones.

## Limits and known issues

- c4lc's preprocessor rejects a function-like macro named without an argument
  list, and `include/c4.h` has one. Builds that include it (`c4sp.c4r`,
  `c4sc.c4r`, `c4th32.c4r`) still preprocess with `gcc -E` first. C4IX builds
  with `-P` and no gcc.
- Integer division stays integer even with float arguments in scope; write
  `(/ 10.0 4)` for a float result (`lisp/floats.lisp`).
- c4sp treats `()` and `nil` as the same value, so `arguments.lisp` prints
  `nil` where alisp prints `()` ([tests/README.md](tests/README.md)).
- The header comment of `c4sp.c` says the image is compiled by c4cc; the
  Makefile builds it with c4lc.

## Docs

- [c4sp-design.md](../../docs/c4sp-design.md): the interpreter. Drifted: it
  describes a single fixed arena, but the arena now grows, and it predates the
  register spill that makes the `-O2` native build safe (both in
  `include/gc.h`).
- [c4lc-design.md](../../docs/c4lc-design.md): the compiler, milestones L0–L11,
  measured results. c4lc images of c4ke, c4sp and c4m are 18–27% smaller than
  c4cc's.
- [fused-opcodes.md](../../docs/fused-opcodes.md) (`-mfuse`),
  [c4or1k-design.md](../../docs/c4or1k-design.md) (`-mcisc`, M12),
  [inline-small-functions.md](../../docs/inline-small-functions.md) (`-minline`),
  [compiler-speed.md](../../docs/compiler-speed.md) (making the C4IX build
  fast: c4sp's cheap wins and the `-R` switch).
