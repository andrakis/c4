# c4sp/lisp — c4lc, the `.c4r` tools, and sample programs

Everything here runs on c4sp (`../c4sp.c`). The files fall into three groups:
the C compiler c4lc, the `.c4r` codec and optimizer that c4lc shares with
other tools, and small sample programs that the c4sp tests run.

`load` resolves names relative to the loading file, so the drivers can be run
from the repo root, e.g. `./c4sp -R src/c4sp/lisp/c4lc.lisp -O in.c out.c4r`.
Flags for c4lc are listed in [../README.md](../README.md).

## c4lc, the C compiler

| file | what |
|---|---|
| [`c4lc.lisp`](c4lc.lisp) | driver: flags, then lex (or preprocess), parse, optional tree passes, generate, optional peephole, encode, write |
| [`c4lc-lex.lisp`](c4lc-lex.lisp) | tokenizer (L0); reproduces c4cc's lexer, with a pp mode for the preprocessor and a `-conforming` escape table |
| [`c4lc-pp.lisp`](c4lc-pp.lisp) | preprocessor (L9), token-based: `#include`, `#define`, `#if` family, stringize and paste |
| [`c4lc-parse.lisp`](c4lc-parse.lisp) | recursive-descent parser to an AST (L1, L7 structs, L11 enum constant expressions) |
| [`c4lc-tree.lisp`](c4lc-tree.lisp) | AST passes run by `-O` (L6): constant folding, dead branches, dead functions, inlining |
| [`c4lc-gen.lisp`](c4lc-gen.lisp) | code generator: AST to a labelled instruction module (L2, L3, L8 objects) |
| [`c4lc-tokens.lisp`](c4lc-tokens.lisp) | test driver: dump a file's tokens (`-count` for a total) |
| [`c4lc-ast.lisp`](c4lc-ast.lisp) | test driver: dump a file's AST |
| [`c4lc-ppdump.lisp`](c4lc-ppdump.lisp) | test driver: dump a preprocessed token stream, to compare with `gcc -E` |
| [`c4lc-eq.lisp`](c4lc-eq.lisp) | test driver: compile in memory and compare with a reference image (host independence, L5) |

## The `.c4r` codec and optimizer

| file | what |
|---|---|
| [`c4r.lisp`](c4r.lisp) | decode a `.c4r` into labelled instruction lists and encode it back; writes format v3 when asked |
| [`c4opt.lisp`](c4opt.lisp) | peephole passes over those lists, including the tail-call rewrite and the `-mfuse` pass |
| [`c4opt-run.lisp`](c4opt-run.lisp) | optimize an existing image: `c4sp c4opt-run.lisp [-mfuse] in.c4r [out.c4r]` |
| [`c4r-roundtrip.lisp`](c4r-roundtrip.lisp) | decode and re-encode an image; it must come out byte-identical: `c4sp c4r-roundtrip.lisp some.c4r [out.c4r]` |

The format is documented in [../../c4cc/README.md](../../c4cc/README.md).

## Sample programs

Run by `make test-c4sp`, with expected output in [`../tests/expected/`](../tests/expected/).

| file | what |
|---|---|
| `arguments.lisp` | prints `argv` |
| `callcc.lisp` | `call/cc`; CEK machine only (`-R` refuses it) |
| `deeprec.lisp` | non-tail recursion 4000 deep; only the CEK machine survives it under the C4 VM |
| `fac.lisp` | factorial, optionally of the first argument |
| `floats.lisp` | binary32 floats |
| `gcloop.lisp` | allocates millions of cells; the collector has to keep up |
| `listadd.lisp`, `quote.lisp`, `set.lisp` | small language checks |
| `macros.lisp`, `switch.lisp` | macros, and a `switch` macro built from them |
| `truthy.lisp` | what counts as false (`false`, `nil`/`()`, integer 0) |
| `seval.lisp` | a Lisp evaluator in Lisp; it can evaluate itself (`-s`, `-t`) |

## Contract for c4lc sources

c4sc ([../../c4sc/README.md](../../c4sc/README.md)) compiles the c4lc files
listed in its Makefile rules (`c4opt`, `c4lc-lex`, `c4lc-pp`, `c4lc-parse`,
`c4r`, `c4lc-tree`, `c4lc-gen`) to C. It only handles what those files use
today: named top-level functions, fixed arity, no closures, no higher-order
calls. A change to those files that uses more than that will build under c4sp
and break `make c4sc.c4r`.

The Makefile's `C4LC_LISP` dependency list does not include
`c4lc-pp.lisp`, so an edit to the preprocessor alone does not rebuild the
images that depend on c4lc.
