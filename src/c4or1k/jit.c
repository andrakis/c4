#include "cpu.h"
#include "mem.h"
#include "jit.h"

// See jit.h for the design contract and M13's history. v2 (this
// version) extends v1's pure-ALU blocks two ways, both aimed at the
// coverage problem v1 measured (5.8% of instructions, 1.87-long
// blocks):
//
//   1. Loads and stores execute INSIDE blocks, through helper
//      functions (jit_h_lwz and friends, below) that replicate the
//      interpreter's DTLB_FAST + ram_* semantics exactly, including
//      faults: before any exception can be raised the helper
//      materializes the faulting instruction's guest pc into the pc
//      global (so EPCR is captured correctly), and on a fault it
//      emulates the interpreter's per-instruction tail, reports the
//      dynamically-executed instruction count through jit_ran, and
//      sets jit_fault so the emitted code bails to a shared LEV.
//   2. Branches become block TERMINATORS instead of block breakers:
//      a l.bf/l.bnf back to the block's own head becomes an EMITTED
//      loop (whole guest iterations per invocation, with an emitted
//      per-iteration budget check so a call never exceeds the
//      caller's remaining batch budget -- tick/interrupt cadence
//      stays bit-identical to interpretation); l.j/l.jal exit to a
//      translation-time-constant target (l.jal's r9 return address
//      folded to a constant); l.jr/l.jalr exit to r[rb]>>2 computed
//      at exit; any other l.bf/l.bnf exits two ways on the SR_F
//      value captured AT the branch, before its delay slot runs
//      (jit_tmp2 -- the slot may legally write SR_F). Delay slots
//      execute inside the block, including MEMORY slots: the
//      helpers' `di` argument replicates the delayed-instruction
//      fault protocol (EPCR points at the BRANCH via cpu_exception's
//      delayedins adjustment; resume re-executes the branch), with
//      taken-ness derived at runtime from the captured flag for
//      conditional branches. Slots that would change the l.jr/
//      l.jalr jump register, and loop fusion outside boot mode
//      (halt_pc detection inside an unbounded loop would be wrong),
//      fall back to interpreter-terminated blocks.
//
// Block layout in the arena:
//   word 0: K  (static instruction count; -1 = budget-driven loop)
//   word 1: vpc (guest virtual pc this block was translated for)
//   word 2: ENT   word 3: 0
//   word 4: JMP   word 5: <address of mainline start>
//   word 6: LEV                  <- shared fault exit (helpers have
//                                   already done ALL bookkeeping)
//   word 7...: loop blocks only: the budget-exhausted exit stub
//   then: the mainline, ending in an epilogue (straight-line) or the
//   backward branch + fallthrough exit (loops).
//
// Emitted code still uses only original plain-c4 subset opcodes plus
// BZ/BNZ/JMP, so blocks run identically under c4m and c4mp.

// cpu.c's full DTLB walk (raises DTLBMISS/DPF itself); not declared
// in cpu.h, so declare it here -- c4lc -c turns an undefined
// prototype into an extern symbol for c4rlink.
int dtlb_lookup(int addr, int write);

enum { JIT_CACHE_MASK = 0x1FFFF };    // 131072 direct-mapped entries
enum { JIT_CACHE_ENTRIES = 0x20000 };
enum { JIT_ARENA_WORDS = 0x200000 };  // 16MB of emitted code, bump-allocated
enum { JIT_MAX_GUEST = 24 };          // guest instructions per straight-line block
enum { JIT_BLOCK_WORDS = 2048 };      // arena words reserved per translation
enum { JIT_WORD_HEADROOM = 96 };      // stop emitting when this close to the cap
enum { JIT_NPAGES = 4096 };           // RAM_SIZE / 8192

int *jit_tag;       // phys byte address of the cached block's first instruction
int *jit_egen;      // jit_pagegen[page] captured when the entry was installed
int *jit_blk;       // arena pointer (as int); 0 = known not eligible
int *jit_pagegen;
int *jit_arena;
int jit_arena_used; // words
int jit_on;
int jit_tmp;        // scratch global for emitted flag sequences (blocks are
                    // atomic -- nothing else runs mid-block -- so one is enough)
int jit_tmp2;       // second scratch: SR_F captured at a conditional
                    // TERMINATOR branch, before its delay slot runs (the
                    // slot may itself write SR_F; the branch decision must
                    // use the value AT the branch). Never touched by the
                    // ALU emitters, which use jit_tmp -- a delay slot that
                    // is an l.addi would otherwise clobber the capture.
int jit_nblocks;    // blocks executed (stats, reported by main.c at exit)
int jit_ninstr;     // guest instructions executed via blocks

// v2 runtime state shared between jit_try, the helpers, and emitted
// code. jit_ran is the actual guest-instruction count a block
// invocation executed (helpers set it on fault; epilogues set it on
// normal exit). jit_count accumulates completed work inside loop
// blocks (and is the base the helpers add their offset to). jit_budget
// is the caller's remaining batch budget, enforced by the emitted
// per-iteration check in loop blocks. jit_fault tells the emitted
// code to bail to the shared LEV.
int jit_ran;
int jit_count;
int jit_budget;
int jit_fault;

// c4m opcode numbers, looked up once by name at init (never hardcoded:
// the enum is append-only but there is no reason to bake numbers when
// the VM will simply tell us). Except LE -- see jit_init.
int jc_IMM, jc_PSH, jc_LI, jc_SI, jc_ENT, jc_LEV;
int jc_ADD, jc_SUB, jc_AND, jc_OR, jc_XOR, jc_SHL, jc_SHR;
int jc_EQ, jc_NE, jc_LT, jc_GT, jc_LE, jc_GE;
int jc_JMP, jc_BZ, jc_BNZ, jc_JSR, jc_ADJ;

int *jit_ep;        // emission cursor during translation
int jit_dry;        // 1 = decode/eligibility pass only: emit nothing

void jit_e1(int op) { if (jit_dry) return; *jit_ep++ = op; }
void jit_e2(int op, int operand) { if (jit_dry) return; *jit_ep++ = op; *jit_ep++ = operand; }

