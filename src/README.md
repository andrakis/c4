# The projects

Every project in the tree, grouped by what it is. For each: what it does, why
it exists, how to build and test it, how it behaves on a lesser host, and
where its own README and design doc are. The top-level
[README](../README.md) has the overview. Each linked README has the detail.

- [Machines](#machines): [c4m](#c4m-and-its-loaders), [c4mpg](#c4mpg), [c4mp](#c4mp), [oisc4](#oisc4), [c4bb](#c4bb), [libjs](#libjs), [c4or1k](#c4or1k)
- [Operating systems](#operating-systems): [C4KE](#c4ke), [C4IX](#c4ix), [C4DOS](#c4dos)
- [Toolchain](#toolchain): [c4cc and `.c4r`](#c4cc-and-the-c4r-format), [cpp](#cpp), [c4sp and c4lc](#c4sp-and-c4lc), [c4sc](#c4sc), [c4th](#c4th), [c4fc](#c4fc)
- [Libraries and programs](#libraries-and-programs): [c4sh](#c4sh), [c4tui](#c4tui), [include/](#include), [tests and benchmarks](#tests-and-benchmarks)
- [Shelved](#shelved): [c4lm](#c4lm), [c4gpu](#c4gpu)
- [Who compiles whom](#who-compiles-whom)


## Machines

### c4m and its loaders

**Files:** [`c4m.c`](../c4m.c), [`c4m_float.c`](../c4m_float.c), [`load-c4r.c`](../load-c4r.c), [`c4l.c`](../c4l.c).

`c4m.c` is c4 plus traps, a cycle interrupt, custom opcodes, protected mode,
function pointers, `switch`, multi-file loading and library opcodes. It is
written in the c4 subset, so unmodified c4 can run it. The feature table is
in the [top-level README](../README.md#c4m-an-extended-c4-that-any-c4-can-run).

- `c4m_float.c` is the native-only handler for the `FLT` opcode. Under plain
  c4, `FLT` traps as an illegal opcode, so a guest can emulate floats itself.
- `load-c4r.c` loads and runs a [`.c4r`](c4cc/README.md) image under c4m:
  `./c4m load-c4r.c -- prog.c4r`. C4KE includes it as its program loader.
- `c4l.c` is a `.c4r` loader for **plain** c4: `./c4 c4l.c prog.c4r`. It
  refuses an image that needs an opcode past c4's `EXIT` and names the
  instruction.

**Build:** `make c4m`, or `make c4m32` for the 32-bit build the breadboard is
checked against.

**Test:** `make test-c4m-mem` (the same program under c4m and under plain c4)
and `make test-c4l` (where plain c4's ability stops). Both are part of
`make test`.

**Hosts:**

- A native build has a real clock, signals and `FLT`.
- Under plain c4, the clock is `/proc/uptime`. Where that file is missing
  (Windows), time reads as 0; see
  [Time on Windows](../README.md#time-on-windows). There are no signals, and
  `memcpy` is written in C.

**Docs:** [docs/internals.md](../docs/internals.md) Part 3,
[docs/fused-opcodes.md](../docs/fused-opcodes.md).

### c4mpg

**Files:** [`c4mpg.c`](../c4mpg.c).

A marked fork of `c4m.c` that checks every load, store and syscall buffer
against a region table, and raises `TRAP_MPG_VIOLATION` on a miss. It is a
fork rather than an `#ifdef` because plain c4 reads `c4m.c` as source and
compiles both arms of every `#if`. [`src/tests/mpg/check-fork.sh`](tests/mpg/check-fork.sh)
strips the `//>>> c4mpg` blocks and requires the rest to match `c4m.c` byte
for byte. It costs about 1.23× over c4m on innerbench.

**Build:** `make c4mpg`, `make c4mpg32`.

**Test:** `make test-mpg`.

**Docs:** [docs/c4mpg-design.md](../docs/c4mpg-design.md). Per-task regions in
C4KE are not built yet.

### c4mp

**Folder:** [c4mp/](c4mp/).

"C4 MultiProcessor": a c4m-compatible VM with no compiler. It only loads
`.c4r` images. Because it is compiled by c4lc rather than read by plain c4,
it is free to use structs and a preprocessor. It has:

- a deterministic simulated SMP round robin
- atomics and park/wake opcodes
- all ten fused opcodes, and the CISC array opcodes `LXI`/`SXI`
- a raw terminal

It runs c4or1k about 18–20% faster than c4m does. It builds two ways: natively
as `./c4mp`, and as `c4mp.c4r`, which c4m can host.

**Build:** `make c4mp`, `make c4mp.c4r`.

**Test:** `make test-c4mp`.

### oisc4

**Folder:** [oisc4/](oisc4/).

A one-instruction computer: `add [src], literal, [dst]` with sign flags. It
translates any `.c4r` onto that instruction and runs it bit-identically to
c4m. The self-hosted c4 and the c4sp Lisp both run inside it. A c4lc-built
oisc4 runs nested under c4m, under plain c4, and under itself.

**Build:** `make oisc4`.

**Test:** `make test-oisc4`, `make test-oisc4-nested`.

**Docs:** [docs/oisc4-design.md](../docs/oisc4-design.md).

### c4bb

**Folder:** [c4bb/](c4bb/).

The c4m VM rebuilt as a microcoded 32-bit breadboard computer, in the spirit
of Ben Eater's. The microcode ([`hw/microcode.uc`](c4bb/hw/microcode.uc))
defines the CPU one bus transfer at a time. A JavaScript simulator runs it,
checked instruction for instruction against native `c4m32`, and the browser
draws the board lit by the signals.

Boot firmware in guest code ([`fw/`](c4bb/fw/)) provides malloc and printf.
The board boots C4DOS, C4KE and C4IX, with drives, a keyboard and preemption.
It is the machine the ladder climbs on.

**Build:** `make c4bb-32bit c4bb-images`.

**Run:** `make serve-c4bb`, then open http://localhost:8471/src/c4bb/web/index.html.
For a headless run, use `node src/c4bb/sim/cli.js`.

**Test:** `make test-c4bb` and its `test-c4bb-*` siblings.

**Docs:** [docs/c4bb-design.md](../docs/c4bb-design.md).

### libjs

**Folder:** [../libjs/](../libjs/).

c4m as a direct JavaScript interpreter: a single switch over an
`Int32Array`, at 106–190M instructions per second in Node. It boots the
32-bit C4KE and C4IX images in Node or in a browser Web Worker. The page
has a terminal and a display that the guest draws into through
[`guest/gui.h`](../libjs/guest/gui.h); C4IX's windowed desktop runs on
that display. It reuses c4bb's memory, devices and loader.

**Run:** `make c4bb-images serve-libjs`, then open http://localhost:8472/libjs/web/.

**Test:** `make test-libjs`, and `make test-libjs-web` in a real browser over
CDP.

**Docs:** [docs/libjs-design.md](../docs/libjs-design.md).

### c4or1k

**Folder:** [c4or1k/](c4or1k/).

An OpenRISC (OR1000) emulator ported from jor1k to the C dialect c4lc
compiles. It has 9p root, UARTs, ethernet with a synthetic LAN, and an FPU,
and it boots an unmodified Linux kernel to a shell.

It runs hosted under c4m or c4mp. It also builds natively with gcc, and
there is a JIT build that emits c4m bytecode. The kernel and filesystem come
from a separate jorconsole checkout next to this repo.

**Build:** `make c4or1k.c4r` or `make c4or1k-native`.

**Run:** the `c4or1k-boot*` targets.

**Docs:** [docs/c4or1k-design.md](../docs/c4or1k-design.md).


## Operating systems

### C4KE

**Folder:** [c4ke/](c4ke/).

The C4 Kernel Experiment, and the original project: "an exploration in
over-extending minimalism". It is a preemptive multitasking kernel written in
plain c4. c4m's cycle interrupt and unknown-opcode traps enter the kernel.
The kernel switches tasks by assigning another task's registers to the trap
handler's own parameters.

It comes with init, a service manager, a RAM filesystem, IPC, a mailbox,
[c4sh](#c4sh), and a userland of `.c4r` programs including a line editor and
the toolchain. `c4ke.c` must still compile without a preprocessor, because
innerbench compiles it from source inside a nested c4m.

**Run:** `make run` (native c4m) or `make run-c4` (unmodified c4 → c4m).

**Test:** `make test`, plus `test-c4ke-ramfs`, `test-task-mem`, `test-link`
and `test-b4ke`.

**Docs:** [docs/internals.md](../docs/internals.md) Parts 4 and 7,
[docs/task-memory.md](../docs/task-memory.md).

### C4IX

**Folder:** [c4ix/](c4ix/).

A Unix-like second-generation OS, written in c4lc's C (structs, typedef) as
twelve modules linked by c4rlink. System calls own all I/O through
protected-mode traps, which gives file descriptors, pipes, redirection and a
real shell.

A compatibility layer runs C4KE's binaries unmodified. On the libjs display it
has an NT-style desktop: windows, a taskbar, terminals, Explorer, Task Manager,
Notepad, Calculator and Run. Compilers run inside it.

**Run:** `make run-c4ix`, or `make run-c4ix-c4` under unmodified c4 → c4m.

**Test:** `make test-c4ix`, `test-c4ix-c4ke`.

**Docs:** [docs/c4ix-design.md](../docs/c4ix-design.md),
[docs/c4ix-desktop.md](../docs/c4ix-desktop.md).

### C4DOS

**Folder:** [c4dos/](c4dos/).

A single-tasking, trap-free DOS: `CONFIG.SYS`, `AUTOEXEC.BAT`, an `A>` prompt,
a RAM disk and transient programs. It uses nothing beyond c4's own opcodes,
so its clockless build runs under plain c4 through `c4l.c`. It is the
ladder's first rung.

Its floppies rebuild its own compiler, compile C4KE from source, and hand
over to it with `dosload`.

**Run:**

- `make run-c4dos`
- `make run-c4dos-c4` (plain c4)
- `make run-c4dos-bb` (breadboard)
- `make run-c4dos-build` (C4DOS builds C4KE)

**Test:** `make test-c4dos`, `test-cpp`, `test-dosload`. The slow ones are
`test-ladder` and `test-c4dos-c4ix32`.

**Docs:** [docs/c4dos-design.md](../docs/c4dos-design.md),
[docs/climbing-the-ladder.md](../docs/climbing-the-ladder.md).


## Toolchain

### c4cc and the `.c4r` format

**Folder:** [c4cc/](c4cc/).

c4cc is c4's front end with a pluggable back end. It adds `for`, `continue`,
`switch`, variadics, `static`/`extern` and constructor attributes. It writes
**`.c4r`**, a relocatable image with code, data, a patch table, symbols, and
constructor and destructor lists. The patch table is what lets a kernel load
an image anywhere without guessing, and what lets `c4rlink` link objects
(`.c4o`) and archives (`.c4l`).

The companion tools live in [c4ke/bin/](c4ke/bin/) and run both natively and
inside the OSes:

- `c4rlink`: the linker
- `c4rstrip`: the same program under another name
- `c4rdump`: dumps an image

c4cc has no preprocessor: builds run `gcc -E` or [cpp](#cpp) first. It is
fast, compiling C4KE in about 124M instructions.

**Build:** `make c4cc`, `make c4cc32`, `make c4cc.c4r`.

### cpp

**File:** [`c4dos/cpp.c`](c4dos/cpp.c).

A C preprocessor written in c4, byte-for-byte with `gcc -E` on this tree. It
is the toolchain's preprocessor inside C4DOS and C4KE.

**Build:** `make cpp` (native), `make cpp.c4r`.

**Test:** `make test-cpp`.

### c4sp and c4lc

**Folder:** [c4sp/](c4sp/).

**c4sp** is a Lisp written in c4. It has a mark-and-sweep collector, a CEK
evaluator with `call/cc`, and binary32 floats.

**c4lc** is an optimizing C compiler written in that Lisp. It supports
structs, typedef, its own preprocessor, separate compilation, tree passes and
a peephole optimizer, and it has flags for the fused and CISC opcodes and for
inlining. Its images are 18–27% smaller than c4cc's.

c4lc compiles C4IX, C4DOS, c4mp, c4or1k, c4tui and the 32-bit c4th. The
standard way to build an image is per-module `c4lc -c` into `.c4o` units,
then `c4rlink` (`-s` to strip).

**Build:** `make c4sp`.

**Test:** `make test-c4sp`, `make test-c4lc`.

**Docs:** [docs/c4sp-design.md](../docs/c4sp-design.md),
[docs/c4lc-design.md](../docs/c4lc-design.md).

### c4sc

**Folder:** [c4sc/](c4sc/).

Transliterates c4lc's Lisp into C over c4sp's runtime, giving "c4lc,
compiled". This cut the in-machine C4IX build on the breadboard from about
31 minutes to about 4.5.

**Build:** `make c4sc.c4r`.

**Test:** `make test-c4sc`.

**Docs:** [docs/c4sc-design.md](../docs/c4sc-design.md).

### c4th

**Folder:** [c4th/](c4th/).

Forth-2012 CORE with indirect threading. Its Forth library adds an assembler,
a peephole optimizer, a native back end, a `.c4r` metacompiler, and a
self-hosting Forth compiler that reaches a fixed point.

**Build:** `make c4th`.

**Test:** `make test-c4th`.

**Docs:** [docs/c4th-design.md](../docs/c4th-design.md),
[docs/c4th-selfhost.md](../docs/c4th-selfhost.md).

### c4fc

**Folder:** [c4fc/](c4fc/).

A C99 compiler written in Forth as a table-driven DSL on c4th. Its output is
byte-identical to c4lc's. It exists so that every system that runs c4th has
a C compiler that doesn't need Lisp.

**Test:** `make test-c4fc`.

**Docs:** [docs/c4fc-design.md](../docs/c4fc-design.md).


## Libraries and programs

### c4sh

**Folder:** [c4sh/](c4sh/).

C4KE's shell. It has builtins, `&` background jobs, quoting, and
non-blocking input.

### c4tui

**Folder:** [c4tui/](c4tui/).

A character-cell UI library in strict c4, with damage tracking and one write
per frame.

**Test:** `make test-c4tui`.

**Docs:** [docs/c4tui-design.md](../docs/c4tui-design.md).

### include

**Folder:** [../include/](../include/).

The headers every build shares:

- libc shims
- VM detection (`c4.h`, `c4m.h`)
- `u0.h`, the C4KE user runtime, and `u0lite.h` for programs that must also
  run on C4DOS
- the C4DOS, breadboard and mailbox APIs

### Tests and benchmarks

**Folders:** [tests/](tests/), [bench/](bench/).

`src/tests` holds the VM corpus. Every machine (c4bb, oisc4, c4mp, libjs) is
checked against it for identical output. It also holds the
compiler-in-the-machine tests, c4lc's tests, the linker tests, a raycaster,
and a Mandelbrot. `src/bench` has `bench`, `benchtop` and `innerbench`
(nested c4m instances, one of them compiling C4KE from source).


## Shelved

### c4lm

**Folder:** [c4lm/](c4lm/).

An early attempt at multitasking under plain c4, without c4m. It could not
switch tasks more than twice. The diagnosis became `c4l.c`'s invoke stub.

### c4gpu

[docs/c4gpu-design.md](../docs/c4gpu-design.md): an investigation into
running many VMs as GPU lanes. There is no code.


## Who compiles whom

- **c4** (unmodified) interprets **c4m.c**, which runs everything below.
- **c4cc** builds C4KE (after `gcc -E`), its userland, c4sh, `c4th.c4r`,
  `cpp.c4r`, and `c4cc.c4r`. `c4cc32` builds the 32-bit userland.
- **c4sp** runs **c4lc**, which builds C4IX, C4DOS, `c4sp.c4r`, c4th32,
  c4tui, c4mp, c4or1k and oisc4's self-hosted image.
- **c4sp + c4sc** turn c4lc into C, which builds `c4sc.c4r` for the fast
  in-machine C4IX build.
- **c4rlink** links C4IX, `libc4ix.c4l`, c4sc, c4mp, c4or1k and the desktop.
- **c4th** hosts **c4fc**, which can compile c4th itself, C4KE, C4DOS and
  C4IX modules.
- **The ladder:**
  1. **C4DOS**, using `cpp.c4r` and `c4cc.c4r`, builds **C4KE**, then hands
     over with `dosload`.
  2. **C4KE**, using `tar`, `b4ke`, `c4sc.c4r` and `c4rlink`, builds **C4IX**.
