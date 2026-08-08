// c4or1k CPU core -- shared declarations.
//
// M1 implements the full non-privileged OR1000 integer ISA (arith,
// shift, compare, branch, load/store, jump, mul/div, ff1/fl1,
// lwa/swa) with no MMU and no SPRs/exceptions yet -- physical ==
// virtual, and l.mfspr/l.mtspr/l.rfe/l.sys/l.trap all fall through to
// the "unimplemented" fault path (M2 gives them real bodies).
//
// Field positions and opcode/func numbers are read directly from
// jor1k/js/worker/or1k/safecpu.js (not the OR1000 spec), because M1's
// decoder is cross-checked against that file as a bit-for-bit oracle
// (tools/or1k-oracle.js). Where safecpu.js's own comment disagrees
// with its code (case 0x1 under 0x2E is commented "rori" but the code
// is `>>>`, a logical shift, not a rotate), the code wins and the
// mnemonic here matches the code, not the comment.
//
// sext()'s existence is not cosmetic: see docs/c4or1k-design.md's
// "Language notes" for the M0 bug it fixes. Every place below that
// pulls a signed field out of an instruction word calls it; nothing
// uses the shift/truncate idiom, because that idiom is silently wrong
// under c4lc's 64-bit `int`.

enum { NREGS = 32 };

// Registers, SR_F, and the pc/nextpc pair are cpu.c's; declared here
// so main.c's test harness can read them for register-dump output.
extern int r[NREGS];
extern int SR_F, SR_CY, SR_OV;
extern int pc, nextpc;
extern int EA; // reservation address for l.lwa/l.swa

int sext(int v, int bits);

// Runs one instruction (fetched from ram[pc<<2..pc<<2+3]).
// Returns 0 to keep running, 1 once pc reaches halt_pc (without
// executing it), 2 if the fetched opcode isn't implemented yet (state
// has already been dumped to stdout when this happens).
int cpu_step(int halt_pc);

void cpu_reset();
void cpu_dump();