// address to be stored through at the end of a sequence: IMM addr; PSH
// -- the pushed address a trailing SI pops. Every ALU body below is
// "push destination address, compute value into the accumulator, SI".
void jit_dst(int addr) { jit_e2(jc_IMM, addr); jit_e1(jc_PSH); }
// load a global int into the accumulator
void jit_ld(int addr) { jit_e2(jc_IMM, addr); jit_e1(jc_LI); }
// a = a & 0xFFFFFFFF (uval(), inline)
void jit_mask32() { jit_e1(jc_PSH); jit_e2(jc_IMM, 0xFFFFFFFF); jit_e1(jc_AND); }
// a = sext(a, 32), inline -- the IDENTICAL mask/xor/subtract formula
// cpu.c's sext() uses, unrolled into bytecode so no JSR is needed.
void jit_sx32() {
    jit_mask32();
    jit_e1(jc_PSH); jit_e2(jc_IMM, 0x80000000); jit_e1(jc_XOR);
    jit_e1(jc_PSH); jit_e2(jc_IMM, 0x80000000); jit_e1(jc_SUB);
}

// ---- v2 memory helpers ----------------------------------------------
//
// One per load/store form the interpreter has (minus l.lwa/l.swa,
// whose EA reservation stays interpreter-only). Each mirrors the
// corresponding cpu_run_batch case body EXACTLY: the same DTLB_FAST
// fast path, the same fall-through to dtlb_lookup (which raises the
// real DTLBMISS/DPF), the same ram_* routing (which is also the MMIO
// path), the same extension of the loaded value. On a fault the
// helper leaves the machine in PRECISELY the state the interpreter's
// faulting iteration would have: pc/nextpc advanced through the
// exception redirect, EPCR captured with the faulting instruction's
// pc (materialized here BEFORE calling dtlb_lookup), the destination
// register NOT written, and jit_ran = the count INCLUDING the
// faulting instruction. jit_fault tells the emitted code to exit.
//
// The fast path is duplicated into jit_addr_r/jit_addr_w rather than
// shared with a flag argument because these run on every in-block
// memory access and c4lc has no inlining.

// `di` is 1 when this access is a branch DELAY SLOT on the taken
// path: the exception protocol for a faulting delay slot differs
// (EPCR points at the BRANCH -- cpu_exception subtracts 4 when
// delayedins is set -- and resume re-executes the branch), so the
// helper materializes delayedins along with pc before the walk. The
// fast path never touches either, so ordinary in-block accesses pay
// nothing for the generality; on a successful slow-path walk
// delayedins is restored to 0 (the interpreter's tail would have
// cleared it after the slot completed).
int jit_addr_r(int addr, int fpc, int ioff, int di) {
    int vpage, phys;
    jit_fault = 0;
    if (!SR_DME) return addr;
    vpage = addr >> 13;
    if (vpage == dtlb_cache_vpage && SR_SM == dtlb_cache_sm) {
        if (dtlb_cache_rok) return dtlb_cache_phys | (addr & 0x1FFF);
    }
    // miss or permission problem: the full walk may raise, so give it
    // the faulting instruction's pc first (EPCR reads the pc global)
    pc = fpc; nextpc = fpc + 1; delayedins = di;
    phys = dtlb_lookup(addr, 0);
    if (phys == -1) {
        pc = nextpc; nextpc = pc + 1;      // the interpreter tail's advance
        jit_ran = jit_count + ioff + 1;    // the faulting instruction counts
        jit_fault = 1;
    } else delayedins = 0;
    return phys;
}

int jit_addr_w(int addr, int fpc, int ioff, int di) {
    int vpage, phys;
    jit_fault = 0;
    if (!SR_DME) return addr;
    vpage = addr >> 13;
    if (vpage == dtlb_cache_vpage && SR_SM == dtlb_cache_sm) {
        if (dtlb_cache_wok) return dtlb_cache_phys | (addr & 0x1FFF);
    }
    pc = fpc; nextpc = fpc + 1; delayedins = di;
    phys = dtlb_lookup(addr, 1);
    if (phys == -1) {
        pc = nextpc; nextpc = pc + 1;
        jit_ran = jit_count + ioff + 1;
        jit_fault = 1;
    } else delayedins = 0;
    return phys;
}

int jit_h_lwz(int addr, int fpc, int ioff, int di) {
    int phys, v;
    phys = jit_addr_r(addr, fpc, ioff, di);
    if (phys == -1) return 0;
    v = ram_lw(phys) & 0xFFFFFFFF;         // sext(.., 32), inline
    return (v ^ 0x80000000) - 0x80000000;
}
int jit_h_lbz(int addr, int fpc, int ioff, int di) {
    int phys;
    phys = jit_addr_r(addr, fpc, ioff, di);
    if (phys == -1) return 0;
    return ram_lb(phys);
}
int jit_h_lbs(int addr, int fpc, int ioff, int di) {
    int phys, v;
    phys = jit_addr_r(addr, fpc, ioff, di);
    if (phys == -1) return 0;
    v = ram_lb(phys) & 0xFF;               // sext(.., 8), inline
    return (v ^ 0x80) - 0x80;
}
int jit_h_lhz(int addr, int fpc, int ioff, int di) {
    int phys;
    phys = jit_addr_r(addr, fpc, ioff, di);
    if (phys == -1) return 0;
    return ram_lh(phys);
}
int jit_h_lhs(int addr, int fpc, int ioff, int di) {
    int phys, v;
    phys = jit_addr_r(addr, fpc, ioff, di);
    if (phys == -1) return 0;
    v = ram_lh(phys) & 0xFFFF;             // sext(.., 16), inline
    return (v ^ 0x8000) - 0x8000;
}
void jit_h_sw(int addr, int val, int fpc, int ioff, int di) {
    int phys;
    phys = jit_addr_w(addr, fpc, ioff, di);
    if (phys != -1) ram_sw(phys, val);
}
void jit_h_sb(int addr, int val, int fpc, int ioff, int di) {
    int phys;
    phys = jit_addr_w(addr, fpc, ioff, di);
    if (phys != -1) ram_sb(phys, val);
}
void jit_h_sh(int addr, int val, int fpc, int ioff, int di) {
    int phys;
    phys = jit_addr_w(addr, fpc, ioff, di);
    if (phys != -1) ram_sh(phys, val);
}

