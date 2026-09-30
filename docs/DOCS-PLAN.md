# DOCS-PLAN — document pm-investigation, then merge it to master

`pm-investigation` is 255 commits ahead of `master` and master has nothing it lacks, so the
merge is a fast-forward. Before it happens, the documentation has to describe what the branch
actually built: most of it lives only in commit messages and per-component design docs that
stopped being updated weeks ago. This file is the tracker (Muster reads it): tick a box only
when the doc is written and checked against the code.

Surveyed 2026-09-26. What exists today, per component, is noted under each milestone.

## D0 — The index: a reader can find everything
- [x] README.md: a component index — c4m, c4mp, c4mpg, c4th, c4fc, c4lc (+ c4sp), c4dos, C4KE, C4IX, c4bb, libjs, c4or1k, oisc4 — one line each, a link to its doc, and how to build/run it
- [x] README.md: drop the stale bits ("preprocessor soon" — the branch has cpp)
- [x] docs/README.md: a map of docs/ — which docs are current references, which are trackers, which are historical (next-session.md, FULL_STACK_ACHIEVEMENT.md, dos-rung-fixes.md)
- [x] docs/MILESTONES.md: add sections for c4th, c4fc, libjs, c4mp, c4mpg, c4dos (today it has none of them, and c4dos one mention)
- [x] docs/internals.md: fix the section order (§4.6 sits after Part 7) and add pointers to the components it never mentions

## D0a — READMEs (2026-09-30)
Tick a box only when the file's links resolve and every make target it names passes `make -n`.
- [x] fixes found on the way: `inline.f` in C4FC_FORTH, the duplicate `c4lc_compile_par`, two stale Makefile comments, the unused root package.json
- [x] README.md — rewritten: c4m first (runs under any unmodified c4), graceful degradation, the Windows clock, the ladder, component index
- [x] src/README.md — per-project detail and who-compiles-whom
- [x] src/c4ke/README.md (+ bin/, extensions/)
- [x] src/c4ix/README.md (+ user/)
- [x] src/c4dos/README.md (+ fs/)
- [x] src/c4cc/README.md (the .c4r format, c4rlink/c4rstrip/c4rdump)
- [x] src/c4sp/README.md (+ lisp/) — c4sp and c4lc
- [x] src/c4sc/README.md
- [x] src/c4th/README.md (+ forth/)
- [x] src/c4fc/README.md
- [x] src/c4sh/README.md
- [x] src/c4tui/README.md
- [x] src/c4mp/README.md (stands in for the never-written c4mp design doc)
- [x] src/oisc4/README.md (takes the OISC explainer from the old root README)
- [x] src/bench/README.md
- [x] src/tests/README.md
- [x] src/c4lm/README.md
- [x] src/c4bb/README.md rewritten (+ hw/, fw/, sim/, web/, tests/, fs/, tools/)
- [x] src/c4or1k/README.md refreshed
- [x] libjs/README.md rewritten (+ guest/)
- [x] include/README.md

## D0b — what the README pass found (2026-09-30)
- [x] test-c4dos-c4fc green. The image c4fc wrote was fine; C4DOS was not. c4lc -O made invoke0's zero-argument `return stub()` a tail call (JMP stub+2) that skipped the JMP the stub had rewritten itself into, re-ran the stub's body and rewrote code near the caller -- so after any program with a destructor (c4th: stdarg.h's), the next program ran silent or crashed. invoke0 passes a dummy argument now (c4dos.c, c4l.c, and include/c4dos.h's __c4dos_call0, which had the same shape)
- [x] src/c4dos/fs/CC.BAT loads the one concatenated c4fc.f (it passes 16 separate .f files, past ARGVMAX, and misses inline.f); the test types CC now, so CC.BAT is what is tested
- [x] innerbench -V no longer passes an option C4KE rejects; -d back in its help
- [x] C4LC_LISP lists c4lc-pp.lisp, so a preprocessor edit rebuilds what c4lc built
- [x] c4bb names the fused opcodes 79-88 on the board and in the panels (opname stops at 66); stale comments in sim/devices.js and sim/arena.js
- [x] no private-project name in the Makefile, code comments or docs; the ladder's tracker is docs/ladder-plan.md (nothing outside this repo pointed at the old name)
- [x] drifted design docs brought current — each one checked against the code:
  - [x] internals.md: §6.5 (c4.c is unmodified again), Part 5 (c4cc has switch/break/arrays), the section order
  - [x] c4cc/.c4r: asm-c4r.c's "V3 unimplemented"
  - [x] c4sp-design.md (growing arena, -O2 spill) and c4lc-design.md (-mcisc/-mfuse/-minline, the 09-07..10 fixes, the c4lc-pp header)
  - [x] c4sc-design.md intro ("nothing built yet")
  - [x] c4th-design.md §2 layout
  - [x] c4fc-design.md (inlining, the module list) and inline-small-functions.md ("I7 still to do")
  - [x] c4dos-design.md (status line, the pieces, ram:name, the tools)
  - [x] c4ix-design.md (after X6: sleep, gui, the desktop, compilers inside C4IX, lazy disk) and c4ix-desktop.md (desktop/ folder)
  - [x] c4bb-design.md (fused opcodes, printf via UART block write, 20 MHz default) and fused-opcodes.md's c4bb section
  - [x] libjs-design.md layout (shared.js, fb-demo.c)
  - [x] c4or1k-design.md opening (raw mode, c4or1k-m0)
  - [x] oisc4-design.md (fused opcodes)
  - [x] c4tui-design.md (browser keys work)
  - [x] c4mpg-design.md status line
  - [x] MILESTONES.md: the src/c4lc path, the realloc note, sections for c4th, c4fc, libjs, c4mp, c4mpg, c4dos
  - [x] stale source headers: c4m.c (__builtin), c4sp.c, c4sh.c, c4ke.c, c4ke_pm.c, c4lc-pp.lisp, c4dos.c, c4ix.h, c4mp.h, serve.mjs
