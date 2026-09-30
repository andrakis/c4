# C4KE — a preemptive multitasking kernel for the c4m VM

C4KE (the C4 Kernel Experiment) is a small multitasking kernel written in
the plain c4 subset of C. It runs as an ordinary program on c4m, the
extended c4 interpreter in the repo root, and schedules tasks by swapping
registers inside c4m's trap handlers: a custom opcode (`schedule()`) raises
a trap for a cooperative switch, and c4m's cycle interrupt raises the same
kind of trap for a preemptive one. A trap handler receives the interrupted
registers as its parameters; assigning to them and returning is the context
switch. There is no second VM: each task is c4 code running in the one c4m.

Programs are `.c4r` images (built by c4cc against `include/u0.h`, the user
runtime) that the kernel loads and calls directly. Filesystems and other
services run as tasks rather than in the kernel. C4KE is the middle rung of
the ladder (C4DOS → C4KE → C4IX): C4DOS can build it from source and hand it
the machine, and it can in turn build C4IX (see
[docs/climbing-the-ladder.md](../../docs/climbing-the-ladder.md)).

## Build

| target | produces |
|---|---|
| `make` / `make pre` | the native tools (`c4`, `c4m`, `c4cc`, `c4rdump`, `c4rlink`), `c4ke.c4r`, `init.c4r`, `c4sh.c4r`, `c4ke.vfs.c4r`, the userland in `bin/`, the benchmarks, the test corpus; then copies `src/c4ke/bin/*.c4r`, `src/bench/*.c4r` and `src/tests/*.c4r` into the repo root, where the kernel looks for them |
| `make c4ke.c4r` | the kernel: `gcc -E src/c4ke/c4ke.c \| ./c4cc -o c4ke.c4r -` |
| `make c4ke-pm.c4r`, `c4ke32-pm.c4r`, `c4ke32-nopm.c4r` | kernels with protected mode forced on (`-DCONFIG_ENABLE_PM=1`) or off, 64- and 32-bit; used by `test-task-mem` |
| `make c4ke-lc.c4r` | the kernel compiled by c4lc `-O` instead of c4cc |
| `make c4ke-src.tar` | the build kit C4DOS unpacks to compile the kernel in-machine |

The extensions in [extensions/](extensions/README.md) are `#include`d at the
end of `c4ke.c`, so they need the preprocessor pass. `c4ke.c` itself must
still compile with no preprocessor at all: `innerbench` compiles it from
source inside nested c4m instances, and c4m skips `#` lines. Anything the
kernel needs from a header is therefore spelled out in `c4ke.c`.

The 32-bit `c4ke32.c4r` that the breadboard (c4bb) and libjs boot is built by
`src/c4bb/tests/build-images.sh` (`make c4bb-images`), with c4lc under
`c4sp32`.

## Run

    make run            # ./c4m load-c4r.c -- c4ke.c4r
    make run-c4         # ./c4 c4m.c load-c4r.c -- c4ke.c4r   (plain c4 runs c4m; slow)
    make run-alt        # passes -a to c4m (alternate reading of /proc/uptime)
    make run-c4-alt
    make run-vg  run-alt-vg  run-c4-vg  run-c4-alt-vg   # the same under valgrind

The kernel's options, from its `--help`:

    c4ke v0.66: C4 Kernel Experiment
              : [-dtmgO] [-v nn] [-c nn] [--] [init_file.c4r] [arguments...]
               -d               Enable debug mode
               -t               Enable test tasks
               -m               Disable speed measurement
               -g               Load .c4r symbols by default
               -O               Disable C4-specific optimizations
               -v nn            Enable verbose mode and set verbosity (0 - 100, default 50)
               -c nn            Set cycle interrupt count (implies -m)
               --               End arguments
               init_file.c4r    File to use as init process (default: init.c4r)
               arguments...     Arguments to pass to init process
               --help -h        Show this help

Kernel options go after the image: `./c4m load-c4r.c -- c4ke.c4r -v 50`.
The first non-option argument replaces init, so `./c4m load-c4r.c --
c4ke.c4r innerbench` boots straight into a benchmark.

With the default init, `init.c4r` starts the VFS service, runs `vfsload`
to fill the RAM filesystem from `c4ke.vfs.txt`, and then starts
[c4sh](../c4sh/README.md) at a `c4sh>` prompt. If init cannot start the
shell it falls back to the emergency shell (`bin/eshell.c`). Once in, try
`help`, `ls`, `ps`, `top &`, `bench` and `innerbench`. Any `.c4r` can be
named without its extension; `\q` leaves. Input is line-buffered, so other
tasks' output appears when you press Enter.

## Test