// l.mfspr: a pure read (cpu_get_spr raises nothing), so no fault
// machinery -- but a very common block BREAKER before this existed,
// stranding every eligible run behind it until the next branch
// target. TTCR/PIC reads mid-block see the same values the
// interpreter would: both only change between batches.
int jit_h_mfspr(int idx) { return cpu_get_spr(idx); }

// ---- per-instruction emission ---------------------------------------

// l.sfXX/l.sfXXi func -> c4m compare opcode, or -1. funcs 2..5 are
// the unsigned forms (caller masks operands with jit_mask32).
int jit_cmpop(int func) {
    if (func == 0x0) return jc_EQ;
    if (func == 0x1) return jc_NE;
    if (func == 0x2) return jc_GT;
    if (func == 0x3) return jc_GE;
    if (func == 0x4) return jc_LT;
    if (func == 0x5) return jc_LE;
    if (func == 0xa) return jc_GT;
    if (func == 0xb) return jc_GE;
    if (func == 0xc) return jc_LT;
    if (func == 0xd) return jc_LE;
    return -1;
}
int jit_cmpu(int func) { return (func >= 0x2 && func <= 0x5); }

// Shared tail of l.addi / l.add / l.sub: with jit_tmp = result stored
// and (rA ^ operand [^ -1]) in the accumulator, emit the SR_OV
// mask/test and the r[rd] writeback. SR_CY differs per form and is
// emitted by the caller first.
void jit_ov_tail(int ra, int rd) {
    jit_e1(jc_PSH);
    jit_ld((int)(r + ra)); jit_e1(jc_PSH); jit_ld((int)&jit_tmp); jit_e1(jc_XOR);
    jit_e1(jc_AND);
    jit_e1(jc_PSH); jit_e2(jc_IMM, 0x80000000); jit_e1(jc_AND);
    jit_e1(jc_PSH); jit_e2(jc_IMM, 0); jit_e1(jc_NE);
    jit_e1(jc_SI);
    jit_dst((int)(r + rd)); jit_ld((int)&jit_tmp); jit_e1(jc_SI);
}

// emit `a = r[ra] + immval` (the effective address of a load/store)
void jit_ea(int ra, int immval) {
    jit_ld((int)(r + ra));
    jit_e1(jc_PSH); jit_e2(jc_IMM, immval); jit_e1(jc_ADD);
}

// emit the helper's `di` argument. dikind: 0 = not a delay slot
// (constant 0); 1 = slot of an unconditional jump (constant 1);
// 2 = slot of l.bf (di = the captured SR_F in jit_tmp2); 3 = slot of
// l.bnf (di = !jit_tmp2 -- taken when the flag was CLEAR).
void jit_di_arg(int dikind) {
    if (dikind == 0) { jit_e2(jc_IMM, 0); jit_e1(jc_PSH); return; }
    if (dikind == 1) { jit_e2(jc_IMM, 1); jit_e1(jc_PSH); return; }
    jit_ld((int)&jit_tmp2);
    if (dikind == 3) { jit_e1(jc_PSH); jit_e2(jc_IMM, 0); jit_e1(jc_EQ); }
    jit_e1(jc_PSH);
}

// The load mainline shared by all five load forms: r[rd] = helper(...)
// with the fault test between the call and the register write (the
// destination register must NOT be written on a fault -- the value is
// parked on the stack across the jit_fault load and recovered with
// the IMM 0/OR pop; a bailing LEV doesn't care about stack junk, it
// restores sp from bp).
void jit_load(int helper, int rd, int ra, int immval, int fpc, int ioff, int fault_addr, int dikind) {
    jit_dst((int)(r + rd));                // [.., &r[rd]]
    jit_ea(ra, immval);                    // a = addr
    jit_e1(jc_PSH);                        // arg: addr
    jit_e2(jc_IMM, fpc); jit_e1(jc_PSH);   // arg: fpc
    jit_e2(jc_IMM, ioff); jit_e1(jc_PSH);  // arg: ioff
    jit_di_arg(dikind);                    // arg: di
    jit_e2(jc_JSR, helper); jit_e2(jc_ADJ, 4);
    jit_e1(jc_PSH);                        // park value: [.., &r[rd], v]
    jit_ld((int)&jit_fault);
    jit_e2(jc_BNZ, fault_addr);            // fault: bail (stack junk is fine)
    jit_e2(jc_IMM, 0); jit_e1(jc_OR);      // a = v (pops it)
    jit_e1(jc_SI);                         // r[rd] = v
}

// The store mainline shared by all three store forms.
void jit_store(int helper, int rb, int ra, int simmval, int fpc, int ioff, int fault_addr, int dikind) {
    jit_ea(ra, simmval);                   // a = addr
    jit_e1(jc_PSH);                        // arg: addr
    jit_ld((int)(r + rb)); jit_e1(jc_PSH); // arg: val
    jit_e2(jc_IMM, fpc); jit_e1(jc_PSH);   // arg: fpc
    jit_e2(jc_IMM, ioff); jit_e1(jc_PSH);  // arg: ioff
    jit_di_arg(dikind);                    // arg: di
    jit_e2(jc_JSR, helper); jit_e2(jc_ADJ, 5);
    jit_ld((int)&jit_fault);
    jit_e2(jc_BNZ, fault_addr);            // fault: bail
}

