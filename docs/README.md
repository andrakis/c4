# docs/

The design docs, trackers and histories behind the projects in [src/](../src/README.md).
Most readers want a project's own README first; the doc it links is the
detail behind it. Each doc is one of three kinds:

- **Reference**: how something works. Kept current.
- **Tracker**: a checkbox plan. A box is ticked only on green evidence. Open boxes are real.
- **History**: what happened, kept for the reasoning. It is not a description of the code today.

## Start here

| Doc | Kind | What it is |
|---|---|---|
| [internals.md](internals.md) | reference | How c4m, C4KE and C4IX work inside: traps, the cycle interrupt, custom opcodes, protected mode, the `.c4r` format, and how c4lm stalled |
| [MILESTONES.md](MILESTONES.md) | history | What was built, in order, per component |
| [windows.md](windows.md) | reference | Building and running the whole suite natively on Windows, under MSYS2 |

## Machines

| Doc | Kind | What it is |
|---|---|---|
| [quake-performance.md](quake-performance.md) | results | What each compiler and interpreter change gained Quake, by instructions and by the wall |
| [duke3d-port.md](duke3d-port.md) | design, results | Duke Nukem 3D's port: which source and why, the plan, what happened, and its speed work (2026-11-10 to 17) |
| [accel.md](accel.md) | reference, results | Accelerators: a game's heaviest loops run by the host in the game's own JavaScript (`games/NAME/c4/accel.mjs`), loaded by libjs behind INFO's `C4I_ACCEL`; Duke's, Quake's, ROTT's, Doom's, Wolf3D's, Tyrian's and Super Blit Kart's, their menu switches, what they do to the frame rates; and C4IX's own (`src/c4ix/accel.mjs`: libarc's CRC-32 and inflater) |
| [framebench.md](framebench.md) | reference, results | `tools/framebench.mjs`: the games' frame rates, lows and instructions a frame; where the heavy frames go; hand-written code against a drawing opcode; input lag |
| [fused-opcodes.md](fused-opcodes.md) | reference | The ten fused opcodes (79–88), which host runs which, and `-mfuse` |
| [c4-16bit.md](c4-16bit.md) | reference | Opcodes 90–94: 16-bit loads and stores, a logical shift right and an unsigned byte load, in place of c4lc's and c4fc's helpers (2026-11-13) |
| [c4mpg-design.md](c4mpg-design.md) | tracker | c4mpg, the memory-protection guard (M4 and M6 open) |
| [oisc4-design.md](oisc4-design.md) | reference | oisc4, the one-instruction computer |
| [c4bb-design.md](c4bb-design.md) | reference | c4bb: memory map, devices, microcode, traps, firmware, the board |
| [c4bb-storage.md](c4bb-storage.md) | tracker (done) | Drives, the BIOS, media, the climb, and which rung may use which opcodes |
| [c4bb-uart-block.md](c4bb-uart-block.md) | reference | The UART block write |
| [c4bb-port.md](c4bb-port.md) | history | 2026-08-26: porting the c4m and c4mp opcode changes to the breadboard |
| [libjs-design.md](libjs-design.md) | tracker | libjs, c4m in JavaScript (a performance pass and a media panel open) |
| [c4pc.md](c4pc.md) | reference | c4pc: libjs's engine in C on a bare PC (2026-11-22). C4IX boots in QEMU and DOSBox-X, byte for byte as on libjs; the DOS loader, the tools, the speeds, what is next |
| [c4or1k-design.md](c4or1k-design.md) | reference | c4or1k, jor1k ported to C, M0–M18 |
| [c4gpu-design.md](c4gpu-design.md) | investigation | A GPU-hosted VM: design and predictions only, no code |
| [c4gpu-bench-prompt.md](c4gpu-bench-prompt.md) | investigation | The benchmark brief that goes with it |

## Operating systems