| target | what it checks | in `make test`? |
|---|---|---|
| `make test` | `test-c4m-mem`, `test-c4l`, then boots C4KE with `innerbench` as init and fails on any inner compile error or if no inner benchmark completes | — |
| `make test-alt`, `test-c4`, `test-c4-alt` | innerbench with c4m `-a`, and under plain c4 (slow); no pass/fail checks | no |
| `make test-massive` (`-alt`, `-c4`, `-c4-alt`) | innerbench with 20 inner kernels | no |
| `make test-c4ke-ramfs` | the kernel RAM filesystem; c4cc, c4lc and c4sp compiling into it and the kernel running the result from memory; a link inside the machine | no |
| `make test-task-mem` | with protected mode on, the kernel reclaims a task's heap when it ends (runs on c4m and on c4bb, with a control run that must fail) | no |
| `make test-link`, `test-respfile` | c4rlink, including `@file` response files | no |
| `make test-b4ke` | `b4ke` driving a two-module compile and link inside C4KE | no |
| `make test-c4bb` | boots `c4ke32.c4r` on the breadboard; the only test of the mailbox extension (`src/c4bb/tests/test-c4ke-mbox.mjs`) | no |
| `make test-mpg` | `bin/badmem.c` under the c4mpg memory guard | no |

`make test-c4ix-c4ke` runs C4KE's own binaries unmodified under C4IX's
compatibility layer.

## Layout

| path | what |
|---|---|
| `c4ke.c` | the kernel: tasks, scheduler, trap and interrupt handlers, custom opcodes, the `.c4r` loader glue, signals, the RAM filesystem (`OP_VFS_*`), kernel extensions API, command line |
| [`extensions/`](extensions/README.md) | optional kernel modules compiled into the image |
| [`bin/`](bin/README.md) | userland programs and tools run as C4KE tasks |
| `services/init.c` | the init process: starts the VFS service, vfsload, then c4sh (linked with `eshell.c` as a fallback) |
| `services/c4ke.vfs.c` | the VFS service task (`c4ke.vfs.c4r`) |
| `services/ps.service.c` | a process-listing service; not built by the Makefile |
| `include/` | kernel headers (`config.h`, `extension.h`, `service.h`, `opcodes.h`, …). Byte-identical to [`/include/c4ke/`](../../include/README.md), which is the copy the kernel build actually reads (`<c4ke/…>` via `-Iinclude`); `service.h` is read from here. Change both. |
| `fs/c4ix.lst` | the manifest `kinstall C:` reads to write a bootable C4IX medium |
| `stdlib/` | experimental class/vector/map/lambda library for c4; not built or tested |

The shell lives in [src/c4sh](../c4sh/README.md) and the benchmarks in
[src/bench](../bench/README.md).

## Hosts

- **Native c4m** (`make run`): full preemption through the cycle
  interrupt. At boot the kernel measures the host's speed to pick the
  interrupt interval; `-m` skips that, `-c N` sets the interval directly.
- **Plain c4** (`make run-c4`): unmodified `c4` interprets `c4m.c`, which
  runs the kernel. Correct and slow. C4KE needs c4m's trap opcodes, so it
  cannot run under `./c4 c4l.c` directly. Under plain c4, c4m's clock reads
  `/proc/uptime`; where that file does not exist (Windows) `__time()`
  returns 0 and the boot-time speed measurement never finishes. Pass `-m`
  or `-c N` there: `./c4 c4m.c load-c4r.c -- c4ke.c4r -m`. A natively built
  c4m uses `clock_gettime` (`timespec_get` on MSVC) and has no such problem.
- **32-bit / c4bb**: `c4ke32.c4r` boots on the breadboard. When `__c4_info()`
  reports the board's interval timer (bit `0x800`) and it answers a probe,
  the kernel schedules on real milliseconds instead of the cycle counter.
  The mailbox extension is active only when the host announces a mailbox.
- **libjs**: the same 32-bit image boots in the JavaScript interpreter
  ([libjs/README.md](../../libjs/README.md)).
- **Protected mode** is off by default (`CONFIG_ENABLE_PM 0` in
  `include/config.h`). With it on, and only on a c4m that reports
  `C4I_PROT`, user tasks' privileged instructions trap into the kernel,
  which is what lets it reclaim a task's memory. It costs a trap per
  syscall.

## Limits and known issues

- Console input is a blocking line read; there is no cursor editing
  (pressing up then Enter repeats the last command in c4sh).
- Without protected mode the kernel cannot give back memory a task
  allocated and never freed.
- `src/c4bb/tests/test-c4ke-mbox.mjs` (run by `make test-c4bb`) fails intermittently in the mbpair
  step, as recorded in [docs/c4ix-desktop.md](../../docs/c4ix-desktop.md).

## Docs

- [docs/internals.md](../../docs/internals.md) Part 4 (C4KE and the trap
  design), Part 7 (protected mode) and §4.6 (the RAM filesystem, which
  sits after Part 7).
- [docs/task-memory.md](../../docs/task-memory.md): what a task owns and who
  frees it.
- [docs/c4bb-design.md](../../docs/c4bb-design.md), section "The mailbox":
  the host mailbox the `c4ke_mbox` extension drives.
- [docs/climbing-the-ladder.md](../../docs/climbing-the-ladder.md): C4DOS
  building C4KE, and C4KE building C4IX, on the breadboard.