// Translate/emit ONE guest instruction (jit_dry = decode-only pass).
// Returns 1 if this instruction can live inside a block, 0 if it must
// end the block. Transcribed from cpu_run_batch's case bodies and
// maintained in lockstep with them. vpc+ioff is the instruction's own
// guest pc (used for fault EPCR); fault_addr is the shared bail LEV.
int jit_emit_one(int ins, int vpc, int ioff, int fault_addr, int dikind) {
    int op, rd, ra, rb, func, imm, cop, fpc;

    op = (ins >> 26) & 0x3F;
    rd = (ins >> 21) & 0x1F;
    ra = (ins >> 16) & 0x1F;
    rb = (ins >> 11) & 0x1F;
    fpc = vpc + ioff;

    if (op == 0x05) return 1;             // l.nop: only the pc advance

    if (op == 0x06) {                     // l.movhi: r[rd] = sext((ins&0xFFFF)<<16, 32)
        if (rd == 0) return 0;
        jit_dst((int)(r + rd));
        jit_e2(jc_IMM, sext((ins & 0xFFFF) << 16, 32));
        jit_e1(jc_SI);
        return 1;
    }
    if (op == 0x2D) {                     // l.mfspr: r[rd] = cpu_get_spr(rA | zimm)
        if (rd == 0) return 0;
        jit_dst((int)(r + rd));
        jit_ld((int)(r + ra)); jit_e1(jc_PSH); jit_e2(jc_IMM, ins & 0xFFFF); jit_e1(jc_OR);
        jit_e1(jc_PSH);
        jit_e2(jc_JSR, (int)&jit_h_mfspr); jit_e2(jc_ADJ, 1);
        jit_e1(jc_SI);
        return 1;
    }
    if (op == 0x21) {                     // l.lwz
        if (rd == 0) return 0;
        jit_load((int)&jit_h_lwz, rd, ra, sext(ins, 16), fpc, ioff, fault_addr, dikind);
        return 1;
    }
    if (op == 0x23) {                     // l.lbz
        if (rd == 0) return 0;
        jit_load((int)&jit_h_lbz, rd, ra, sext(ins, 16), fpc, ioff, fault_addr, dikind);
        return 1;
    }
    if (op == 0x24) {                     // l.lbs
        if (rd == 0) return 0;
        jit_load((int)&jit_h_lbs, rd, ra, sext(ins, 16), fpc, ioff, fault_addr, dikind);
        return 1;
    }
    if (op == 0x25) {                     // l.lhz
        if (rd == 0) return 0;
        jit_load((int)&jit_h_lhz, rd, ra, sext(ins, 16), fpc, ioff, fault_addr, dikind);
        return 1;
    }
    if (op == 0x26) {                     // l.lhs
        if (rd == 0) return 0;
        jit_load((int)&jit_h_lhs, rd, ra, sext(ins, 16), fpc, ioff, fault_addr, dikind);
        return 1;
    }
    if (op == 0x35) {                     // l.sw
        jit_store((int)&jit_h_sw, rb, ra, sext(((ins >> 10) & 0xF800) | (ins & 0x7FF), 16), fpc, ioff, fault_addr, dikind);
        return 1;
    }
    if (op == 0x36) {                     // l.sb
        jit_store((int)&jit_h_sb, rb, ra, sext(((ins >> 10) & 0xF800) | (ins & 0x7FF), 16), fpc, ioff, fault_addr, dikind);
        return 1;
    }
    if (op == 0x37) {                     // l.sh
        jit_store((int)&jit_h_sh, rb, ra, sext(((ins >> 10) & 0xF800) | (ins & 0x7FF), 16), fpc, ioff, fault_addr, dikind);
        return 1;
    }
    if (op == 0x27) {                     // l.addi, full SR_CY/SR_OV semantics
        if (rd == 0) return 0;
        imm = sext(ins, 16);
        jit_dst((int)&jit_tmp);           // jit_tmp = sext(rA + imm, 32)
        jit_ld((int)(r + ra)); jit_e1(jc_PSH); jit_e2(jc_IMM, imm); jit_e1(jc_ADD);
        jit_sx32();
        jit_e1(jc_SI);
        jit_dst((int)&SR_CY);             // SR_CY = result < rA
        jit_ld((int)&jit_tmp); jit_e1(jc_PSH); jit_ld((int)(r + ra)); jit_e1(jc_LT);
        jit_e1(jc_SI);
        jit_dst((int)&SR_OV);             // (((rA ^ imm ^ -1) & (rA ^ result)) & bit31) != 0
        jit_ld((int)(r + ra)); jit_e1(jc_PSH); jit_e2(jc_IMM, imm ^ -1); jit_e1(jc_XOR);
        jit_ov_tail(ra, rd);
        return 1;
    }
    if (op == 0x29) {                     // l.andi
        if (rd == 0) return 0;
        jit_dst((int)(r + rd));
        jit_ld((int)(r + ra)); jit_e1(jc_PSH); jit_e2(jc_IMM, ins & 0xFFFF); jit_e1(jc_AND);
        jit_e1(jc_SI);
        return 1;
    }
    if (op == 0x2A) {                     // l.ori
        if (rd == 0) return 0;
        jit_dst((int)(r + rd));
        jit_ld((int)(r + ra)); jit_e1(jc_PSH); jit_e2(jc_IMM, ins & 0xFFFF); jit_e1(jc_OR);
        jit_e1(jc_SI);
        return 1;
    }
    if (op == 0x2B) {                     // l.xori (plain XOR: the interpreter's
        if (rd == 0) return 0;            // sext is a no-op on canonical operands)
        jit_dst((int)(r + rd));
        jit_ld((int)(r + ra)); jit_e1(jc_PSH); jit_e2(jc_IMM, sext(ins, 16)); jit_e1(jc_XOR);
        jit_e1(jc_SI);
        return 1;
    }
    if (op == 0x2E) {                     // l.slli / l.srli / l.srai
        func = (ins >> 6) & 0x3;
        if (func > 2) return 0;           // func 3: interpreter faults; leave it there
        if (rd == 0) return 0;
        imm = ins & 0x1F;
        jit_dst((int)(r + rd));
        jit_ld((int)(r + ra));
        if (func == 0) {                  // slli: sext(rA << k, 32)
            jit_e1(jc_PSH); jit_e2(jc_IMM, imm); jit_e1(jc_SHL);
            jit_sx32();
        }
        else if (func == 1) {             // srli: uval(rA) >> k (matches the
            jit_mask32();                 // interpreter EXACTLY, incl. k=0)
            jit_e1(jc_PSH); jit_e2(jc_IMM, imm); jit_e1(jc_SHR);
        }
        else {                            // srai: rA >> k (c4m SHR is arithmetic)
            jit_e1(jc_PSH); jit_e2(jc_IMM, imm); jit_e1(jc_SHR);
        }
        jit_e1(jc_SI);
        return 1;
    }
    if (op == 0x2F) {                     // l.sfXXi: SR_F = cmp(rA, sext imm)
        func = (ins >> 21) & 0x1F;
        cop = jit_cmpop(func);
        if (cop < 0) return 0;
        imm = sext(ins, 16);
        jit_dst((int)&SR_F);
        jit_ld((int)(r + ra));
        if (jit_cmpu(func)) { jit_mask32(); imm = imm & 0xFFFFFFFF; }
        jit_e1(jc_PSH); jit_e2(jc_IMM, imm); jit_e1(cop);
        jit_e1(jc_SI);
        return 1;
    }
    if (op == 0x38) {                     // three-operand ALU (safe subset)
        func = ins & 0x3CF;
        if (func == 0x0) {                // add: addi's flag semantics with rB
            if (rd == 0) return 0;
            jit_dst((int)&jit_tmp);
            jit_ld((int)(r + ra)); jit_e1(jc_PSH); jit_ld((int)(r + rb)); jit_e1(jc_ADD);
            jit_sx32();
            jit_e1(jc_SI);
            jit_dst((int)&SR_CY);
            jit_ld((int)&jit_tmp); jit_e1(jc_PSH); jit_ld((int)(r + ra)); jit_e1(jc_LT);
            jit_e1(jc_SI);
            jit_dst((int)&SR_OV);         // (((rA ^ rB ^ -1) & (rA ^ result)) & bit31) != 0
            jit_ld((int)(r + ra)); jit_e1(jc_PSH); jit_ld((int)(r + rb)); jit_e1(jc_XOR);
            jit_e1(jc_PSH); jit_e2(jc_IMM, -1); jit_e1(jc_XOR);
            jit_ov_tail(ra, rd);
            return 1;
        }
        if (func == 0x2) {                // sub
            if (rd == 0) return 0;
            jit_dst((int)&jit_tmp);       // jit_tmp = sext(rA - rB, 32)
            jit_ld((int)(r + ra)); jit_e1(jc_PSH); jit_ld((int)(r + rb)); jit_e1(jc_SUB);
            jit_sx32();
            jit_e1(jc_SI);
            jit_dst((int)&SR_CY);         // SR_CY = rB > rA
            jit_ld((int)(r + rb)); jit_e1(jc_PSH); jit_ld((int)(r + ra)); jit_e1(jc_GT);
            jit_e1(jc_SI);
            jit_dst((int)&SR_OV);         // (((rA ^ rB) & (rA ^ result)) & bit31) != 0
            jit_ld((int)(r + ra)); jit_e1(jc_PSH); jit_ld((int)(r + rb)); jit_e1(jc_XOR);
            jit_ov_tail(ra, rd);
            return 1;
        }
        if (func == 0x3 || func == 0x4 || func == 0x5) { // and / or / xor
            if (rd == 0) return 0;
            jit_dst((int)(r + rd));
            jit_ld((int)(r + ra)); jit_e1(jc_PSH); jit_ld((int)(r + rb));
            if (func == 0x3) jit_e1(jc_AND);
            else if (func == 0x4) jit_e1(jc_OR);
            else jit_e1(jc_XOR);          // interpreter sexts; no-op, see xori
            jit_e1(jc_SI);
            return 1;
        }
        if (func == 0x8) {                // sll: sext(rA << (rB & 0x1F), 32)
            if (rd == 0) return 0;
            jit_dst((int)(r + rd));
            jit_ld((int)(r + ra)); jit_e1(jc_PSH);
            jit_ld((int)(r + rb)); jit_e1(jc_PSH); jit_e2(jc_IMM, 0x1F); jit_e1(jc_AND);
            jit_e1(jc_SHL);
            jit_sx32();
            jit_e1(jc_SI);
            return 1;
        }
        if (func == 0x48) {               // srl: uval(rA) >> (rB & 0x1F)
            if (rd == 0) return 0;
            jit_dst((int)(r + rd));
            jit_ld((int)(r + ra)); jit_mask32(); jit_e1(jc_PSH);
            jit_ld((int)(r + rb)); jit_e1(jc_PSH); jit_e2(jc_IMM, 0x1F); jit_e1(jc_AND);
            jit_e1(jc_SHR);
            jit_e1(jc_SI);
            return 1;
        }
        if (func == 0x88) {               // sra: rA >> (rB & 0x1F)
            if (rd == 0) return 0;
            jit_dst((int)(r + rd));
            jit_ld((int)(r + ra)); jit_e1(jc_PSH);
            jit_ld((int)(r + rb)); jit_e1(jc_PSH); jit_e2(jc_IMM, 0x1F); jit_e1(jc_AND);
            jit_e1(jc_SHR);
            jit_e1(jc_SI);
            return 1;
        }
        return 0;                         // mul/div/ff1/fl1/unknown
    }
    if (op == 0x39) {                     // l.sfXX: SR_F = cmp(rA, rB)
        func = (ins >> 21) & 0x1F;
        cop = jit_cmpop(func);
        if (cop < 0) return 0;
        jit_dst((int)&SR_F);
        jit_ld((int)(r + ra));
        if (jit_cmpu(func)) jit_mask32();
        jit_e1(jc_PSH);
        jit_ld((int)(r + rb));
        if (jit_cmpu(func)) jit_mask32();
        jit_e1(cop);
        jit_e1(jc_SI);
        return 1;
    }
    return 0;                             // branches, spr, sys, rfe, lwa/swa, ...
}

