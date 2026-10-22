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
| [DOCS-PLAN.md](DOCS-PLAN.md) | tracker | This documentation pass, before `pm-investigation` merges to master |

## Machines

| Doc | Kind | What it is |
|---|---|---|
| [fused-opcodes.md](fused-opcodes.md) | reference | The ten fused opcodes (79–88), which host runs which, and `-mfuse` |
| [c4mpg-design.md](c4mpg-design.md) | tracker | c4mpg, the memory-protection guard (M4 and M6 open) |
| [oisc4-design.md](oisc4-design.md) | reference | oisc4, the one-instruction computer |
| [c4bb-design.md](c4bb-design.md) | reference | c4bb: memory map, devices, microcode, traps, firmware, the board |
| [c4bb-storage.md](c4bb-storage.md) | tracker (done) | Drives, the BIOS, media, the climb, and which rung may use which opcodes |
| [c4bb-uart-block.md](c4bb-uart-block.md) | reference | The UART block write |
| [c4bb-port.md](c4bb-port.md) | history | 2026-08-26: porting the c4m and c4mp opcode changes to the breadboard |
| [libjs-design.md](libjs-design.md) | tracker | libjs, c4m in JavaScript (a performance pass and a media panel open) |
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
| [c4ix-wolf3d.md](c4ix-wolf3d.md) | design | Wolfenstein 3D in a desktop window: Wolf4SDL by c4lc, the shareware zip unpacked at first run, 8-bit frames through `GUI_BLIT8`, sound through libjs's sound card, and how fast |
| [busybox-port.md](busybox-port.md) | tracker | Busybox compiled by c4lc for C4IX: the measured compiler, kernel and libc gaps, and the order of work |
| [c4dos-design.md](c4dos-design.md) | reference | C4DOS: the trap-free DOS, its API table and floppies |
| [climbing-the-ladder.md](climbing-the-ladder.md) | reference | The ladder by hand on the breadboard: C4DOS → C4KE → C4IX, each built by the one before |
| [ladder-plan.md](ladder-plan.md) | history | The completed plan for the ladder: the DOS → C4KE hand-over and C4IX built in the machine |
| [dos-rung-fixes.md](dos-rung-fixes.md) | history | The fixes the DOS rung needed on the board |
| [compiler-on-the-board.md](compiler-on-the-board.md) | tracker (done) | Compiling C4KE on the breadboard: c4cc32 against c4fc |

## Compilers and languages

| Doc | Kind | What it is |
|---|---|---|
| [c4sp-design.md](c4sp-design.md) | reference | c4sp, the Lisp: cells, the collector, the CEK evaluator |
| [c4lc-design.md](c4lc-design.md) | reference | c4lc, the C compiler in that Lisp, and how images are built (§13) |
| [c4sc-design.md](c4sc-design.md) | tracker (done) | c4sc: c4lc's Lisp compiled to C |
| [c4-float.md](c4-float.md) | tracker (done) | `float` and `double`: softfp (F1), c4lc (F2), the FPU opcode (F3), libc's printf, strtod and math.h (F4), the Calculator (F5), soft against hard (F6) |
| [compiler-speed.md](compiler-speed.md) | tracker | Making the C4IX build fast. Tracks 0 and A1 are mostly done; track A2 (a hand port of c4lc to C) was overtaken by c4sc and is still open |
| [inline-small-functions.md](inline-small-functions.md) | tracker | `-minline` in c4lc and c4fc (I6, the fixed point after inlining, is open) |
| [c4th-design.md](c4th-design.md) | reference | c4th, Forth-2012 CORE, and its B-stages |
| [c4th-selfhost.md](c4th-selfhost.md) | tracker (done) | c4th's self-hosting fixed point |
| [c4fc-design.md](c4fc-design.md) | reference | c4fc, the C99 compiler in Forth, and its DSL |
| [c4tui-design.md](c4tui-design.md) | reference | c4tui, the character-cell UI library |

## History only

| Doc | What it is |
|---|---|
| [next-session.md](next-session.md) | What was open on 2026-09-06 |
| [FULL_STACK_ACHIEVEMENT.md](FULL_STACK_ACHIEVEMENT.md) | An early essay. Several of its claims are inaccurate; use MILESTONES.md instead |
