# c4fc — a C compiler written in Forth

c4fc compiles C to `.c4r` images, with c4lc's coverage (structs, unions,
`typedef`, a preprocessor, objects, `-O`), and its output is byte-identical to
c4lc's. It is written in c4th ([../c4th/README.md](../c4th/README.md)) as a
small table-driven vocabulary ([`dsl.f`](dsl.f)): each C construct is one
`NODE:` row plus one method per compiler phase.

It is not fast. What it gives is a C compiler anywhere c4th runs, without a
Lisp: c4th runs on c4m, C4KE, C4IX and C4DOS from one image, and on c4bb
as a 32-bit build.
Compiling the whole C4KE kernel with c4fc costs about 85.5 billion VM
instructions (about 69 minutes at c4bb's speed), against 0.18 billion for
`cpp` plus c4cc; one C4IX module (`vfs.c`, `-O -c`) took 5 min 29 s on the
board ([compiler-on-the-board.md](../../docs/compiler-on-the-board.md)).

## Build

There is no build step: c4fc is Forth source, loaded into c4th at every run.
The Makefile names the files in load order as `C4FC_ALL` (for the native
tests) and `C4FC_FORTH` (for the C4DOS disk):

```
src/c4th/forth/core.f src/c4th/forth/ext.f src/c4th/forth/locals.f src/c4th/forth/dos.f
src/c4fc/dsl.f src/c4fc/lex.f src/c4fc/pp.f src/c4fc/ast.f src/c4fc/types.f
src/c4fc/emit.f src/c4fc/tree.f src/c4fc/inline.f src/c4fc/gen.f
src/c4fc/parse.f src/c4fc/opt.f src/c4fc/c4fc.f
```

`make c4dos-c4fc` builds a C4DOS disk directory with `c4th32.c4r`, those
files concatenated into one `c4fc.f`, `cc.f`, `c4rlink.c4r` and a `hello.c`.

## Run

The compiler is the Forth word `C4FC ( a u -- )`, which compiles the named
file and writes the image to stdout. Options are words executed before it:

| word | effect |
|---|---|
| `C4FC-INIT` | set up the arena; needed before `-I` or `-D` |
| `-P` | preprocess with c4fc's own preprocessor (otherwise `#` lines are skipped, as for `gcc -E` output) |
| `S" dir" -I` | include path |
| `S" NAME=1" -D` | predefined macro |
| `-c` | compile one unit to an object for `c4rlink` |
| `S" out" -o` | write to a file instead of stdout |
| `1 OPTIMIZE !` | c4lc's `-O`: tree passes and peephole passes (there is no `-O` word) |
| `-mcisc` | c4mp's fused array opcodes; the image runs only under c4mp |
| `-mfuse` | fused opcodes; not runnable on plain c4. Implies optimization |
| `-minline` | inline small functions. Implies optimization |
| `1 CONFORMING !` | real C escape sequences, as c4lc's `-conforming` |

Examples, as the Makefile runs them:

```
./c4th $(C4FC_ALL) -e ': GO S" hello.c" C4FC ; GO' > hello.c4r
./c4th $(C4FC_ALL) -e ': GO C4FC-INIT -P S" src/c4fc/tests" -I S" src/c4fc/tests/spike9.c" C4FC ; GO' > spike9.c4r
```

The `: GO ... ; GO` wrapper is needed because `IF`/`THEN` and friends are
compile-only, so the options have to run inside a definition.

On C4DOS (the simulated c4bb): `make run-c4dos-c4fc`, then at the prompt

```
RUN c4th.c4r c4fc.f cc.f
```

[`../c4dos/fs/cc.f`](../c4dos/fs/cc.f) defines `CC ( src-a src-u out-a out-u -- )`
and `OBJ`, with optimization, `-P` and the usual `-D`s set, and a `MAIN` that compiles
`hello.c`. Edit its last lines to compile something else. The output goes to
the DOS RAM disk through `dos.f`.

## Test

Neither target is part of `make test`.

| target | checks |
|---|---|
| `make test-c4fc` | the DSL; the lexer and preprocessor against c4lc's token dumps and against `gcc -E`; whole programs byte-identical to c4lc, plain and optimized; c4fc compiles `c4th.c` byte-identical to c4lc and the result passes the Forth-2012 suite; C4DOS and C4KE compiled whole; `-mcisc`/`-mfuse` |
| `make test-c4dos-c4fc` | on the simulated c4bb: C4DOS loads c4fc, compiles `hello.c`, writes the image to the RAM disk and runs it (about half a minute on the dev box) |

## Layout

| file | what |
|---|---|
| [`dsl.f`](dsl.f) | the vocabulary: arena, vectors, tagged nodes, generics |
| [`lex.f`](lex.f) | tokenizer, mirroring c4lc-lex.lisp |
| [`pp.f`](pp.f) | preprocessor, token-based like c4lc's |
| [`ast.f`](ast.f) | node kinds (`NODE:` rows) and the phases over them |
| [`types.f`](types.f) | type encoding, the same integers c4lc uses |
| [`emit.f`](emit.f) | code, data, patch and symbol segments, and the `.c4r` writer |
| [`tree.f`](tree.f) | AST passes run by optimization (c4lc L6) |
| [`inline.f`](inline.f) | the inliner |
| [`gen.f`](gen.f) | code emission, one method per node kind |
| [`parse.f`](parse.f) | tokens to AST |
| [`opt.f`](opt.f) | peephole passes, matching c4opt.lisp |
| [`c4fc.f`](c4fc.f) | driver: builtins table, option words, `C4FC` |
| [`tests/`](tests/) | `dsl.f` and the `spike*.c` programs, with expected output in `tests/expected/` |

## Hosts

- **Native c4th**: where the Makefile tests run; the image goes to stdout.
- **c4th under c4m, C4KE, C4IX**: c4fc runs wherever c4th does. `-o` saves
  through `dos.f`'s `SAVE-BLOCK`, which needs either C4DOS or a native c4th;
  on a bare VM the image can only go to stdout.
- **C4DOS / c4bb**: `c4th32.c4r` plus the concatenated `c4fc.f`; `-o` writes
  to the RAM disk. `cc.f`'s `MAIN` refuses to run outside C4DOS.

## Limits and known issues

- Slow: see the numbers above. The sources are loaded on every run, since
  c4th cannot save an image.
- On the C4DOS floppy, type `CC` (`cc.bat` runs `RUN c4th.c4r c4fc.f cc.f`;
  `c4fc.f` is every source file concatenated, because sixteen names do not
  fit on a C4DOS command line). `test-c4dos-c4fc` types exactly that.

## Docs

- [c4fc-design.md](../../docs/c4fc-design.md): the design and milestones.
  It predates the inliner (`inline.f`, `-minline`), which it does not mention;
  see [inline-small-functions.md](../../docs/inline-small-functions.md).
- [c4lc-design.md](../../docs/c4lc-design.md): the compiler c4fc matches byte
  for byte.