// is this a memory-access instruction? (loop delay slots must not be:
// a faulting delay slot carries delayedins=1 exception semantics a
// fused loop cannot replicate)
int jit_is_mem(int ins) {
    int op;
    op = (ins >> 26) & 0x3F;
    if (op == 0x21 || op == 0x23 || op == 0x24 || op == 0x25 || op == 0x26) return 1;
    if (op == 0x35 || op == 0x36 || op == 0x37) return 1;
    return 0;
}
// does this instruction write SR_F? (loop delay slots must not: the
// emitted loop tests SR_F after the slot runs)
int jit_writes_srf(int ins) {
    int op;
    op = (ins >> 26) & 0x3F;
    return (op == 0x2F || op == 0x39);
}

// decode-only eligibility via the emitter itself, so the two can
// never drift apart
int jit_ok(int ins, int vpc, int ioff) {
    int e;
    jit_dry = 1;
    e = jit_emit_one(ins, vpc, ioff, 0, 0);
    jit_dry = 0;
    return e;
}

// ---- block translation ----------------------------------------------

// the interpreter-tail emission used by exit stubs: pc/nextpc as baked
// constants, jit_ran from a constant or from jit_count
void jit_exit_const(int newpc, int ranval) {
    jit_dst((int)&pc); jit_e2(jc_IMM, newpc); jit_e1(jc_SI);
    jit_dst((int)&nextpc); jit_e2(jc_IMM, newpc + 1); jit_e1(jc_SI);
    jit_dst((int)&jit_ran); jit_e2(jc_IMM, ranval); jit_e1(jc_SI);
    jit_e1(jc_LEV);
}
void jit_exit_counted(int newpc) {
    jit_dst((int)&pc); jit_e2(jc_IMM, newpc); jit_e1(jc_SI);
    jit_dst((int)&nextpc); jit_e2(jc_IMM, newpc + 1); jit_e1(jc_SI);
    jit_dst((int)&jit_ran); jit_ld((int)&jit_count); jit_e1(jc_SI);
    jit_e1(jc_LEV);
}