| Doc | Kind | What it is |
|---|---|---|
| [task-memory.md](task-memory.md) | reference | What a C4KE task owns, and who gives it back |
| [c4ix-design.md](c4ix-design.md) | reference | C4IX: why it exists, its modules, syscalls and milestones |
| [c4ix-desktop.md](c4ix-desktop.md) | tracker (done) | The C4IX desktop on libjs |
| [c4ix-cpu.md](c4ix-cpu.md) | tracker (done) | Where C4IX's cycles go: the profiler, CPU accounting, the idle task, and the eleven steps that made it cheaper |
| [c4ix-wolf3d.md](c4ix-wolf3d.md) | design | Wolfenstein 3D on C4IX's whole display (a Terminal window with `--window`): Wolf4SDL by c4lc, the shareware zip unpacked at first run, 8-bit frames through `GUI_BLIT8`, sound through libjs's sound card, and how fast |
| [c4ix-smp.md](c4ix-smp.md) | tracker (done) | Multiprocessing: libjs running several CPUs (simulated, and on workers sharing memory with `Atomics`), a CPU-local window for shared libraries' data, and C4IX on them under one kernel lock (S1–S4) |
| [c4ix-mmu.md](c4ix-mmu.md) | reference, results | C4IX's MMU in libjs (`?mmu=1`): paged virtual memory, a space per task, swap, and what it costs |
| [c4ix-i386-port.md](c4ix-i386-port.md) | investigation | C4IX on a real PC (2026-11-21): a native host that runs C4IX unchanged, a native kernel built by gcc, and translated images compared; milestones, testing in DOSBox and QEMU, x86-64, and a Linux-like layout for the kernel source |
| [c4-explorer.md](c4-explorer.md) | tracker | Source maps in C4R (S0–S3 built), Code Explorer (C source beside the VM code it became), windowed programs on the C4IX desktop with the IDE as one, and Wolfenstein 3D as an optional repository |
| [busybox-port.md](busybox-port.md) | tracker | Busybox compiled by c4lc for C4IX: the measured compiler, kernel and libc gaps, and the order of work |
| [c4dos-design.md](c4dos-design.md) | reference | C4DOS: the trap-free DOS, its API table and floppies |
| [grin.md](grin.md) | reference | grin, C4DOS's graphical shell in the manner of Windows 3.1: how it is built and works, G0–G7 as built, the games' groups since (2026-11-19 and 20: five groups, game windows with the game's menu bar, the Uploads on B:), and G8 (a system monitor) and G9 (two games at once) planned |
| [grin-plan.md](grin-plan.md) | history | The plan grin was built from, with each milestone's results and measurements |
| [c4dos-ladder.md](c4dos-ladder.md) | reference | The Build Ladder on the page (`?system=c4dos-ladder`): C4DOS builds and boots C4KE, C4KE builds C4IX and boots it with `kboot`, games at each rung, in one session; what each step costs |
| [climbing-the-ladder.md](climbing-the-ladder.md) | reference | The ladder by hand on the breadboard: C4DOS → C4KE → C4IX, each built by the one before |
| [ladder-plan.md](ladder-plan.md) | history | The completed plan for the ladder: the DOS → C4KE hand-over and C4IX built in the machine |
| [dos-rung-fixes.md](dos-rung-fixes.md) | history | The fixes the DOS rung needed on the board |
| [compiler-on-the-board.md](compiler-on-the-board.md) | tracker (done) | Compiling C4KE on the breadboard: c4cc32 against c4fc |

## Compilers and languages

| Doc | Kind | What it is |
|---|---|---|
| [c4sp-design.md](c4sp-design.md) | reference | c4sp, the Lisp: cells, the collector, the CEK evaluator |
| [c4lc-design.md](c4lc-design.md) | reference | c4lc, the C compiler in that Lisp, and how images are built (§13) |
| [c4sc-design.md](c4sc-design.md) | tracker (done) | c4sc: c4lc's Lisp compiled to C, and since 2026-11-17 the host's c4lc (`c4sc-host`, `c4sc-host32`, `C4LC=interp`) |
| [link-fold.md](link-fold.md) | reference | c4rlink keeps one copy of the helpers c4lc puts in every object: how, what must still match, and what it saves per image |
| [c4-float.md](c4-float.md) | tracker (done) | `float` and `double`: softfp (F1), c4lc (F2), the FPU opcode (F3), libc's printf, strtod and math.h (F4), the Calculator (F5), soft against hard (F6) |
| [compiler-speed.md](compiler-speed.md) | tracker | Making the C4IX build fast. Tracks 0 and A1 are mostly done; track A2 (a hand port of c4lc to C) was retired on 2026-08-26, answered by c4fc |
| [gcc-port.md](gcc-port.md) | investigation | Whether porting an old gcc beats growing c4lc (2026-10-24) |
| [inline-small-functions.md](inline-small-functions.md) | tracker | `-minline` in c4lc and c4fc (I6, the fixed point after inlining, is open) |
| [c4th-design.md](c4th-design.md) | reference | c4th, Forth-2012 CORE, and its B-stages |
| [c4th-selfhost.md](c4th-selfhost.md) | tracker (done) | c4th's self-hosting fixed point |
| [c4fc-design.md](c4fc-design.md) | reference | c4fc, the C99 compiler in Forth, and its DSL |
| [c4tui-design.md](c4tui-design.md) | reference | c4tui, the character-cell UI library |

## History only

| Doc | What it is |
|---|---|
| [next-session.md](next-session.md) | What was open on 2026-09-06 |
| [DOCS-PLAN.md](DOCS-PLAN.md) | The documentation pass of 2026-09-26 to 10-24, for `pm-investigation` (superseded) |
| [ports-feasibility.md](ports-feasibility.md) | What the compilers needed for the ports after Wolfenstein 3D |
| [FULL_STACK_ACHIEVEMENT.md](FULL_STACK_ACHIEVEMENT.md) | An early essay. Several of its claims are inaccurate; use MILESTONES.md instead |
