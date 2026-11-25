// c4or1k JIT -- M13. Translates guest code into real c4m bytecode at
// runtime, caches the translation per guest PHYSICAL instruction
// address, and invokes the cached block as an ordinary function on
// repeat visits -- instead of re-fetching/re-decoding/re-dispatching
// through cpu_run_batch's switch every time. Three mechanisms were
// verified in isolation before this file existed (docs/
// c4or1k-design.md's M13 section): looking up real opcode numbers at
// runtime via __opcode(), hand-assembling them into a malloc'd
// buffer, and calling that buffer through a plain `int *` function
// pointer (works under both c4m and c4mp, and under c4lc-compiled
// code specifically).
//
// v2 scope: ALU/compare/shift/movhi opcodes, l.mfspr, all loads and
// stores except l.lwa/l.swa (through helpers in jit.c replicating the
// interpreter's DTLB fast path and fault protocol exactly), fused
// self-loops (l.bf/l.bnf back to the block head become an EMITTED
// loop with a per-iteration budget check), and fused jump terminators
// (l.j/l.jal static-target exits, l.jr/l.jalr dynamic-target exits,
// other l.bf/l.bnf two-way exits on the SR_F value captured AT the
// branch) -- so blocks chain through calls, returns, and taken
// branches. Delay slots of fused branches may be memory ops: the
// helpers' `di` argument reproduces the delayed-instruction fault
// protocol (EPCR points at the branch, resume re-executes it).
// Still interpreter-only: l.mtspr, l.sys/l.trap, l.rfe, l.lwa/l.swa,
// mul/div/ff1/fl1, writes to r0, and anything crossing an 8KB page.
//
// Correctness machinery (each concern is real for this workload --
// Linux reuses physical pages for different processes' code, and
// busybox's text arrives in guest RAM via virtio9p writes):
//  - The cache is keyed on PHYSICAL address; every guest RAM store
//    (mem.c's ram_sw/ram_sh/ram_sb -- which is also the virtio DMA
//    path, verified) bumps jit_pagegen[page], and a cached block is
//    only valid while its recorded generation matches. Code changed
//    since translation -> automatic retranslate.
//  - Each block records the guest VIRTUAL pc it was translated for
//    (exits bake pc/nextpc constants), so a second virtual mapping of
//    the same physical page retranslates instead of resuming at the
//    wrong virtual pc.
//  - A block never runs more guest instructions than the caller's
//    remaining batch budget (statically for straight-line blocks,
//    via the emitted budget check for loops), so tick/interrupt
//    cadence is bit-identical to pure interpretation.

// STATUS (M13 v2): correct, fully verified (bit-for-bit against the
// jor1k oracle with blocks/loops/terminators executing; byte-identical
// 60M-instruction boot logs; full 700M-instruction boot to the
// interactive shell) and decisively FASTER: 67.7s vs the same-session
// M12 baseline's 94.5s on the standard 60M-instruction boot (~28%),
// with 56% of guest instructions executing inside blocks. Translation
// is ON by default in the C4OR1K_JIT build (c4or1k-jit.c4r; -nojit
// opts out at runtime). The default and -mcisc images compile the
// hooks out entirely and are unchanged from M12. History, per-stage
// numbers (v1 was net SLOWER at 5.8% coverage -- the whole v2 design
// came from that measurement), and the remaining v3 ideas live in
// docs/c4or1k-design.md's M13 section.
void jit_init(int enable);

// Called from cpu_run_batch once per loop iteration, before the
// normal fetch/decode path. Returns the number of guest instructions
// executed (cpu state -- r[], SR flags, pc, nextpc -- already updated
// exactly as the interpreter would have left it), or 0 if nothing ran
// (ineligible address, delay slot, ITLB cache not primed for this
// page, block longer than the remaining batch budget, or halt_pc
// falls inside the block -- all of which fall back to normal
// interpretation of one instruction, after which jit_try is simply
// asked again at the next pc).
int jit_try(int halt_pc, int max_run);

// One generation counter per 8KB guest physical page, bumped by every
// guest RAM store (mem.c) so stale translations self-invalidate.
// Allocated by jit_init -- which main.c calls immediately after
// mem_init(), before anything (kernel loader, test-program loader,
// virtio) can store to guest RAM.
extern int *jit_pagegen;

// Execution statistics, maintained by jit_try and reported by main.c
// after a boot-mode run (on the `c4or1k:` prefix, which the boot-log
// diffing methodology already filters out).
extern int jit_nblocks, jit_ninstr;
