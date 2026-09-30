# src/bench — C4KE benchmarks

Three C4KE programs that measure the VM and the kernel. They are also the
workload of `make test`, which boots C4KE with `innerbench` as its init
process.

## Build

Built by `make pre` with the rule for `src/bench/%.c4r`:

    ./c4cc -o src/bench/NAME.c4r include/u0.h src/bench/NAME.c

`make pre` then copies them to the repo root, where C4KE finds them by
name. The `.c4r` files are build outputs and are not tracked. The 32-bit
C4DOS floppies (`c4dos-build32`, `c4dos-c4ix32`) carry builds made with
`c4cc32`.

## Run

From a C4KE shell (`make run`), or as the kernel's init:

    ./c4m load-c4r.c -- c4ke.c4r bench -b all
    ./c4m load-c4r.c -- c4ke.c4r innerbench -n 4
    ./c4m load-c4r.c -- c4ke.c4r benchtop

`bench`, from its help text:

    bench [-C] [-t nn] [-c nn] [-b benchmark] [-ndq] [-w nn] [-a x]
           -C            Run continuously, until ctrl+c caught
           -t nn         Time to measure for
           -c nn         Number of repeats of measurement to run
           -b benchmark  Benchmark to use. Available options:
                         fac     Factorial benchmark
                         fac_r   Recursive factorial benchmark
                         pi10    10th digit of pi
                         pi30    30th digit of pi
                         pi100   100th digit of pi
                         pi1000  1000th digit of pi
                         pi      Run all pi benchmarks
                         all     Run all benchmarks
           -n            No delay, do not settle clock between benchmarks
           -d            Display debug information
           -w nn         Wait nn seconds before starting benchmark
           -q            Quiet mode
           -a x          Pass argument to benchmark

`innerbench` starts several copies of a benchmark, each by default inside
its own nested kernel: `c4m load-c4r.c src/c4ke/c4ke.c -- -v 0 bench`,
that is, c4m compiling C4KE from source and running `bench` under it. It
also starts `top -b` unless given `-T`. From its help text:

    innerbench [-n x] [-aTBDqcmlvVt] [-b args]
          -n x    Start x (number) inner benchmarks
          -a      Pass `-a` to `c4m`, to use alternate time mode
          -T      Disable running `top` during execution
          -B      Enable direct bench execution (ignores below options except -b)
          -D      Launch `benchtop` instead of `bench`
          -q      Enable quiet mode for benchmark
          -c      Enable C4 mode (ignored if -m given)
          -m      Enable C4M mode (default if -c not given)
          -l      Use load-c4r by itself (ignored if -d given)
          -d      Use load-c4r + c4ke.c from source (default)
          -v      Enable verbose mode
          -t      Enable test processes
          -b args Pass args to bench

The default is two inner benchmarks. `-c` nests `c4 c4m.c` instead of
`c4m`. `-B` runs `bench` directly as tasks of the current kernel, with no
nesting.

`benchtop` starts `top -b` and `bench -C` together and waits on the bench.

## Test

| target | what |
|---|---|
| `make test` | `innerbench` as C4KE's init; fails on an inner compile error or if no inner benchmark completes |
| `make test-massive` | `innerbench -n 20`; runs, no pass/fail check (also `-alt`, `-c4`, `-c4-alt`) |
| `make test-c4ix-c4ke` | `bench -q`, `innerbench -B -n 2 -q -T` and `benchtop` under C4IX's C4KE compatibility layer, masked and compared with goldens |
| `make test-c4ix-c4ke-nested` | `innerbench -n 1 -T` under C4IX, nested kernels and all; takes minutes |
| `make bench-c4ix` | C4IX's own benchmark (`src/c4ix/user/bench.c`, the same pi/factorial workload) and boot costs next to C4KE's |

## Layout

| file | what |
|---|---|
| `bench.c` | factorial and pi-digit benchmarks; prints a score, the VM cycles spent and the time taken |
| `innerbench.c` | starts N benchmarks, each in a nested c4m (or c4) running C4KE from source |
| `benchtop.c` | `top` plus a continuous `bench`; usable as an init process |

## Hosts

innerbench's default mode compiles `src/c4ke/c4ke.c` from source with
c4m, which has no preprocessor, so `c4ke.c` must stay compilable without
one. `innerbench` opens `c4m.c`, `load-c4r.c` and `src/c4ke/c4ke.c` by
those names, relative to the working directory: run it from the repo root
(the C4DOS build floppies copy them in under the same names). `-a` matters
only when the nested c4m reads its clock from `/proc/uptime`.

## Limits and known issues