// Translate a block starting at guest physical byte address `phys` /
// virtual word index `vpc`. allow_loop is 1 only in boot mode
// (halt_pc == -1): a fused loop runs an unbounded number of guest
// instructions per call (budget-bounded, not halt-checked), which is
// wrong if a halt pc could fall inside it. Returns the arena block
// pointer as an int, or 0.
int jit_translate(int phys, int vpc, int allow_loop) {
    int *hdr;
    int k, ins, words, fault_addr, exhaust_addr, i, brop, brins, slot, slotok, L, tgt, rb9;
    int *looptop;
    int *jmp_operand;
    int *patch;

    if (!jit_on) return 0;
    if (jit_arena_used + JIT_BLOCK_WORDS > JIT_ARENA_WORDS) return 0;

    // ---- scan pass: how far does eligibility run, and what ends it?
    // brop != 0 means a branch/jump we may be able to FUSE as the
    // block's terminator instead of stopping short of it.
    k = 0;
    brop = 0;
    brins = 0;
    while (k < JIT_MAX_GUEST) {
        if (((phys & 0x1FFF) + k * 4) >= 0x2000) break;
        ins = ram_lw(phys + k * 4);
        i = (ins >> 26) & 0x3F;
        if (i == 0x00 || i == 0x01 || i == 0x03 || i == 0x04 || i == 0x11 || i == 0x12) {
            brop = i;                     // l.j / l.jal / l.bnf / l.bf / l.jr / l.jalr
            brins = ins;
            break;
        }
        if (!jit_ok(ins, vpc, k)) break;
        ++k;
    }

    // ---- terminator viability. Every fused form needs the delay slot
    // in-page, block-eligible, and NON-MEMORY: a faulting delay slot
    // carries delayedins=1 exception semantics (EPCR points at the
    // BRANCH, resume re-executes it) that emitted code cannot
    // replicate, so those branches stay interpreter-terminated.
    slot = 0;
    slotok = 0;
    if (brop) {
        if (((phys & 0x1FFF) + (k + 1) * 4) < 0x2000) {
            slot = ram_lw(phys + (k + 1) * 4);
            if (jit_ok(slot, vpc, k + 1)) slotok = 1;   // memory slots OK: the
                                          // helpers' di argument replicates the
                                          // delayed-instruction fault protocol
        }
    }
    // l.jr/l.jalr: the jump target register is read AT the branch, but
    // the emitted exit reads it AFTER the slot -- so the slot must not
    // write it (conservative: any slot whose rd field equals rb is
    // refused, writer or not). l.jalr additionally reads rB BEFORE
    // writing r9, so rb == 9 would see the wrong value: refuse.
    rb9 = (brins >> 11) & 0x1F;
    if (slotok && (brop == 0x11 || brop == 0x12)) {
        if (((slot >> 21) & 0x1F) == rb9) slotok = 0;
        if (brop == 0x12 && rb9 == 9) slotok = 0;
    }
    // self-loop? (l.bf/l.bnf back to the block's own first
    // instruction, boot mode only): the budget-driven fused-loop form
    // instead of the generic conditional terminator
    L = 0;
    if (slotok && (brop == 0x03 || brop == 0x04) && allow_loop && k >= 1) {
        if (sext(brins, 26) == -k) L = k + 2;
    }

    if (k == 0 && !(brop && slotok)) return 0;   // nothing translatable at all

    // ---- emit pass
    hdr = jit_arena + jit_arena_used;
    jit_ep = hdr + 2;
    jit_e2(jc_ENT, 0);
    jmp_operand = jit_ep + 1;             // JMP's operand word, backfilled
    jit_e2(jc_JMP, 0);
    fault_addr = (int)jit_ep;
    jit_e1(jc_LEV);                       // shared fault exit
    exhaust_addr = (int)jit_ep;
    if (L) jit_exit_counted(vpc);         // budget-exhausted: still AT the loop head
    *jmp_operand = (int)jit_ep;           // mainline starts here

    if (L) {
        // fused self-loop: per-iteration budget check, body, slot,
        // count, conditional back-edge, fallthrough exit. The slot may
        // write SR_F here too: the branch decision uses the value
        // captured BEFORE the slot, exactly as the interpreter reads
        // SR_F at the branch instruction.
        looptop = jit_ep;
        jit_ld((int)&jit_budget); jit_e1(jc_PSH); jit_ld((int)&jit_count); jit_e1(jc_SUB);
        jit_e1(jc_PSH); jit_e2(jc_IMM, L); jit_e1(jc_LT);
        jit_e2(jc_BNZ, exhaust_addr);
        i = 0;
        while (i < k) {
            if (!jit_emit_one(ram_lw(phys + i * 4), vpc, i, fault_addr, 0)) return 0;
            ++i;
        }
        jit_dst((int)&jit_tmp2); jit_ld((int)&SR_F); jit_e1(jc_SI);   // capture at branch
        if (!jit_emit_one(slot, vpc, k + 1, fault_addr, (brop == 0x04) ? 2 : 3)) return 0;
        jit_dst((int)&jit_count);         // jit_count = jit_count + L
        jit_ld((int)&jit_count); jit_e1(jc_PSH); jit_e2(jc_IMM, L); jit_e1(jc_ADD);
        jit_e1(jc_SI);
        jit_ld((int)&jit_tmp2);
        if (brop == 0x04) jit_e2(jc_BNZ, (int)looptop);  // l.bf: loop while SR_F
        else jit_e2(jc_BZ, (int)looptop);                // l.bnf: loop while !SR_F
        jit_exit_counted(vpc + L);        // fallthrough: past branch + slot
        hdr[0] = -1;                      // budget-driven
    } else {
        // straight-line body, optionally ending in a fused terminator
        i = 0;
        while (i < k) {
            words = ((int)jit_ep - (int)hdr) / 8;
            if (words > JIT_BLOCK_WORDS - JIT_WORD_HEADROOM) { brop = 0; break; }
            if (!jit_emit_one(ram_lw(phys + i * 4), vpc, i, fault_addr, 0)) { brop = 0; break; }
            ++i;
        }
        if (i < k) brop = 0;              // body cut short: can't reach the branch
        if (brop && slotok) {
            // the terminator instruction itself, then its delay slot,
            // then the exit. Guest pc of the branch is vpc+i, of the
            // slot vpc+i+1; both count as executed (blk[0] = i+2).
            if (brop == 0x01 || brop == 0x12) {
                // l.jal/l.jalr write the return address FIRST (the
                // slot may legally read or overwrite r9): r9 =
                // sext(((branch_nextpc)<<2)+4, 32), a translation-time
                // constant: branch_nextpc = vpc+i+1
                jit_dst((int)(r + 9));
                jit_e2(jc_IMM, sext(((vpc + i + 1) << 2) + 4, 32));
                jit_e1(jc_SI);
            }
            if (brop == 0x03 || brop == 0x04) {
                // conditional: capture SR_F at the branch, before the slot
                jit_dst((int)&jit_tmp2); jit_ld((int)&SR_F); jit_e1(jc_SI);
            }
            if (brop == 0x03) slotok = 3;
            else if (brop == 0x04) slotok = 2;
            else slotok = 1;
            if (!jit_emit_one(slot, vpc, i + 1, fault_addr, slotok)) return 0;
            if (brop == 0x00 || brop == 0x01) {
                // static target: jump = branch_pc + sext26
                jit_exit_const(vpc + i + sext(brins, 26), i + 2);
            } else if (brop == 0x11 || brop == 0x12) {
                // dynamic target: pc = r[rb] >> 2 (the slot was barred
                // from writing rb above)
                jit_dst((int)&pc);
                jit_ld((int)(r + rb9)); jit_e1(jc_PSH); jit_e2(jc_IMM, 2); jit_e1(jc_SHR);
                jit_e1(jc_SI);
                jit_dst((int)&nextpc);
                jit_ld((int)&pc); jit_e1(jc_PSH); jit_e2(jc_IMM, 1); jit_e1(jc_ADD);
                jit_e1(jc_SI);
                jit_dst((int)&jit_ran); jit_e2(jc_IMM, i + 2); jit_e1(jc_SI);
                jit_e1(jc_LEV);
            } else {
                // conditional two-way exit on the captured flag:
                // l.bf takes when SR_F, l.bnf when !SR_F
                jit_ld((int)&jit_tmp2);
                patch = jit_ep + 1;       // forward branch to the taken exit
                if (brop == 0x04) jit_e2(jc_BNZ, 0);
                else jit_e2(jc_BZ, 0);
                jit_exit_const(vpc + i + 2, i + 2);          // not taken: fall through
                *patch = (int)jit_ep;
                jit_exit_const(vpc + i + sext(brins, 26), i + 2);  // taken
            }
            hdr[0] = i + 2;
        } else {
            if (i == 0) return 0;
            jit_exit_const(vpc + i, i);
            hdr[0] = i;
        }
    }

    hdr[1] = vpc;
    jit_arena_used = ((int)jit_ep - (int)jit_arena) / 8;
    return (int)hdr;
}