- [x] found on the way: the c4dos-c4fc floppy carried 64-bit c4rlink/dostar/dosload for a 32-bit DOS; it carries the 32-bit builds now
- [x] found on the way: test-c4th-os's C4DOS leg failed at byte 1 -- DOS's bare `A>` prompt line; the filter drops it now
- [x] found on the way: `c4rdump -c` segfaulted on c4lc images (it skipped word 0, which c4lc uses, then read operands as opcodes off the end of the name table); it starts at an ENT, reads the fused opcodes' operands and bounds-checks
- [ ] still open: test-c4ke-mbox fails about one run in four (its "kernel shut down cleanly" check; mbpair prints "Custom opcode not found: 1073741824")

## D1 — c4m: the extended interpreter
Today: README "C4M?" section; internals Part 3 (§3.1–3.6); docs/fused-opcodes.md.
- [x] internals Part 3 brought up to date with the fused opcodes (split into c4m 2026-08-24) and the cycle interrupt
- [x] one place that lists every opcode/trap c4m adds over c4, with the version it arrived in

## D2 — c4mp: the separate C machine
Today: no doc of its own — only mentioned from fused-opcodes, libjs, c4th, c4fc, c4dos docs.
- [x] (as src/c4mp/README.md rather than docs/c4mp.md) what it is (VM, SMP, loader, the trampoline for image constructors), why it exists next to c4m, its seven fused opcodes, how to build and run it
- [x] README + MILESTONES entries

## D3 — c4mpg: the memory-protection guard
Today: docs/c4mpg-design.md (status line 2026-09-06: M1, M2, M5 and M3's trap half built).
- [x] design doc status checked against the code and brought current
- [x] README + MILESTONES entries

## D4 — c4th: Forth-2012 CORE
Today: docs/c4th-design.md, docs/c4th-selfhost.md (both 2026-08-26, match the source).
- [x] a user-facing "how to run it / what it supports" section (the design docs are for implementers)
- [x] c4th inside C4IX (e427c9c) documented
- [x] README + MILESTONES entries

## D5 — c4fc: the C99 compiler in Forth
Today: docs/c4fc-design.md (2026-08-26) — predates inlining (d445540, 2026-09-07).
- [x] design doc updated for inlining (fold in or link docs/inline-small-functions.md) and use inside C4IX
- [x] README + MILESTONES entries

## D6 — c4lc: the C compiler in c4sp Lisp
Today: docs/c4lc-design.md (2026-08-23, "L0–L6 done"); MILESTONES c4lc section.
- [x] design doc covers L7+ (preprocessor, -conforming escapes, enum expressions) and the 2026-09-07..10 fixes (#else in skipped text, swallowed #include, codegen error naming, inlining)
- [x] "c4lc units + c4rlink32 -s" as THE way to build images, written down in one place

## D7 — c4dos: the single-tasking disk OS
Today: docs/c4dos-design.md (status line "in development, 2026-08-21"), dos-rung-fixes.md, ladder-plan.md, climbing-the-ladder.md.
- [x] status line and design doc updated: the DOS rung, INSTALL and the three power-ons all landed
- [x] README + MILESTONES entries; dos-rung-fixes.md marked historical

## D8 — C4KE: the kernel
Today: README (architecture, running); internals Parts 4 and 7; MILESTONES; docs/task-memory.md.
- [x] the mailbox extension (ad57ad0), wall-clock scheduling and task memory documented in internals
- [x] README's C4KE sections checked against the current kernel

## D9 — C4IX: the second-generation OS
Today: docs/c4ix-design.md (2026-08-09), docs/c4ix-desktop.md (2026-09-24, a current tracker).
- [x] design doc covers what came after 08-09: sleep syscall, `gui`, the desktop, compilers inside C4IX
- [x] README + internals §7.4 refreshed

## D10 — c4bb: the breadboard computer
Today: docs/c4bb-design.md (2026-09-10, roughly current), c4bb-port.md, c4bb-storage.md, c4bb-uart-block.md, compiler-on-the-board.md, src/c4bb/README.md.
- [x] design doc checked against the work after 09-10
- [x] top-level README entry (today c4bb is only in MILESTONES)

## D11 — libjs: c4m in JavaScript
Today: docs/libjs-design.md (2026-09-23, a current tracker; open: performance pass, media panel), libjs/README.md.
- [x] README entry beyond the one line under "Other experiments"
- [x] internals + MILESTONES entries

## D12 — Review and merge
- [ ] the user reads the index and the refreshed docs (the user's call)
- [ ] merge to main: fast-forward master to pm-investigation and push (the user's call)
