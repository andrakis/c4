# src/tests — test programs

C programs the rest of the repo runs as tests: a corpus of small programs
that every VM and compiler here must agree on, programs that only make
sense inside C4KE, compiler test inputs, and a few demos. Most of them are
compiled by c4cc against the C4KE user runtime:

    ./c4cc -o src/tests/NAME.c4r include/u0.h src/tests/NAME.c

(the Makefile's `src/tests/%.c4r` rule). Exceptions are noted below.
`make pre` builds the corpus and copies every `src/tests/*.c4r` to the
repo root, so C4KE can run them by name (`./c4m load-c4r.c -- c4ke.c4r
test_basic`). The `.c4r` files are build outputs and are not tracked.

`make test-c4lc` parses every `.c` here with c4lc (except `genfloat.c`,
`test_illins.c` and `c4lc_pp.c`), so a file in this folder must at least
be valid c4cc-dialect C.

## The corpus (`TESTS_C4R`)

The images listed in `TESTS_C4R` in the Makefile are the parity oracle:
`make test-c4bb`, `make test-libjs`, `make test-oisc4` and `make test-c4mp`
run them on the breadboard, the JavaScript interpreter, the one-instruction
computer and c4mp, and compare against native c4m.

| file | what |
|---|---|
| `hello.c` | prints a greeting. Built without u0, so it uses no opcode above `EXIT` and runs under plain c4 via `c4l.c` (`make test-c4l`) |
| `factorial.c`, `multifun.c`, `fun_with_ptrs.c`, `tests.c`, `test_basic.c`, `test-order.c`, `test-ptrs.c`, `test_args.c`, `test_continue.c`, `test_static.c`, `test_malloc.c`, `test_printf.c`, `test_printloop.c`, `test_fread.c`, `test_exit.c`, `test_crash.c`, `test_signal.c`, `test_customop.c`, `test_timekeeping.c`, `test_infiniteloop.c`, `rps.c`, `test_for.c` | language, library and VM behaviour, one area per file |
| `test_float.c` + `float_cases.h` | soft float (`include/c4_float.h`) against a table generated from hardware floats; built through `gcc -E` |
| `test_vprintf.c` | the `include/stdio.h` printf family; built through `gcc -E` |
| `mandel.c` | ASCII Mandelbrot, a port of github.com/Johnlon/mandelbrot |
| `cycles.c` | prints the VM cycle counter first thing, to measure boot cost (`make bench-c4ix`) |
| `raycast.c` | console raycaster, below |
| `test_ramfs.c`, `test_selfhost.c`, `test_ramopt.c`, `test_ramcc.c`, `test_ramlink.c`, `test_ixbuild.c` | C4KE-only, below |

## Programs that run inside C4KE

| file | what | target |
|---|---|---|
| `test_ramfs.c` | the kernel RAM filesystem: write, read, replace, list, unlink | `make test-c4ke-ramfs` |
| `test_selfhost.c` | c4cc compiles a program into the RAM filesystem and the kernel runs it from memory | `make test-c4ke-ramfs` |
| `test_ramopt.c` | c4sp optimizes an image into the RAM filesystem | `make test-c4ke-ramfs` |
| `test_ramcc.c` | c4lc (on c4sp) compiles into the RAM filesystem | `make test-c4lc` |
| `test_ramlink.c` | two objects compiled and linked in the RAM filesystem | `make test-c4ke-ramfs`, `make test-ladder` |
| `test_ixbuild.c` | c4lc compiles one C4IX module to an object in memory and c4rlink makes a library from it | `make test-ladder` |
| `test_leak.c` | allocates and never frees, round after round | `make test-task-mem` |

## Compiler and linker inputs

| file | used by |
|---|---|
| `c4lc_lex_sample.c` | token and AST goldens: `make test-c4lc`, `test-c4sc-lex`, `test-c4sc-front`, `test-c4fc` |
| `c4lc_l2.c`, `c4lc_l7.c`, `c4lc_for.c`, `c4lc_enum.c`, `c4lc_esc.c`, `test_tailcall.c` | c4lc code generation (`make test-c4lc`; `test_tailcall.c` also `test-c4sp-opt`) |
| `c4lc_ppinc.c`, `c4lc_ppinc.h`, `c4lc_ppinc2.h` | nested `#include` and line tracking in c4lc's preprocessor (`make test-c4lc`) |
| `c4lc_pp.c` | preprocessor token stream: c4fc's must match c4lc's (`make test-c4fc`) |
| `test_link_a.c` … `test_link_f.c` | separate compilation and linking: `a`/`b` in `make test-link`, `test-respfile`, `test-b4ke`; `c`–`f` in `make test-c4lc` (`e`/`f` share data) |
| `test_switch.c` | a jumptable `switch`; `make test-c4l` checks plain c4 refuses it by name |
| `test_realloc.c` | c4m's guest allocator, native and under plain c4 (`make test-c4m-mem`, golden in `expected/`) |
| `test_for.c` | c4cc's `for` against gcc's output (`make test-c4cc-for`, golden in `expected/`) |
| `test_coop_switch.c` | task switching with only `LEV`; the proof of concept behind C4IX's and C4DOS's cooperative paths |
| `c4_jailbreak.c`, `global.c`, `puts.c`, `reverse.c`, `test_gcscan.c`, `test_globals.c`, `test-oisc.c`, `vararg2.c` | extra inputs to the compiler differential sweeps (`C4LC_DIFF*` for `test-c4sp-opt`, `C4SC_BACK_DIFF` for `test-c4sc-back`, `C4FC_GEN_SWEEP` for `test-c4fc`) |
| `test_crash2.c` | used by `mpg/test-mpg.sh` |
| `dosenum.c` | enumerates the C4DOS RAM disk through the v2 API; built with `include/c4dos.h` by `src/c4dos/tests/test-c4dos.sh` |

## raycast.c

A console raycaster (integer DDA, 256-colour background blocks, no
floating point). One source, several builds, chosen with `-D` (c4lc's
preprocessor is on only when `-D` is given):

| build | target | for |
|---|---|---|
| `-D RC_KE=1` | `make src/tests/raycast.c4r` | c4m, C4KE, C4IX, c4bb |
| `-D RC_DOS=1` | `make raycast-dos.c4r`, `raycast-dos32.c4r` | a C4DOS transient: stock opcodes plus `TIME` |
| `-D RC_C4=1` | `make raycast-c4.c4r` | C4DOS on plain c4: no `TIME` |

Usage, from the program:

    raycast [WxH] [-g COLSxROWS] [-s SEED] [-f MS] [-n N] [-d] [-i]
      WxH   map size in cells (default 21x21, forced odd, 9..63)
      -g    terminal geometry (default 80x25)
      -s N  maze seed        -n N  render N frames then exit
      -f N  frame cap in ms  -d/-i  force demo / interactive
      -K    read keys from stdin instead of the terminal
      -G WxH  draw WxH pixels on the libjs display instead (keys there too)
    keys: w a s d move, m minimap, p demo, q quit

`make test-raycast` compares 64-bit and 32-bit output against
`raycast.golden.txt` and `raycast.maze.txt` (`-M` prints the maze).

## Everything else

| path | what |
|---|---|
| `expected/` | goldens for `test-c4m-mem` and `test-c4cc-for` |
| `mpg/` | the c4mpg memory guard's tests: one known-bad program per mistake (`mpg_*.c`), `test-mpg.sh`, and `check-fork.sh`, which checks `c4mpg.c` is `c4m.c` plus marked blocks. `make test-mpg` |
| `genfloat.c` | native tool that regenerates `float_cases.h`: `gcc -O2 -o genfloat src/tests/genfloat.c && ./genfloat > src/tests/float_cases.h` |
| `test_illins.c`, `test_trap.c` | marked OBSOLETE in their headers; not built |
| `oldtest_vararg.c`, `oldtest_vararg2.c`, `oldvararg3.c`, `vararg.c` | older varargs tests; only parsed by `test-c4lc` |
| `test_extern.c`, `test_extern_fun.c` | an extern-function pair; not built by any target |
| `oisc-min.c`, `oisc-asm.c`, `oisc-c4.c` | an early one-instruction-computer experiment, superseded by `src/oisc4`; not built |

Of the targets on this page, only `test-c4m-mem` and `test-c4l` are part
of `make test`; the rest run from their own targets.
