// c4or1k CPU core -- shared declarations.
//
// M1 implemented the full non-privileged OR1000 integer ISA. M2 adds
// SPRs (GetSPR/SetSPR), the SR flag register (SetFlags/GetFlags),
// exception delivery (cpu_exception, matching jor1k's Exception()),
// and DTLB/ITLB checks -- l.mfspr/l.mtspr/l.rfe/l.sys/l.trap are now
// real. What's still deferred: automatic tick-timer interrupt
// delivery (TTMR/TTCR are stored via GetSPR/SetSPR but nothing fires
// EXCEPT_TICK yet -- that needs the main-loop batching M3/M6 add) and
// TLB-entry LRU-bit checking (jor1k itself aborts if a guest ever
// sets those bits; nothing in this project's test programs does
// either, so it's simply not checked rather than replicating an abort
// path that's never exercised).
//
// Field positions and opcode/func numbers, SPR indices, exception
// vector numbers, and the exact GetSPR/SetSPR/Exception/DTLBLookup
// bit tests are read directly from jor1k/js/worker/or1k/safecpu.js
// (not the OR1000 spec or manual), because the decoder is
// cross-checked against that file as a bit-for-bit oracle
// (tools/or1k-oracle.js). Where safecpu.js's own comment disagrees
// with its code (case 0x1 under 0x2E is commented "rori" but the code
// is `>>>`, a logical shift, not a rotate; GetInstruction's ITLB
// permission check is commented "user read enable" inside the
// SR_SM-true branch, which reads backwards from the usual SRE/URE
// naming), the code wins and this project matches the code, not the
// comment or the naming convention it suggests.
//
// sext()'s existence is not cosmetic: see docs/c4or1k-design.md's
// "Language notes" for the M0 bug it fixes. Every place below that
// pulls a signed field out of an instruction word calls it; nothing
// uses the shift/truncate idiom, because that idiom is silently wrong
// under c4lc's 64-bit `int`.

enum { NREGS = 32 };

// SPR indices (safecpu.js's SPR_* constants).
enum {
    SPR_VR        = 0,
    SPR_DMMUCFGR  = 3,
    SPR_IMMUCFGR  = 4,
    SPR_DCCFGR    = 5,
    SPR_ICCFGR    = 6,
    SPR_UPR       = 1,
    SPR_SR        = 17,
    SPR_EPCR_BASE = 32,
    SPR_EEAR_BASE = 48,
    SPR_ESR_BASE  = 64
};

// Exception vectors (safecpu.js's EXCEPT_* constants).
enum {
    EXCEPT_RESET     = 0x100,
    EXCEPT_BUSERR    = 0x200,
    EXCEPT_DPF       = 0x300,
    EXCEPT_IPF       = 0x400,
    EXCEPT_TICK      = 0x500,
    EXCEPT_INT       = 0x800,
    EXCEPT_DTLBMISS  = 0x900,
    EXCEPT_ITLBMISS  = 0xA00,
    EXCEPT_SYSCALL   = 0xC00,
    EXCEPT_TRAP      = 0xE00
};

// Registers, flags, and SPR-backing state -- declared here so main.c's
// test harness can read them for register-dump output.
extern int r[NREGS];
extern int pc, nextpc;
extern int delayedins; // was the instruction about to run reached via a taken branch/jump?
extern int EA;          // reservation address for l.lwa/l.swa

// SR_* flags, decomposed (SetFlags/GetFlags pack/unpack these to/from
// a single SPR_SR value exactly as safecpu.js does).
extern int SR_SM, SR_TEE, SR_IEE, SR_DCE, SR_ICE, SR_DME, SR_IME;
extern int SR_LEE, SR_CE, SR_F, SR_CY, SR_OV, SR_OVE, SR_DSX, SR_EPH;
extern int SR_FO, SR_SUMRA, SR_CID;

extern int group0[2048]; // general SPRs (group 0): SR, MMU/cache config, UPR/VR, EEAR/EPCR/ESR
extern int group1[2048]; // DTLB match (0x200|set) / translate (0x280|set) registers
extern int group2[2048]; // ITLB match (0x200|set) / translate (0x280|set) registers
extern int TTMR, TTCR;     // tick timer mode/count (SPR group 10)
extern int PICMR, PICSR;   // interrupt controller mask/status (SPR group 9)

int sext(int v, int bits);

void cpu_set_flags(int x);
int cpu_get_flags();
int cpu_get_spr(int idx);
void cpu_set_spr(int idx, int val);
void cpu_exception(int excepttype, int addr);
void cpu_check_for_interrupt();
void cpu_raise_interrupt(int line);
void cpu_clear_interrupt(int line);

// Advances TTCR and fires EXCEPT_TICK when due, matching safecpu.js's
// Step loop exactly (the `if (!(steps&63)) {...tick...}` block run
// once per 64-instruction batch, plus the SR_TEE-gated delivery check
// that runs every instruction) -- except restructured for a
// one-instruction-at-a-time driver instead of jor1k's N-at-a-time
// Step(steps, clockspeed): call cpu_tick_check(clockspeed) from the
// main loop every 64 instructions (main.c), not every instruction.
// Deferred through M1-M3 (see this file's top comment); needed
// starting M4 because kernel code that busy-waits on a jiffies-driven
// timeout hangs forever without it -- found by a real boot attempt
// getting stuck retry-polling an unimplemented ATA controller.
void cpu_tick_check(int clockspeed);

// Runs one instruction. Returns 0 to keep running, 1 once pc reaches
// halt_pc (without executing it), 2 if the fetched opcode (or SPR
// group, or TLB LRU state) isn't implemented yet (state has already
// been dumped to stdout when this happens). A real DTLB/ITLB miss or
// permission fault is NOT this -- cpu_exception() already redirected
// pc/nextpc to the vector, and cpu_step returns 0 normally, matching
// jor1k's own "fault -> deliver exception -> fall through to the
// normal pc advance" control flow.
int cpu_step(int halt_pc);

void cpu_reset();
void cpu_dump();
