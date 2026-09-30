# src/c4ke/bin — C4KE userland and tools

Programs that run as C4KE tasks, plus the native `.c4r` tools that share
their source. Unless a row says otherwise, a program here is compiled by
c4cc with the user runtime passed first as a source file:

    ./c4cc -o src/c4ke/bin/NAME.c4r include/u0.h src/c4ke/bin/NAME.c

which is the Makefile's `src/c4ke/bin/%.c4r` rule. c4cc has no
preprocessor, so the `#include` lines in these files are skipped and every
file a program needs is named on the command line. `make pre` builds the
set listed in `C4KE_BIN` (`c4le cat echo kill ls ps spin type xxd b4ke`)
plus `top`, `c4rdump` and `c4rlink`, and copies them to the repo root,
where the kernel's loader finds them by bare name.

## Shell-level programs

| file | what | built by |
|---|---|---|
| `ls.c` | lists the kernel RAM filesystem, inferring one directory level from `/` in the flat names; `-a` lists every full path | `make pre`; `ls32.c4r` |
| `ps.c` | process listing; also a library (`PS_NOMAIN`) linked into `top`, `c4sh`, `eshell` and `init` | `make pre`; `ps32.c4r` |
| `top.c` | repeating process view; `-d nn` refresh ms, `-b` background (no focus), `-n nn` repeat count | `make top.c4r` (`ps.c` + `top.c`) |
| `cat.c` | print files | `make pre` |
| `echo.c` | print arguments | `make pre` |
| `type.c` | print files, `[-np] [-P n]` for line numbers and paging | `make pre` |
| `xxd.c` | hex dump | `make pre` |
| `kill.c` | send a signal (`kill -l` lists them); compiled after `eshell.c` | `make pre` |
| `spin.c` | loops until SIGINT, for exercising the scheduler | `make pre` |
| `c4le.c` | line editor stub: argument parsing and a command table whose handlers are empty | `make pre` |
| `c4led.txt` | a mock-up transcript of a line-editor session; nothing reads it | — |
| `eshell.c` | the emergency shell: linked into `init.c4r` as its fallback, or built alone as `eshell.c4r` by `build-images.sh` | via `init.c4r` |

## Build and install tools

| file | what | built by |
|---|---|---|
| `c4rdump.c` | print the details of a `.c4r` image | native `make c4rdump`; `make c4rdump.c4r` |
| `c4rlink.c` | link `.c4o` objects and `.c4l` libraries; `-r` writes a library, `-s` strips symbols, `@file` reads arguments from a file. Invoked as `c4rstrip` it strips one image | native `make c4rlink`, `make c4rstrip`; `make c4rlink.c4r`, `c4rlink32.c4r` |
| `b4ke.c` | build driver: `b4ke [-f FILE] [-n] [-k] [-t]` runs a `.b4k` script (verbs `ECHO`, `LIST`, `TARGET`, `RUN`, magic `B4KE1`); a step is skipped when its `TARGET` already exists | `make pre`; `b4ke32.c4r` |
| `b4ke-selftest.b4k` | two modules compiled, listed and linked in the RAM filesystem | read by `make test-b4ke` |
| `c4ix.b4k` | builds C4IX's twelve modules with `c4sc.c4r` and links them | used on the breadboard climb |
| `tar.c` | `tar x\|t archive.tar` unpacks a C4TAR1 archive (the format of `src/c4dos/dostar.c`) into the RAM filesystem | `make tar.c4r`, `tar32.c4r` |
| `vfsload.c` | fills the RAM filesystem at boot from `c4ke.vfs.txt`; the file's header documents the manifest grammar. Built by c4lc, with `gcc -E` first | `make vfsload.c4r`; 32-bit by `build-images.sh` |
| `save.c` | `save 1: [file]` writes RAM filesystem files onto a c4bb drive (board only, `include/c4bb.h`) | `make save32.c4r` |
| `install.c` | `kinstall C: [manifest]` writes a bootable medium from a manifest (default `c4ix.lst`, see `../fs/c4ix.lst`); board only | `make kinstall32.c4r` |

## Test programs

| file | what | used by |
|---|---|---|
| `badop.c` | raises an uninstalled custom opcode; the kernel must kill the task and survive | `test-c4bb` |
| `badmem.c` | writes one word past a buffer, for the c4mpg guard | `make test-mpg` |
| `mbecho.c` | binds the host mailbox and echoes frames | `test-c4bb` (`test-c4ke-mbox.mjs`) |
| `mbpair.c` | two tasks messaging through their inboxes | `test-c4bb` (`test-c4ke-mbox.mjs`) |
| `dbg.c` | a design note for a debugger; comments only, not built | — |

`mbecho.c` and `mbpair.c` also need `include/c4ke_mbox.h` after `u0.h`;
`build-images.sh` compiles them that way.

## Conventions

- A program ends by returning from `main` or calling `exit()`; u0 turns
  that into a kernel call, because a task cannot tear down the stack it is
  running on.
- Programs that print with `printf` write straight to the host console;
  C4KE has no per-task output redirection.
- `.c4r` files here are build outputs and are not tracked.