// The self-test block invocation, in its own function so jit_init's
// (cold) call doesn't force the indirect-call pattern into any hotter
// frame than needed.
void jit_run_selftest(int *code) {
    int *fn;
    fn = code;
    fn();
}

int jit_try(int halt_pc, int max_run) {
    int vaddr, phys, page, idx, k, hit;
    int *blk;
    int *fn;

    if (!jit_on) return 0;
    if (nextpc != pc + 1) return 0;           // delay slot: sequential-flow premise broken
    vaddr = pc << 2;
    if (SR_IME) {
        // only act when the ITLB fast-path cache already covers this
        // page -- if it doesn't, the interpreter's own fetch will
        // prime it and the next iteration gets a clean answer here
        if ((vaddr >> 13) != itlb_cache_vpage) return 0;
        if (SR_SM != itlb_cache_sm) return 0;
        if (!itlb_cache_xok) return 0;
        phys = itlb_cache_phys | (vaddr & 0x1FFF);
    } else {
        phys = vaddr;
    }
    if (phys & 0x80000000) return 0;          // MMIO: never translate
    phys = phys & (RAM_SIZE - 1);
    page = phys >> 13;
    idx = (phys >> 2) & JIT_CACHE_MASK;

    hit = 0;
    if (jit_tag[idx] == phys && jit_egen[idx] == jit_pagegen[page]) {
        blk = (int *)jit_blk[idx];
        if (!blk) return 0;                   // known not eligible at this phys+generation
        if (blk[1] == pc) hit = 1;            // else: virtual alias -- retranslate below
    }
    if (!hit) {
        jit_egen[idx] = jit_pagegen[page];    // capture BEFORE reading instructions
        jit_tag[idx] = phys;
        jit_blk[idx] = jit_translate(phys, pc, halt_pc == -1);
        blk = (int *)jit_blk[idx];
        if (!blk) return 0;
    }

    k = blk[0];
    jit_count = 0;
    if (k == -1) {
        // fused loop: bounded by the emitted per-iteration budget
        // check instead of a static count (boot mode only, so no
        // halt_pc can fall inside it)
        jit_budget = max_run;
    } else {
        if (k > max_run) return 0;            // would overrun the batch: tick/interrupt
                                              // cadence must stay identical to interpretation
        if (halt_pc >= pc && halt_pc < pc + k) return 0;  // halt lands inside: interpreter
                                              // must get to check it (boot passes -1: never)
    }
    fn = blk + 2;
    fn();
    if (jit_ran) {
        ++jit_nblocks;
        jit_ninstr = jit_ninstr + jit_ran;
    }
    return jit_ran;
}

