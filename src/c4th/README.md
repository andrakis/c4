# c4th — a Forth for C4

c4th is a Forth-2012 CORE system written in the C subset this repo compiles.
Like c4sp, one source builds natively with gcc and as a `.c4r` image, and the
same image runs under c4m, inside C4KE, inside C4IX and under C4DOS.

It is indirect-threaded: each word's code field holds the address of a C
function, and the inner interpreter calls it through a local variable, which
the C4 compilers turn into a single `JSRS` instruction. Everything else is
Forth in [`forth/`](forth/): the rest of CORE, an assembler for C4 opcodes, a
native-code backend, a metacompiler that writes standalone `.c4r` images, and
a Forth compiler written in Forth that reproduces itself. c4fc, a C compiler
([../c4fc/README.md](../c4fc/README.md)), is written on top of it.

## Build

| target | produces |
|---|---|
| `make c4th` | native binary (`gcc -O2`) |
| `make c4th.c4r` | image: `gcc -E` then c4cc |
| `make c4th32.c4r` | 32-bit image for c4bb: `gcc -E` then c4lc under `c4sp32` |
| `make c4dos-c4th` | a C4DOS disk directory with `c4th.c4r`, `core.f` and a test program |

## Run

```
c4th [-selftest] [-e text] [-i cells] [-d cells] [-r cells] [file.f ...]
```

| flag | effect |
|---|---|
| `-selftest` | run the hand-threaded B1 check (10!) and exit |
| `-e text` | interpret `text`, then continue with any files |
| `-i cells` | image size (default 1048576 cells) |
| `-d cells` | data stack (default 1024) |
| `-r cells` | return stack (default 256) |

Files are interpreted in order. With neither `-e` nor a file, c4th reads
standard input. The kernel in C has only the primitives; load
`forth/core.f` first for the rest of CORE:

```
./c4th src/c4th/forth/core.f -e ': SQ DUP * ; 7 SQ . CR'
./c4m load-c4r.c -- c4th.c4r src/c4th/forth/core.f myprog.f
./c4th src/c4th/forth/core.f src/c4th/forth/self.f -e 'MAIN' > out.c4r
```

## Test

None of these is part of `make test`.

| target | checks |
|---|---|
| `make test-c4th` | B1 selftest; B2 outer interpreter; B3 the Forth-2012 CORE suite (native, under c4m, inside C4KE); B4 assembler and peephole; B5 native backend (under c4m and c4mp); B5d metacompiler images run under c4m, c4mp, oisc4 and plain c4; the differential fuzzer; B5d.5 the self-hosting fixed point |
| `make test-c4th-os` | one `c4th.c4r` on C4KE, C4DOS and C4IX, against the native build |
| `make test-c4th-bb` | `c4th32.c4r` on the simulated c4bb |

Details of the test files are in [tests/README.md](tests/README.md).

## Layout

| path | what |
|---|---|
| [`c4th.c`](c4th.c) | driver: flags, B1 selftest, `main` |
| [`include/mem.h`](include/mem.h) | the image, the two stacks, the allocation pointer |
| [`include/dict.h`](include/dict.h) | the dictionary |
| [`include/inner.h`](include/inner.h) | the inner interpreter |
| [`include/prim.h`](include/prim.h) | primitives |
| [`include/num.h`](include/num.h) | double-cell arithmetic and pictured numeric output |
| [`include/io.h`](include/io.h) | input sources and character output |
| [`include/outer.h`](include/outer.h) | the outer (text) interpreter |
| [`forth/`](forth/) | the Forth library — see [forth/README.md](forth/README.md) |
| [`tests/`](tests/) | Forth-2012 suite, B-milestone tests, the fuzzer — see [tests/README.md](tests/README.md) |
| [`bench/`](bench/) | the B5 probe that chose the native backend's strategy — see [bench/README.md](bench/README.md) |

## Hosts

- **Native** (`make c4th`): everything except calling code that `native.f`
  has just generated, which needs a C4 VM to run in (those tests run under
  c4m). Writing a `.c4r` file (`c4r.f`, `self.f`) needs a write syscall, so the
  metacompiler tests run natively; the images it writes run on the hosts
  below.
- **Under c4m** (`./c4m load-c4r.c -- c4th.c4r ...`): the full system,
  including the native backend.
- **c4mp**: runs `c4th.c4r` and the fused-opcode tests.
- **Inside C4KE and C4IX**: `c4th.c4r` runs as a task; in C4IX it is driven by
  C4IX's shell and compiles `self.f` inside the kernel (`test-c4th-os`).
- **Under C4DOS**: `RUN c4th.c4r core.f ...`. `forth/dos.f` writes files to the
  DOS RAM disk through the C4DOS API table, the only way a program on the VM
  can produce a file.
- **Plain c4** (`./c4 c4l.c x.c4r`): images written by the metacompiler use
  nothing above `EXIT`, and run. `c4th.c4r` itself uses `JSRS`, so it does not.
- **c4bb**: `c4th32.c4r`, built by c4lc because the board is 32-bit.

## Limits and known issues

- `make test-c4th-os`: the C4DOS leg prints an empty prompt line ahead of
  c4th's output and fails the comparison at byte 1 (recorded in
  [c4ix-desktop.md](../../docs/c4ix-desktop.md)).
- c4lc's own preprocessor cannot read `c4th.c` (a function-like macro named
  without an argument list), so both image builds go through `gcc -E`.
- There is no saved image: the dictionary holds real addresses, so the
  sources are loaded at every start (about 19 s for c4fc's on c4bb).

## Docs

- [c4th-design.md](../../docs/c4th-design.md): design and milestones B1–B6.
  §2 Layout has drifted: it lists `coreext.f` and `meta.f`, which do not
  exist, and misses `ext.f`, `locals.f`, `peep.f`, `self.f`, `trt.f` and
  `dos.f`.
- [c4th-selfhost.md](../../docs/c4th-selfhost.md): B5d.5, the fixed point.
