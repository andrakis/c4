# C4DOS — a single-tasking DOS for the C4 family

C4DOS is the bottom rung of the ladder (C4DOS → C4KE → C4IX): a
single-tasking, trap-free disk operating system in the classic DOS shape.
It reads `CONFIG.SYS`, runs `AUTOEXEC.BAT`, shows an `A>` prompt, and runs
transient programs that load, run and return. It needs no scheduler, no
traps and no opcode above c4's original `EXIT`, except the clock (`TIME`),
which is compiled in only for the clock builds and switched on only by
`DEVICE=CLOCK.SYS`. The clockless build runs under unmodified `c4`.

A transient that exports `__c4dos_api` gets DOS's API table written into
it at load time (`include/c4dos.h`): open, read, create/write on the RAM
disk, enumeration, memory release. That is how tools save files on a VM
with no write syscall, and how C4DOS hands its RAM disk to a C4KE it just
built (`src/c4ke/extensions/c4ke_dos.c`).

This folder also holds the tools that rung needs: `cpp`, the toolchain's
own C preprocessor, and the transients that unpack archives, load a
kernel and write media.

## Build

| target | produces |
|---|---|
| `make c4dos.c4r` | clockless, 64-bit; the image `./c4 c4l.c` can run |
| `make c4dos-clock.c4r` | 64-bit with `-DC4DOS_CLOCK=1`; the native dev build |
| `make c4dos32.c4r` | 32-bit clock build, the one c4bb boots |
| `make cpp` | the native preprocessor `./cpp` (from `cpp.c`) |
| `make cpp.c4r`, `cpp32.c4r` | the same preprocessor as a C4DOS transient |
| `make dostar.c4r`, `dostar32.c4r` | `dostar x\|t archive.tar`: unpack a C4TAR1 archive onto the RAM disk |
| `make dosload.c4r`, `dosload32.c4r` | `RUN dosload.c4r <image.c4r> [kernel arguments...]`: load a kernel, give back DOS's 4 MB scratch, and start it with its own command line (LOADLIN for C4DOS) |
| `make bbsave32.c4r` | `bbsave <drive>: [file...]`: write RAM-disk files to a c4bb drive |
| `make install32.c4r` | `install <drive>: [manifest]`: write a bootable medium from a manifest (default `install.lst`) |

The C4DOS images are preprocessed by `./cpp` and compiled by c4lc `-O`
(c4sp for 64-bit, c4sp32 for 32-bit). The transients are compiled by
c4cc with `include/c4dos.h` passed as a source file.

Floppies (directories; C4DOS's "disk" is its working directory, and
`c4dos.dir` in it is the directory listing):

| target | what is on it |
|---|---|
| `make c4dos-disk` | `CONFIG.SYS`, `AUTOEXEC.BAT`, `hello.c4r`, `raycast.c4r` (the plain-c4 build) |
| `make c4dos-disk32` | the same at 32 bits, with `DEVICE=CONSOLE.SYS FLUSH` and `DEVICE=DRIVES.SYS` |
| `make c4dos-build` | everything to build C4KE in-machine: `BUILD.BAT`, `LADDER.BAT`, `dostar`, `cpp`, `c4cc`, `c4ke-src.tar`, `tools-src.tar`, `dosload`, C4KE's userland |
| `make c4dos-build32` | the same for c4bb, all 32-bit |
| `make c4dos-c4ix32` | the whole climb: also `IX.BAT`, `c4ix-src.tar`, `c4sc32`, `install`, `bbsave`, the C4IX userland (needs `make c4bb-images` first for some of it) |
| `make c4dos-c4fc` | c4th plus the c4fc C compiler (in Forth) concatenated into one `c4fc.f` |

## Run

    make run-c4dos          # cd c4dos-disk && ./c4m load-c4r.c -- c4dos-clock.c4r
    make run-c4dos-c4       # cd c4dos-disk && ./c4 c4l.c c4dos.c4r   (unmodified c4; no clock)
    make run-c4dos-bb       # node src/c4bb/sim/cli.js -i -d c4dos-disk32 c4dos32.c4r
    make run-c4dos-build    # the build floppy on c4m: type BUILD, then RUN dosload.c4r c4ke.c4r
    make run-c4dos-build32  # the same on c4bb
    make run-c4dos-c4ix32   # the climb floppy on c4bb: LADDER, then IX
    make run-c4dos-c4fc     # the c4fc floppy on c4bb

C4DOS takes no command-line options; it is configured by `CONFIG.SYS`:

| line | effect |
|---|---|
| `DEVICE=CLOCK.SYS` | enables `TIME` and the API's clock slot (clock builds only) |
| `DEVICE=RAMDISK.SYS SIZE=n` | a RAM disk of `n` bytes (default 1 MB); without it nothing can be written |
| `DEVICE=CONSOLE.SYS FLUSH` | print `A>` without a newline (for consoles that show a partial line, like c4bb's UART) |
| `DEVICE=DRIVES.SYS` | use c4bb's drives; enables `B:`, `C:` … |

Commands at the prompt: `DIR [/P]`, `TYPE`, `COPY`, `RUN prog[.c4r]
[args]`, `ECHO [ON|OFF|text]`, `REM`, `VER`, `TIME`, `MEM`, `EXIT`, a drive
letter such as `B:`, a bare program name, or a `.BAT` file name. Names
resolve on the RAM disk before the disk; `ram:name` opens only the RAM
copy.

## Test

| target | what |
|---|---|
| `make test-c4dos` | four legs: native c4m, the clock build, unmodified c4 through `c4l.c`, and c4bb (including `c4.c4r` compiling hello inside it) |
| `make test-cpp` | `cpp \| c4cc` and `gcc -E \| c4cc` give byte-identical images over the corpus, and `cpp.c` interpreted by plain c4 matches the native build |
| `make test-dosload` | `dosload.c4r` uses no opcode above `EXIT` |
| `make test-c4dos-build32` | on c4bb: `BUILD`, then boot the kernel just built, to a clean shutdown |
| `make test-c4dos-ladder32` | on c4bb: `LADDER` rebuilds the compiler to a fixed point, then the kernel; boots it |
| `make test-c4dos-c4ix32` | on c4bb: `IX` builds C4IX with the compiled compiler and boots it. Slow (minutes) |
| `make test-c4dos-c4fc` | on c4bb: c4fc compiles a C program and C4DOS runs the result. About half a minute on the dev box |
| `make test-ladder` | the full ladder on the host: C4DOS builds C4KE, `dosload` hands over, both toolchains compile and link in the RAM filesystem. Minutes |

None of these is part of `make test`.

## Layout

| path | what |
|---|---|
| `c4dos.c` | the DOS: loader, invoke stub, API table, RAM disk, batch files, `CONFIG.SYS`, prompt |
| `cpp.c` | the C preprocessor (`#include`, object- and function-like `#define`, `#if` family, `-I`, `-D`, `-o`); refuses `##`, `#`-stringize, line continuations and variadic macros |
| `dostar.c` | unpack C4TAR1 archives onto the RAM disk |
| `dosload.c` | load and start a kernel with its own command line |
| `bbsave.c` | save RAM-disk files to a c4bb drive |
| `install.c` | write a bootable medium from a manifest |
| [`fs/`](fs/README.md) | `CONFIG.SYS`, `AUTOEXEC.BAT`, the build batch files and manifests copied onto the floppies |
| `tests/test-c4dos.sh`, `tests/test-cpp.sh` | the scripts behind `test-c4dos` and `test-cpp` |

`include/c4dos.h` is the transient-side API and `include/c4dos_native.h`
its stub for host builds (see [include/README.md](../../include/README.md)).
Programs meant to run at the C4DOS prompt and inside C4KE are compiled
against `include/u0lite.h` instead of `u0.h`.

## Hosts

- **Native c4m**: `make run-c4dos`, the development loop.
- **Plain c4 via `c4l.c`**: `./c4 c4l.c c4dos.c4r`, clockless build only.
  `c4l.c` refuses an image with any opcode above `EXIT` and names it, which
  is how `test-c4dos` proves the image is pure. `TIME` reports that there
  is no clock, so the missing `/proc/uptime` on Windows does not matter
  here.
- **32-bit / c4bb**: `c4dos32.c4r` boots on the breadboard, where the
  drives, `bbsave`, `install` and the whole climb live
  ([docs/climbing-the-ladder.md](../../docs/climbing-the-ladder.md)).
- C4DOS does not call `__c4_info()` (it is opcode 57, above `EXIT`); what
  the machine has is declared in `CONFIG.SYS` instead of probed.

## Limits and known issues

- A command line holds at most 16 tokens (`ARGVMAX`). Linking C4IX needs
  more, which is why `c4rlink` accepts `@file` response files and why
  `fs/c4ix.objs` exists.
- No `>` redirection, by design: tools write files through the API.
- The RAM disk has 64 slots and 8 open handles, and dies with the session
  unless saved with `bbsave` or `install`.
- DOS never gets back memory a transient allocated; many compiler runs in
  one session accumulate. Building C4IX is left to C4KE for that reason.

## Docs

- [docs/c4dos-design.md](../../docs/c4dos-design.md): the design contract.
  It has drifted: its status line is dated 2026-08-21, and it names files
  that do not exist (such as `c4dos_load.c`). The build moved from c4cc to
  c4lc after it was written; the header of `c4dos.c` still shows the c4cc
  command.
- [docs/compiler-on-the-board.md](../../docs/compiler-on-the-board.md):
  building C4KE and C4IX on c4bb, with cycle counts.
- [docs/climbing-the-ladder.md](../../docs/climbing-the-ladder.md): the
  climb, typed by hand.
- [docs/dos-rung-fixes.md](../../docs/dos-rung-fixes.md): the tracker for
  a set of fixes at this rung (2026-09-05).
