# c4sc — c4lc compiled to C, so the C compiler stops being interpreted

c4sc is a compiler from c4sp Lisp to C, written in c4sp Lisp
([`c4sc.lisp`](c4sc.lisp)). It exists for one program: c4lc, the C compiler in
[`../c4sp/lisp/`](../c4sp/lisp/). A survey of c4lc found no closures, no
higher-order calls, no variadic lambdas, no macros and no `eval`, so every
Lisp function becomes a C function and every local a C local. It is a
transliterator for that subset, not a general Lisp compiler.

The generated C runs on c4sp's own cells, allocator and collector (the runtime
in [`scrt.h`](scrt.h) is the builtins without the call protocol). The seven
generated units are compiled by c4lc into objects and linked with a driver
into `c4sc.c4r`: c4lc, compiled. On the simulated c4bb this brought the
in-machine build of the twelve C4IX modules from about 30 minutes to 4.6
minutes of cycles (5.8 minutes wall), measured in
[c4sc-design.md](../../docs/c4sc-design.md) M8.

## Build

| target | produces |
|---|---|
| `src/c4sc/c4<unit>_gen.c` | generated C for one c4lc file (`opt lex pp parse c4r tree gen`); made on demand by the targets below, and gitignored |
| `make c4sc-host` | native binary with all seven units compiled in; the differential harness |
| `make c4sc.c4r` | 64-bit image: the driver and seven units, each `c4lc -O -c`, linked by `c4rlink` |
| `make c4sc32.c4r` | the same at 32 bits, via `c4sp32` and `c4rlink32` |
| `make c4sc32-fused.c4r` | `c4sc32.c4r` put through c4opt's fuse pass; for c4bb, c4mp and oisc4, not c4m |
| `make c4sc-self` | c4sc compiled by itself, as a native binary |

The generation step is `./c4sp -R src/c4sc/c4sc.lisp in.lisp out.c unitname`.

## Run

Both `c4sc-host` and the images take an optional `-c ncells` (arena size,
default 4000000) and then a mode. The modes and their usage lines, from
[`body.h`](body.h):

```
c4sc-host [opt] [-mfuse] in.c4r out.c4r                    # c4opt-run.lisp
c4sc-host tokens [-count|-conforming] file.c               # c4lc-tokens.lisp
c4sc-host pp [-I dir] [-D name] file.c                     # c4lc-ppdump.lisp
c4sc-host pptok file.i
c4sc-host ast [-check] file.c                              # c4lc-ast.lisp
c4sc-host compile [-O] [-c] [-P] [-mcisc] [-mfuse] [-conforming]
                  [-fno-builtin] [-fpic] [-I dir] [-D name] in.c out.c4r          # c4lc.lisp
```

`compile` is c4lc. For example, one C4IX module on the simulated board:

```
node src/c4bb/sim/cli.js -s -m 512 -d <disk> c4sc32-fused.c4r -c 8000000 \
    compile -O -c -I src/c4ix src/c4ix/boot.c out.c4o
```

`c4sc-self in.lisp out.c [unitname]` is c4sc itself.

## Test

None of these is part of `make test`.

| target | checks |
|---|---|
| `make test-c4sc` | every generated unit compiles with `c4lc -O -c`; prints the expansion per file |
| `make test-c4sc-run` | compiled c4opt produces byte-identical images to the interpreted one (needs `factorial.c4r` in the root: run `make` first) |
| `make test-c4sc-lex` | compiled lexer's token dump matches the interpreted one over the c4fc lexer corpus |
| `make test-c4sc-front` | preprocessor agrees with gcc -E on the twelve C4IX modules; parser matches the committed AST dump |
| `make test-c4sc-back` | every image and object is byte-identical to the interpreted c4lc's; the twelve C4IX objects link to the same `c4ix.c4r` that `make c4ix.c4r` builds, and it boots |
| `make test-c4sc-image` | `c4sc.c4r` under c4m produces the same object sizes as the native host |
| `make test-c4sc-self` | fixed point: generations 1, 2 and 3 of c4sc are byte-identical, and it emits the same seven units |
| `make test-c4sc-board` | on the simulated c4bb, `c4sc32-fused.c4r` compiles `boot.c` to the same bytes c4lc does |

## Layout

| file | what |
|---|---|
| [`c4sc.lisp`](c4sc.lisp) | the compiler, Lisp to C |
| [`scrt.h`](scrt.h) | runtime the generated code calls: c4sp builtins without the call protocol |
| [`host.h`](host.h) | bridge between compiled units and the interpreter: calls that look up a name in c4sp's global environment |
| [`body.h`](body.h) | the driver's modes, shared by the native and image builds |
| [`c4sc-host.c`](c4sc-host.c) | native driver: `#include`s the generated units |
| [`c4sc-main.c`](c4sc-main.c) | image driver: declares the units, compiled with `c4lc -c` and linked |
| [`c4sc-self.c`](c4sc-self.c) | driver for c4sc compiled by itself (separate because `c4sc.lisp` and `c4r.lisp` both define `second` and `third`) |
| `*_gen.c` | generated, gitignored |

## Hosts

- **Native**: `c4sc-host`, `c4sc-self`.
- **Under c4m**: `c4sc.c4r` runs, but bare c4m cannot write files; the tests
  compare the size line printed before the write.
- **c4bb**: `c4sc32-fused.c4r` is the compiler the in-machine C4IX build uses;
  `src/c4bb/tests/build-images.sh` copies it onto the disk. It uses fused
  opcodes that c4m does not have, so `c4sc32.c4r` is kept for c4m and for the
  byte check.
- **C4IX**: the same image is `/bin/c4sc`, which the BUILD.B4K files under
  `/usr` run and `cc` (`src/c4ix/user/cc.c`) drives, on a task's default
  256 KB stack. It needs 11 KB of it at most for the tree's sources
  (c4m.c, 2026-10-25); the 1 MB and 4 MB it was given before were for
  g:resolvecode recursing once per instruction (fixed in 29cbac9).

## Limits and known issues

- It compiles the subset c4lc uses and refuses anything else. A change to
  c4lc that adds a closure or a higher-order call breaks this build.
- `compile` mode has no `-minline`; the Lisp driver does.
- `c4sc-main.c` goes through `gcc -E` first, because c4lc's own preprocessor
  rejects a function-like macro named without an argument list (in
  `include/c4.h`).

## Docs

- [c4sc-design.md](../../docs/c4sc-design.md): the survey, milestones M0–M8
  (all ticked) and measurements.
- [compiler-on-the-board.md](../../docs/compiler-on-the-board.md): why the
  compiler's speed on c4bb matters.
- [../c4sp/bench/README.md](../c4sp/bench/README.md): the M0 measurement that
  justified building this.
