c4, over-extended
=================

[c4](https://github.com/rswier/c4) is "C in four functions": a self-hosting
C subset compiler and VM in about 500 lines. It was an exercise in minimalism.

This fork is an exercise in over-extending it. Starting from a VM that
unmodified c4 can still run, it has grown a whole stack. That includes three
operating systems, a Lisp, a Forth, and three more C compilers, one written in
that Lisp and one in that Forth. There is a multiprocessor VM, a one-instruction
computer, and a microcoded breadboard CPU that runs in the browser, plus an
OpenRISC emulator that boots Linux. Each OS on the ladder can build the next
from source. On the breadboard, C4DOS builds C4KE, and C4KE builds C4IX.

[`c4.c`](c4.c) itself is kept as upstream wrote it. The only changes are a
portability shim and a little more `-d` debug output.


c4m: an extended c4 that any c4 can run
---------------------------------------

[`c4m.c`](c4m.c) ("C4 Multiload") is c4's compiler and VM plus the machinery
an operating system needs. **It is written in the c4 subset, so any unmodified
c4 can compile and run it:**

	$ ./c4 c4m.c load-c4r.c -- src/tests/hello.c4r
	yello

That is plain c4 interpreting c4m, which loads and runs a relocatable image
built by [c4cc](src/c4cc/). Compiled natively (`gcc -O2 -o c4m c4m.c`), the
same source runs much faster and picks up signals, floats and a real clock.

What c4m adds over c4:

| Area | Additions |
|---|---|
| Compiler | Reads several `.c` files as one program (the last definition wins). `&func` gives a function's address, and any `int *` can be called (`JSRI`/`JSRS`). `switch`/`case`/`break` compile to a jump table (`JMPA`). `static`, `extern` and `__attribute__((…))` are accepted. |
| Traps | `install_trap_handler()` runs C code on an unknown opcode, a cycle interrupt, a signal, a segfault, or a protected-mode violation. The handler receives `mode, a, bp, sp, returnpc` as parameters. **Assigning to them changes where execution resumes**, which is all a task switch needs. |
| Cycle interrupt | `__c4_configure(CONF_CYCLE_INTERRUPT_INTERVAL, n)` gives preemption. `CONF_TRAP_RESTORES_INTERVAL` keeps the interrupt masked for the whole handler. |
| Custom opcodes | `__c4_opcode(args…, OP)` executes any opcode number. Unknown ones trap, so an OS can define its own system calls as opcodes. |
| Protected mode | I/O opcodes executed in user mode trap to the kernel. |
| Library opcodes | `putchar`, `puts`, `memcpy`, `realloc`, `stacktrace`. |
| Introspection | `__c4_info()` returns capability bits (plain c4 underneath, high-resolution timer, signals, float, protected mode, trap handler installed). `__c4_cycles()`, `__time()` and `__opcode("NAME")` are also available. |
| Other | `__c4_jmp`, `__c4_adjust`, and `__c4_invoke` (function pointers on a VM with none, by rewriting a stub). Fused `LDL`/`STL`/`POPA`. `FLT` (binary32 float, native builds only). |

[docs/internals.md](docs/internals.md) explains how each extension works, and
[src/README.md](src/README.md#c4m-and-its-loaders) lists the related files.


Everything degrades gracefully
------------------------------

Every layer asks what it is running on, then either uses a slower fallback or
says plainly what it can't do. It does not crash.

- **One source, two arms.** Plain c4 skips `#` lines but compiles everything
  between them. c4m.c puts its fallbacks in an `#if C4_ONLY` arm, so plain c4
  compiles the fallbacks. A real preprocessor takes the `#else` arm and binds
  the host's clock, signals and `memcpy` instead.
- **Ask before using.** Programs check `__c4_info()` bits before relying on a
  feature. Tools check `C4I_TRAPH` before probing custom opcodes. For example,
  C4KE turns protected mode on only when `C4I_PROT` is set, and uses the
  breadboard's timer only when the board says it has one.
- **Refuse by name.** [`c4l.c`](c4l.c) loads `.c4r` images under plain c4.
  Given an image that needs an opcode plain c4 lacks, it names the
  instruction ("needs JMPA … which plain c4 does not have") and points you
  at c4m. `make test-c4l` pins this.

| Host | What runs |
|---|---|
| native `c4m` | Everything: C4KE and C4IX with preemption, signals, float, real clock |
| `./c4 c4m.c …` (unmodified c4) | Same programs, slower. No signals or float; the clock is `/proc/uptime` (see below) |
| `./c4 c4l.c prog.c4r` | Images that use only c4's own opcodes, for example C4DOS's clockless build |
| 32-bit builds (`c4m32`, `c4cc32`, …) | The images the breadboard and libjs load |
| [c4bb](src/c4bb/) / [libjs](libjs/) | 32-bit c4m in JavaScript: C4DOS, C4KE and C4IX in a browser |

### Time on Windows

Under unmodified c4, c4m reads the time from `/proc/uptime`, because plain c4
has no clock of its own. **Windows has no `/proc/uptime`.** There c4m prints
`c4m: unable to open uptime file` once, and `__time()` returns 0 from then on.
Anything that measures or waits on time stops working. C4KE, for instance,
hangs at boot while measuring instructions per second unless it is started
with `-m` (skip the measurement) or `-c N` (a fixed cycle interval).

A natively compiled c4m from this fork has a real clock: `clock_gettime`, or
`timespec_get` under MSVC. **On Windows, build c4m natively to get working
time measurement.**


The stack
---------

	               c4  (unmodified)
	                │ interprets
	               c4m ──────────── c4mp (SMP), c4mpg (memory guard), oisc4 (one instruction)
	                │ runs .c4r images
	   ┌────────────┼──────────────┬──────────────┐
	 C4KE          C4IX           C4DOS          c4sp · c4th · c4or1k · …
	 (multitasking) (Unix-like)    (single task)

	 the same machine, microcoded:  c4bb (breadboard, browser)   libjs (fast JS interpreter)

**The ladder.** On the breadboard, one power-on can climb from a DOS to a
Unix-like OS, each system compiled by the one before it:

1. C4DOS boots and rebuilds its own compiler to a fixed point.
2. It then compiles C4KE from source and hands its RAM disk to that kernel
   with `dosload`.
3. C4KE unpacks C4IX's source, compiles its modules, links them, and boots
   it.

See [docs/climbing-the-ladder.md](docs/climbing-the-ladder.md).


Components
----------

**Machines**

- [c4m](c4m.c): the extended interpreter above, with its loaders [`load-c4r.c`](load-c4r.c) and [`c4l.c`](c4l.c).
- [c4mpg](src/README.md#c4mpg): c4m with a memory-protection guard on every load, store and buffer.
- [c4mp](src/c4mp/): a multiprocessor c4m-compatible VM (SMP, atomics, fused opcodes) with no built-in compiler.
- [oisc4](src/oisc4/): the whole VM reduced to one instruction. Runs `.c4r` images bit-identically to c4m.
- [c4bb](src/c4bb/): c4m as a microcoded 32-bit breadboard computer, simulated and drawn in the browser.
- [libjs](libjs/): c4m as a direct JavaScript interpreter in a Web Worker, with a display the guest draws into.
- [c4or1k](src/c4or1k/): an OpenRISC emulator (a jor1k port) compiled by c4lc. Boots an unmodified Linux kernel.

**Operating systems**

- [C4KE](src/c4ke/): the C4 Kernel Experiment. A preemptive multitasking kernel written in plain c4, with a shell, ramfs, IPC and a mailbox.
- [C4IX](src/c4ix/): a Unix-like second OS with a VFS, pipes, redirection and protected-mode syscalls. It runs C4KE's programs unmodified and has a windowed desktop on libjs.
- [C4DOS](src/c4dos/): a single-tasking DOS that needs no traps at all, and so runs even under plain c4. It is the ladder's first rung.

**Toolchain**

- [c4cc](src/c4cc/): a C compiler to the relocatable [`.c4r`](src/c4cc/README.md) format, plus `c4rlink`, `c4rstrip` and `c4rdump`.
- [cpp](src/c4dos/): a C preprocessor in c4, byte-for-byte with `gcc -E` on this tree.
- [c4sp and c4lc](src/c4sp/): a Lisp written in c4, and an optimizing C compiler written in that Lisp. c4lc compiles most of the tree's images.
- [c4sc](src/c4sc/): c4lc's Lisp compiled to C, so the compiler runs fast inside the machine. C4IX's `cc` runs it.
- [c4fp](src/c4fp/): IEEE `float` and `double` in software, which c4lc's floating point compiles to ([docs/c4-float.md](docs/c4-float.md)).
- [c4th](src/c4th/): Forth-2012 CORE, with a self-hosting metacompiler.
- [c4fc](src/c4fc/): a C99 compiler written in Forth. Its output is byte-identical to c4lc's.

**Libraries and programs**

- [c4sh](src/c4sh/): C4KE's shell.
- [c4tui](src/c4tui/): a character-cell UI library.
- [wolf3d](games/wolf3d/): Wolfenstein 3D on C4IX, compiled by c4lc, with sound, in a desktop window.
- [include/](include/): the headers every build shares, including `u0.h`, the C4KE user runtime.
- [src/tests](src/tests/) and [src/bench](src/bench/): the test corpus and benchmarks.

**Shelved**

- [c4lm](src/c4lm/): an early microkernel for plain c4. It is on hold; its lesson became `c4l.c`'s invoke stub.
- [docs/c4gpu-design.md](docs/c4gpu-design.md): a GPU-hosted VM. Investigation only, no code.


Quick start
-----------

	make                  # c4, c4m, c4cc, c4rlink and every .c4r image
	make run              # C4KE under native c4m; type `help` or `ls`, try `bench`
	make run-c4           # the same kernel under unmodified c4 (slow)
	make test             # the core suite

	make run-c4ix         # C4IX's shell
	make run-c4dos        # C4DOS's A> prompt
	make run-c4dos-build  # a rung of the ladder: C4DOS builds C4KE from source on its floppy

	make serve-c4bb       # the breadboard: http://localhost:8471/src/c4bb/web/index.html
	make serve-libjs      # libjs and the C4IX desktop: http://localhost:8472/libjs/web/
	                      # (both need `make c4bb-images` first)

Each project's README gives its own targets. The [Makefile](Makefile) has the
rest, with a comment above each rule.


Further reading
---------------

- [src/README.md](src/README.md): every project in more detail, and who compiles whom.
- [docs/internals.md](docs/internals.md): how c4m, C4KE and C4IX work inside.
- [docs/MILESTONES.md](docs/MILESTONES.md): what was built, in order.
- [docs/README.md](docs/README.md): a map of the design docs, trackers and histories.
