# c4cc — the c4 compiler, retargeted to write `.c4r` images

c4cc is a fork of the front end of rswier's `c4.c`. The parser is kept, but
instead of writing opcodes straight into memory it calls a table of emit
handlers (`c4cc_emithandlers[]`, indexed by `EH_*`). `asm-c4r.c` fills that
table in and turns the result into a `.c4r` relocatable image: code, data,
relocation patches, a symbol table, and constructor/destructor lists. An image
can then be loaded by `load-c4r.c` under c4m, by `c4l.c` under plain c4, or by
the kernels in this repo.

On top of the c4 language it adds `for`, `continue`, `switch`/`case`/`default`/
`break` (a jump table in the data segment, dispatched with c4m's `JMPA`),
variadic functions (with `include/stdarg.h`), comma expressions, global and
local arrays, `extern`, `static`, and `__attribute__((constructor))` /
`((destructor))`. It is small and fast: compiling the whole C4KE kernel as a
32-bit image costs about 124 million VM instructions (see
[compiler-on-the-board.md](../../docs/compiler-on-the-board.md)), which is why
it is the C compiler carried on the build floppies.

This directory is also where the `.c4r` format and its tools (`c4rlink`,
`c4rstrip`, `c4rdump`) are documented, below. Their source is in
[`src/c4ke/bin/`](../c4ke/bin/) because they also run as programs inside C4KE.

## Build

| target | produces |
|---|---|
| `make c4cc` | native 64-bit compiler (gcc builds `asm-c4r.c`, which `#include`s `c4cc.c`) |
| `make c4cc32` | native compiler built with `gcc -m32`; writes 32-bit images for c4bb |
| `make c4cc.c4r` | c4cc compiled by itself, as a 64-bit image |
| `make c4cc32.c4r` | c4cc compiled by `c4cc32`, the image the c4bb build disks carry |

c4cc has no preprocessor: it skips `#` lines. Headers are either passed as
extra source files (the Makefile passes `include/u0.h` and `include/c4dos.h`
this way) or the source is run through `gcc -E` or the repo's own `./cpp`
first and piped in on standard input.

## Run

```
c4cc [-S] [-g] [-o outfile] file.c ...     # usage text: "usage: [-g] [-o outfile]"
```

| flag | effect |
|---|---|
| `-o outfile` | write the image to `outfile` (default `a.c4r`) |
| `-S` | also print an assembly listing, interleaved with source lines, to stdout |
| `-g` | keep static symbols in the symbol table (already the default) |
| `@file` | replaced by the whitespace-separated words in `file`; `#` starts a comment |
| `-` | read the source from standard input (last argument) |

Options must come before the source files. All source files are concatenated
and compiled as one unit. Examples, from the Makefile:

```
./c4cc -o hello.c4r src/tests/hello.c
./c4cc -o tar.c4r include/u0.h src/c4ke/bin/tar.c
gcc -E -Iinclude -I. -DC4CC=1 -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1 -C src/c4ke/c4ke.c | ./c4cc -o c4ke.c4r -
```

Where the output goes depends on what c4cc is running on:

- natively: a host file;
- as `c4cc.c4r` under C4KE: the kernel's RAM filesystem, through the
  `OP_VFS_PUT` opcode (the VM has no write syscall);
- as `c4cc.c4r` under C4DOS: the DOS RAM disk, through the C4DOS API table;
- under bare c4m with neither: nothing is written, and it says
  `c4cc: output in C4KE not supported`. `-S` still works.

Input files and `@file` lists are looked up the same way: C4DOS first (so it
sees the RAM disk), then the C4KE RAM filesystem, then the host.

## Test

There is no suite for c4cc alone; it is exercised by the targets below. Of
these only `test-c4l` is part of `make test` (which also builds everything
with c4cc and runs C4KE's `innerbench`).

| target | checks |
|---|---|
| `make test-c4cc-for` | `for` against gcc's output for `src/tests/test_for.c` |
| `make test-c4l` | a switch image is refused by plain c4 (`needs JMPA`) and runs under c4m |
| `make test-link` | two modules compiled separately, linked with `c4rlink`, library mode `-r` |
| `make test-respfile` | an `@file` link produces the same image as the typed one |
| `make test-c4lc` | the c4lc differential battery compiles the same sources with c4cc and c4lc |
| `make test-c4dos-ladder32` | on the simulated c4bb: c4cc rebuilds itself to a fixed point (slow, minutes) |

## Layout

| file | what |
|---|---|
| [`c4cc.c`](c4cc.c) | front end: lexer, parser, symbol table, emit-handler table |
| [`asm-c4r.c`](asm-c4r.c) | the `.c4r` back end and `main`: command line, response files, image writer, `-S` listing. Its header comment is the format reference |
| [`../c4ke/bin/c4rlink.c`](../c4ke/bin/c4rlink.c) | linker; also `c4rstrip` |
| [`../c4ke/bin/c4rdump.c`](../c4ke/bin/c4rdump.c) | image dumper |

## The `.c4r` format

A `.c4r` is a relocatable image. By convention `.c4o` is an object meant for
linking and `.c4l` a library written by `c4rlink -r`; all three are the same
format. The header comment in [`asm-c4r.c`](asm-c4r.c) and the reader in
[`load-c4r.c`](../../load-c4r.c) are the reference.

Header (W = one machine word, so 8 bytes in a 64-bit image, 4 in a 32-bit one):

| field | size | meaning |
|---|---|---|
| `"C4R"` | 3 bytes | signature |
| Version | 1 byte | 2 or 3 |
| WordBits | 1 byte | 64 or 32 |
| MEMSZ / padding | 8 bytes | v3: the first word is the total in-memory data size, the part past DataLen being zero-filled BSS. v2: `p` filler |
| Entry | W | code offset of `main`, or -1 for an object with none |
| CodeLen | W | code segment length in words |
| DataLen | W | data segment length in bytes |
| PatchLen | W | number of patch entries |
| SymbolsLen | W | number of symbols (0 when stripped) |
| ConstructLen | W | number of constructors |
| DestructLen | W | number of destructors |

Then the segments in this order, each preceded by a one-word marker
(`C`, `D`, `P`, `c`, `d`, `S`):

- **code** (`C`): words; **data** (`D`): bytes.
- **patches** (`P`): `(type, address, value)` triples. Type -1 is a code word
  that points into code, -2 a code word that points into data, -3 a data word
  that points into code, -4 a data word that points into data. Addresses are
  word offsets into code for -1/-2 and byte offsets into data for -3/-4
  (pointer initializers of globals, switch jump tables). A positive type is a
  symbol reference, resolved by the linker.
- **constructors / destructors** (`c`, `d`): one code offset per entry.
  Constructors run before `main`; destructors run after `main` returns, not
  after `exit()`.
- **symbols** (`S`): id, type, class and attributes (one word each), a name
  length byte, the name, and a value word. Attribute bits: `0x1` constructor,
  `0x2` destructor, `0x8` static, `0x20` variadic, `0x40` array or struct
  variable.

c4cc writes version 2. c4lc and `c4rlink` write version 3, which puts
uninitialized globals in BSS. The loader accepts both.

### c4rlink

```
c4rlink [-dvrs] [-o outfile] [--] file1.c4r [...fileN.c4r]
```

| flag | effect |
|---|---|
| `-o outfile` | output (default `a.c4r`) |
| `-r` | library mode: unresolved symbols are allowed (writes a `.c4l`) |
| `-s` | strip: write no symbol table |
| `-v` / `-d` | verbose / debug |
| `@file` | response file, as for c4cc |

It concatenates the inputs, rebasing each module's code, data, patches and
constructor lists. Static symbols stay per module. Non-static symbols are
merged by name: an extern resolves to the module that defines it, and if two
modules define the same name the first wins and a warning is printed. Anything
still unresolved is an error unless `-r` is given. The entry point is taken
from the first module that has one.

Objects come from `c4lc -c`, or from plain c4cc (a prototype with no body
becomes a symbol reference). The multi-module images (C4IX, c4or1k, c4mp,
c4sc) are all built as `c4lc -c` per module and then `c4rlink`, which is what
the Makefile's `c4lc_compile_par` does.

Builds: `make c4rlink` (native), `make c4rlink32` (native, `-m32`),
`make c4rlink.c4r`, `make c4rlink32.c4r`. As an image it runs inside C4KE and
C4DOS and writes to their RAM filesystem or RAM disk, as c4cc does.

### c4rstrip

The same program as c4rlink. When `argv[0]` ends in `c4rstrip` it takes one
input, strips it, and writes over the input unless `-o` is given:

```
c4rstrip [-v] [-o outfile] [--] file.c4r
```

Builds: `make c4rstrip`, `make c4rstrip32`, `make c4rstrip.c4r` (a copy of
`c4rlink.c4r`).

### c4rdump

```
c4rdump [-aicdps] [--] file1.c4r [...fileN.c4r]
```

`-i` header info (the default), `-c` disassembly, `-d` data, `-p` patches,
`-s` symbols, `-a` all of them. `make c4rdump` builds it natively; it includes
`c4cc.c` for the instruction-name table.

## Hosts

- **Native** (`make c4cc`, `make c4cc32`): reads and writes host files.
- **Under c4m**: `./c4m load-c4r.c -- c4cc.c4r -S file.c` runs, but bare c4m
  has no way to write a file, so only the listing is useful there.
- **Inside C4KE and C4DOS**: writes to the RAM filesystem or RAM disk, as
  above. The C4DOS build disks carry `c4cc.c4r`, `cpp.c4r` and `c4rlink.c4r`
  and build C4KE and C4IX with them.
- **Plain c4** (`./c4 c4l.c x.c4r`): runs c4cc's *output* as long as it uses
  nothing above c4's `EXIT` opcode. A `switch` needs `JMPA`, so `c4l.c`
  refuses such an image and names the missing opcode; run it under c4m instead.
- **32-bit**: `c4cc32` and `c4cc32.c4r` produce images for c4bb and `c4m32`.

## Limits and known issues

- No preprocessor, `struct`, `union`, `typedef`, `unsigned`, `short`, `long`,
  or `do`/`while`. Use c4lc for those.
- `switch` is limited to 256 cases and a value range under 4096 (it is always
  a jump table).
- A bare function name used as a value (without `&`) emits a wrong load; write
  `&fn`. Found by c4lc's differential tests ([c4lc-design.md](../../docs/c4lc-design.md) L3).
- `-pie` is parsed but broken: the parser matches the `p` and then rejects the
  following `i` as an unknown option.
- `make test-c4rstrip` and `make test-c4sc-run` depend on `hello.c4r` and
  `factorial.c4r` in the repo root, which have no rule of their own; they are
  copied there by the default `make` (`pre`). Run `make` first.

## Docs

- [internals.md](../../docs/internals.md) Part 5 describes c4cc's design. It
  has drifted: it lists `switch`/`break` and arrays as missing, but both are
  implemented.
- The header comment of [`asm-c4r.c`](asm-c4r.c) still calls version 3 an
  unimplemented proposal. Version 3 as written today is the MEMSZ field
  described above; the source-map and opcode-check ideas in that comment were
  never built.
- [compiler-on-the-board.md](../../docs/compiler-on-the-board.md): c4cc on
  c4bb, response files, measured costs.
- [c4lc-design.md](../../docs/c4lc-design.md) §5.1 lists c4cc bugs that c4lc
  deliberately does not reproduce.