void jit_init(int enable) {
    int i;

    // jit_pagegen exists unconditionally -- mem.c's store paths bump
    // it whether or not translation is enabled (conditioning the bump
    // on jit_on would cost the same global read as the bump itself).
    jit_pagegen = (int *)malloc(JIT_NPAGES * 8);
    i = 0;
    while (i < JIT_NPAGES) { jit_pagegen[i] = 0; ++i; }

    jit_on = 0;
    if (!enable) return;   // default: off -- see jit.h's status note

    jc_IMM = __opcode("IMM"); jc_PSH = __opcode("PSH");
    jc_LI = __opcode("LI"); jc_SI = __opcode("SI");
    jc_ENT = __opcode("ENT"); jc_LEV = __opcode("LEV");
    jc_ADD = __opcode("ADD"); jc_SUB = __opcode("SUB");
    jc_AND = __opcode("AND"); jc_OR = __opcode("OR"); jc_XOR = __opcode("XOR");
    jc_SHL = __opcode("SHL"); jc_SHR = __opcode("SHR");
    jc_EQ = __opcode("EQ"); jc_NE = __opcode("NE");
    jc_LT = __opcode("LT"); jc_GT = __opcode("GT");
    jc_GE = __opcode("GE");
    jc_JMP = __opcode("JMP"); jc_BZ = __opcode("BZ"); jc_BNZ = __opcode("BNZ");
    jc_JSR = __opcode("JSR"); jc_ADJ = __opcode("ADJ");
    // "LE" CANNOT be looked up by name: c4m's __opcode_match treats a
    // shorter name as a prefix match, and "LEA " is the very first
    // table entry, so __opcode("LE") returns LEA (0) -- which, when
    // emitted as a compare, consumes the following SI word as its
    // operand and silently corrupts the block. This was a REAL bug,
    // found only by boot-log divergence (an l.sfles in the kernel's
    // early string-scan loop left SR_F unwritten); m1-check alone did
    // NOT catch it. Derive LE from its enum neighbor instead -- the
    // comparison block is EQ,NE,LT,GT,LE,GE in every mirror of the
    // opcode table -- and cross-check against the independently
    // resolved GE below. The self-test block at the end of this
    // function then EXECUTES every looked-up opcode against a known
    // result, so any future lookup drift disables the JIT instead of
    // corrupting translations.
    jc_LE = jc_GT + 1;
    if (jc_IMM < 0 || jc_PSH < 0 || jc_LI < 0 || jc_SI < 0) return;
    if (jc_ENT < 0 || jc_LEV < 0 || jc_GT < 0 || jc_GE < 0) return;
    if (jc_JMP < 0 || jc_BZ < 0 || jc_BNZ < 0 || jc_JSR < 0 || jc_ADJ < 0) return;
    if (jc_GE != jc_LE + 1) return;           // enum order EQ..GE violated: refuse to run

    jit_tag = (int *)malloc(JIT_CACHE_ENTRIES * 8);
    jit_egen = (int *)malloc(JIT_CACHE_ENTRIES * 8);
    jit_blk = (int *)malloc(JIT_CACHE_ENTRIES * 8);
    jit_arena = (int *)malloc(JIT_ARENA_WORDS * 8);
    if (!jit_tag || !jit_egen || !jit_blk || !jit_arena) return;
    i = 0;
    while (i < JIT_CACHE_ENTRIES) { jit_tag[i] = -1; jit_blk[i] = 0; jit_egen[i] = 0; ++i; }
    jit_arena_used = 0;
    jit_dry = 0;

    // Self-test: hand-assemble one block exercising EVERY opcode this
    // JIT ever emits (including the invocation mechanism itself), run
    // it, and demand the known answer. A wrong lookup (see the jc_LE
    // comment above), a broken indirect call, or any host-side opcode
    // drift disables the JIT here -- degrading to pure interpretation
    // -- instead of surfacing later as silently wrong guest execution.
    jit_tmp = 1234;
    jit_ep = jit_arena;
    jit_e2(jc_ENT, 0);
    jit_dst((int)&jit_tmp);                            // [&tmp]
    jit_ld((int)&jit_tmp);                             // a = 1234       (LI)
    jit_e1(jc_PSH); jit_e2(jc_IMM, 1000); jit_e1(jc_SUB);  // a = 234
    jit_e1(jc_PSH); jit_e2(jc_IMM, 200); jit_e1(jc_SUB);   // a = 34
    jit_e1(jc_PSH); jit_e2(jc_IMM, 3); jit_e1(jc_AND);     // a = 2
    jit_e1(jc_PSH); jit_e2(jc_IMM, 5); jit_e1(jc_OR);      // a = 7
    jit_e1(jc_PSH); jit_e2(jc_IMM, 1); jit_e1(jc_XOR);     // a = 6
    jit_e1(jc_PSH); jit_e2(jc_IMM, 2); jit_e1(jc_SHL);     // a = 24
    jit_e1(jc_PSH); jit_e2(jc_IMM, 1); jit_e1(jc_SHR);     // a = 12
    jit_e1(jc_PSH); jit_e2(jc_IMM, 100); jit_e1(jc_LT);    // a = (12 < 100) = 1
    jit_e1(jc_PSH); jit_e2(jc_IMM, 1); jit_e1(jc_EQ);      // a = 1
    jit_e1(jc_PSH); jit_e2(jc_IMM, 0); jit_e1(jc_NE);      // a = 1
    jit_e1(jc_PSH); jit_e2(jc_IMM, 1); jit_e1(jc_GE);      // a = (1 >= 1) = 1
    jit_e1(jc_PSH); jit_e2(jc_IMM, 1); jit_e1(jc_LE);      // a = (1 <= 1) = 1
    jit_e1(jc_PSH); jit_e2(jc_IMM, 0); jit_e1(jc_GT);      // a = (1 > 0) = 1
    jit_e1(jc_PSH); jit_e2(jc_IMM, 41); jit_e1(jc_ADD);    // a = 42
    jit_e1(jc_SI);                                     // jit_tmp = 42
    jit_e1(jc_LEV);
    jit_run_selftest(jit_arena);
    if (jit_tmp != 42) {
        printf("jit: opcode self-test failed (got %d, want 42) -- JIT disabled\n", jit_tmp);
        return;
    }
    jit_on = 1;
}
