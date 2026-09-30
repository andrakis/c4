# include — headers for C4 programs and the native builds

Headers shared across the repo. Two things are unusual here.

**c4cc has no preprocessor.** Its lexer skips every `#` line, so an
`#include` in a program compiled by c4cc does nothing. A header such as
`u0.h` or `c4dos.h` is passed to c4cc as one more *source file*, ahead of
the program:

    ./c4cc -o prog.c4r include/u0.h prog.c

Builds that need `#if` answered run `gcc -E -Iinclude -I. -DC4CC=1
-D__c4__=1 -D__C4CC__=1 -D__c4cc__=1` (the Makefile's `PREPROC`) or the
repo's own `./cpp` first and pipe the result to c4cc; c4lc has its own
preprocessor (`-I`, `-D`, `-P`).

**The libc-named headers serve two masters.** Native builds use
`-idirafter include`, so the system headers win and these are only a
fallback. Under the c4cc defines (`__c4cc__` / `C4CC`), the same files
supply C4 versions instead; natively they defer to the system header
(`#include_next` or an absolute `/usr/include/...` path).

## Runtimes: pick exactly one

| header | for |
|---|---|
| `u0.h` | the C4KE user runtime: pids, signals, task start and wait, the RAM filesystem, kernel opcodes looked up by name. Programs for C4KE (and C4KE programs run under C4IX) |
| `u0lite.h` | the part of u0 that needs no kernel and no opcode above `EXIT` (string and character helpers, the same `rand`). Programs for the C4DOS prompt. Same names as u0, so one source builds either way |
| `c4ke.h` | an older, drifted copy of `u0.h`; nothing is built against it. `build-images.sh` still copies it onto the c4bb disk |

A u0 build run under C4DOS prints "C4DOS: This application requires C4KE."
and returns to the prompt.

## Files

| file | what |
|---|---|
| `c4.h` | native-vs-C4 detection (`NATIVE`), the standard includes, and portability shims so the same source compiles under gcc, MSVC and c4cc |
| `c4m.h` | no-op definitions of c4m's builtins (`__c4_trap`, `__c4_opcode`, `__c4_info`, …) so gcc can syntax-check c4m code |
| `c4m_util.h` | c4m's native clock: `clock_gettime`, with a `timespec_get` fallback on MSVC. Included by `c4m.c` in native builds |
| `c4m_float.h` | the interface to c4m's floating-point support (`c4m_float.c`) |
| `c4_float.h` | IEEE-754 binary32 arithmetic written in C4, carried in an `int`; subnormals flush to zero |
| `stdio.h` | the printf family on one formatting core (`vsnprintf`), so output goes through one function a kernel can redirect; uses only stock c4 opcodes |
| `stdlib.h`, `string.h`, `memory.h`, `ctype.h`, `stdarg.h`, `stddef.h`, `signal.h`, `unistd.h`, `fcntl.h`, `time.h`, `stdint.h` | C4 versions of the libc headers, or pass-throughs to the system ones |
| `c4cc_helpers.h` | a stack-printing debug helper; used by `src/tests/vararg2.c` |
| `c4dos.h` | the C4DOS transient API: `__c4dos_api`, the invoke stub, open/read/create/write on the RAM disk. Strict c4, so gcc cannot compile it |
| `c4dos_native.h` | the same API stubbed out for host builds (`dos_can_write()` is false), used by `cpp`, `dostar` and the c4sp/c4sc host tools |
| `c4bb.h` | c4bb's memory-mapped drives, clock and timer registers, as plain loads and stores (nothing above `EXIT`) |
| `c4bb_info.h` | "is this a breadboard?" via `__c4_info()` — opcode 57, so including it lifts a program above the C4DOS rung |
| `c4bb_mbox.h` | the c4bb host mailbox, guest side: two rings of frames in shared RAM. Inert unless the host announces a mailbox |
| `c4ke_mbox.h` | the mailbox from a C4KE task (`mbox_*`, `msg_*`); pass after `u0.h` |
| `c4ke/` | the C4KE kernel headers, see below |

## c4ke/

`c4.h`, `c4ke.h`, `c4m.h`, `common.h`, `config.h`, `extension.h`,
`opcodes.h`, `service.h`: the headers C4KE's kernel and extensions include
as `<c4ke/...>`. They are byte-identical to `src/c4ke/include/`, and both
copies are in use (the kernel build reads these; the VFS service build
reads `src/c4ke/include/service.h`), so a change must be made in both.
`config.h` holds the kernel's compile-time options, such as
`CONFIG_ENABLE_PM`. See [src/c4ke/README.md](../src/c4ke/README.md).

## Notes

- Many sources say `#include "u0.c"` or `#include "u0.h"`. There is no
  `u0.c`; the line is inert under c4cc, and the build passes
  `include/u0.h` explicitly.
- `c4bb.h` and `c4bb_info.h` are split on purpose: a C4DOS-rung program
  can drive the drives and clock without asking `__c4_info()`, which would
  need opcode 57.
- `src/c4lm/include/stdio.h` carries its own copy of the printf core for
  c4lm's separate include tree.
