#include "cpu.h"
#include "mem.h"

int r[NREGS];
int pc, nextpc;
int delayedins;
int EA;

int SR_SM, SR_TEE, SR_IEE, SR_DCE, SR_ICE, SR_DME, SR_IME;
int SR_LEE, SR_CE, SR_F, SR_CY, SR_OV, SR_OVE, SR_DSX, SR_EPH;
int SR_FO, SR_SUMRA, SR_CID;

int group0[2048];
int group1[2048];
int group2[2048];
int TTMR, TTCR;
int PICMR, PICSR;

int dtlb_cache_vpage, dtlb_cache_sm, dtlb_cache_phys, dtlb_cache_rok, dtlb_cache_wok;
int itlb_cache_vpage, itlb_cache_sm, itlb_cache_phys, itlb_cache_xok;

// See cpu.h's header comment and docs/c4or1k-design.md: this is NOT
// the "(v << (32-bits)) >> (32-bits)" idiom. That relies on the left
// shift truncating at bit 31, which never happens on c4lc's 64-bit
// `int` -- it silently reconstructs v unchanged instead of sign-
// extending it. This mask + XOR/subtract form only relies on
// `1 << bits` for a small constant bits, so it is correct at any
// host word width.
int sext(int v, int bits) {
    int signbit;
    v = v & ((1 << bits) - 1);
    signbit = 1 << (bits - 1);
    return (v ^ signbit) - signbit;
}

// Recovers the nonnegative 32-bit-as-unsigned reading of a register
// value that sext() has (possibly) sign-extended above bit 31: used
// wherever OR1000 draws an unsigned/signed distinction c4lc's `int`
// doesn't have (l.sfXXu, l.divu).
int uval(int v) {
    return v & 0xFFFFFFFF;
}

void cpu_check_for_interrupt() {
    if (!SR_IEE) return;
    if (PICMR & PICSR) {
        cpu_exception(EXCEPT_INT, group0[SPR_EEAR_BASE]);
        pc = nextpc; nextpc = pc + 1;
    }
}

void cpu_raise_interrupt(int line) {
    PICSR = PICSR | (1 << line);
    cpu_check_for_interrupt();
}

void cpu_clear_interrupt(int line) {
    PICSR = PICSR & ~(1 << line);
}

// See cpu.h: combines safecpu.js's once-per-64-instructions TTCR
// advance with its every-instruction SR_TEE delivery check into one
// call, made every 64 instructions by main.c -- coarser delivery
// latency than jor1k's (up to 63 instructions late), immaterial for
// unblocking a jiffies-driven wait.
void cpu_tick_check(int clockspeed) {
    int delta;
    if ((TTMR >> 30) != 0) {
        delta = (TTMR & 0xFFFFFFF) - (TTCR & 0xFFFFFFF);
        if (delta < 0) delta = delta + 0xFFFFFFF;
        TTCR = (TTCR + clockspeed) & 0xFFFFFFFF;
        if (delta < clockspeed) {
            if (TTMR & (1 << 29)) TTMR = TTMR | (1 << 28); // set pending
        }
    }
    if (SR_TEE && (TTMR & (1 << 28))) {
        cpu_exception(EXCEPT_TICK, group0[SPR_EEAR_BASE]);
        pc = nextpc; nextpc = pc + 1;
    }
}

// Bit layout, vector list, and the abort-on-unsupported-feature set
// (little-endian mode, context IDs, exception prefix, delay-slot
// exceptions) are all read straight from safecpu.js's SetFlags -- see
// cpu.h.
void cpu_set_flags(int x) {
    int old_SR_IEE;
    SR_SM = (x & (1 << 0)) ? 1 : 0;
    SR_TEE = (x & (1 << 1)) ? 1 : 0;
    old_SR_IEE = SR_IEE;
    SR_IEE = (x & (1 << 2)) ? 1 : 0;
    SR_DCE = (x & (1 << 3)) ? 1 : 0;
    SR_ICE = (x & (1 << 4)) ? 1 : 0;
    SR_DME = (x & (1 << 5)) ? 1 : 0;
    SR_IME = (x & (1 << 6)) ? 1 : 0;
    SR_LEE = (x & (1 << 7)) ? 1 : 0;
    SR_CE = (x & (1 << 8)) ? 1 : 0;
    SR_F = (x & (1 << 9)) ? 1 : 0;
    SR_CY = (x & (1 << 10)) ? 1 : 0;
    SR_OV = (x & (1 << 11)) ? 1 : 0;
    SR_OVE = (x & (1 << 12)) ? 1 : 0;
    SR_DSX = (x & (1 << 13)) ? 1 : 0;
    SR_EPH = (x & (1 << 14)) ? 1 : 0;
    SR_FO = 1;
    SR_SUMRA = (x & (1 << 16)) ? 1 : 0;
    SR_CID = (x >> 28) & 0xF;
    if (SR_LEE) printf("cpu_set_flags: little endian not supported\n");
    if (SR_CID) printf("cpu_set_flags: context id not supported\n");
    if (SR_EPH) printf("cpu_set_flags: exception prefix not supported\n");
    if (SR_DSX) printf("cpu_set_flags: delay slot exception not supported\n");
    if (SR_IEE && !old_SR_IEE) cpu_check_for_interrupt();
}

int cpu_get_flags() {
    int x;
    x = 0;
    if (SR_SM) x = x | (1 << 0);
    if (SR_TEE) x = x | (1 << 1);
    if (SR_IEE) x = x | (1 << 2);
    if (SR_DCE) x = x | (1 << 3);
    if (SR_ICE) x = x | (1 << 4);
    if (SR_DME) x = x | (1 << 5);
    if (SR_IME) x = x | (1 << 6);
    if (SR_LEE) x = x | (1 << 7);
    if (SR_CE) x = x | (1 << 8);
    if (SR_F) x = x | (1 << 9);
    if (SR_CY) x = x | (1 << 10);
    if (SR_OV) x = x | (1 << 11);
    if (SR_OVE) x = x | (1 << 12);
    if (SR_DSX) x = x | (1 << 13);
    if (SR_EPH) x = x | (1 << 14);
    if (SR_FO) x = x | (1 << 15);
    if (SR_SUMRA) x = x | (1 << 16);
    x = x | (SR_CID << 28);
    return sext(x, 32);
}

// Group/address split and per-group dispatch match SetSPR/GetSPR
// exactly (cpu.h). Groups 3/4 (caches) are accepted no-ops, matching
// jor1k; anything else unmatched is treated as this project's
// "unimplemented, halt" fault via the caller checking a -1 sentinel
// isn't meaningful here, so unmatched groups just fall through
// returning 0 / discarding the write with a diagnostic, mirroring
// jor1k's message.Abort() intent without actually aborting the host.
void cpu_set_spr(int idx, int val) {
    int address, group;
    address = idx & 0x7FF;
    group = (idx >> 11) & 0x1F;

    switch (group) {
    case 0:
        if (address == SPR_SR) cpu_set_flags(val);
        group0[address] = val;
        break;
    case 1: group1[address] = val; dtlb_cache_vpage = -1; break;
    case 2: group2[address] = val; itlb_cache_vpage = -1; break;
    case 3: case 4:
        // data/instruction cache: not supported, accepted no-op
        break;
    case 8:
        // accepted no-op
        break;
    case 9:
        if (address == 0) PICMR = val | 0x3; // non-maskable interrupt bits always on
        else if (address == 2) { /* PICSR: writes ignored, matches jor1k */ }
        else printf("cpu_set_spr: unsupported PIC address %d\n", address);
        break;
    case 10:
        if (address == 0) TTMR = val;
        else if (address == 1) TTCR = val;
        else printf("cpu_set_spr: unsupported tick timer address %d\n", address);
        break;
    default:
        printf("cpu_set_spr: unsupported SPR group %d\n", group);
    }
}

int cpu_get_spr(int idx) {
    int address, group;
    address = idx & 0x7FF;
    group = (idx >> 11) & 0x1F;

    switch (group) {
    case 0:
        if (address == SPR_SR) return cpu_get_flags();
        return group0[address];
    case 1: return group1[address];
    case 2: return group2[address];
    case 8: return 0;
    case 9:
        if (address == 0) return PICMR;
        else if (address == 2) return PICSR;
        printf("cpu_get_spr: unsupported PIC address %d\n", address);
        return 0;
    case 10:
        if (address == 0) return TTMR;
        else if (address == 1) return TTCR;
        printf("cpu_get_spr: unsupported tick timer address %d\n", address);
        return 0;
    }
    printf("cpu_get_spr: unsupported SPR group %d\n", group);
    return 0;
}

// EPCR_BASE save timing (pc<<2, minus 4 if the faulting instruction
// was itself a delay slot; EXCEPT_SYSCALL saves pc<<2 PLUS 4 instead)
// is exactly safecpu.js's Exception() -- see cpu.h for why that
// distinction is load-bearing and not just copied for form.
void cpu_exception(int excepttype, int addr) {
    int except_vector;
    except_vector = excepttype; // SR_EPH (exception prefix) is rejected in cpu_set_flags, so it's always 0 here

    cpu_set_spr(SPR_EEAR_BASE, addr);
    cpu_set_spr(SPR_ESR_BASE, cpu_get_flags());

    EA = -1;
    SR_OVE = 0;
    SR_SM = 1;
    SR_IEE = 0;
    SR_TEE = 0;
    SR_DME = 0;

    nextpc = except_vector >> 2;

    if (excepttype == EXCEPT_RESET) {
        // no EPCR save
    } else if (excepttype == EXCEPT_SYSCALL) {
        cpu_set_spr(SPR_EPCR_BASE, sext((pc << 2) + 4 - (delayedins ? 4 : 0), 32));
    } else {
        // ITLBMISS, IPF, DTLBMISS, DPF, BUSERR, TICK, INT, TRAP
        cpu_set_spr(SPR_EPCR_BASE, sext((pc << 2) - (delayedins ? 4 : 0), 32));
    }

    if (excepttype == EXCEPT_TICK && ((TTMR >> 30) == 0x1)) TTCR = 0;

    delayedins = 0;
    SR_IME = 0;
}

// DTLB/ITLB checks: identity map when the respective MMU-enable flag
// is off (M1's whole-project default), otherwise a match-register
// validity + tag check and a permission check -- NOT a page-table
// walk. Real OR1000 software (the guest kernel) walks its own page
// tables and installs the resulting entry with l.mtspr from its
// TLB-miss handler; this function's job is only to notice there's no
// entry (or no permission) and raise the vector, exactly as
// safecpu.js's DTLBLookup/GetInstruction do. See cpu.h for why TLB
// LRU bits aren't checked at all.
//
// M9: cold path only (see cpu.h's dtlb_cache_* comment), called by
// cpu_run_batch's DTLB_FAST macro only when SR_DME is on and the
// cache doesn't already match. A tag mismatch raises EXCEPT_DTLBMISS
// without touching the cache, same reasoning as fetch_ins's ITLB
// miss path. A tag match populates dtlb_cache_phys/_rok/_wok --
// BOTH permission decisions are made once here (not just the one
// `write` asked about), since the next access to this cached page
// might be a different direction and byproduct-caching the other
// direction's answer is free once tlbtr is already in hand.
int dtlb_lookup(int addr, int write) {
    int setindex, tlmbr, tlbtr, vpage;
    if (!SR_DME) return addr;
    vpage = addr >> 13;
    setindex = vpage & 63;
    tlmbr = group1[0x200 | setindex];
    if (((tlmbr & 1) == 0) || ((tlmbr >> 19) != (addr >> 19))) {
        cpu_exception(EXCEPT_DTLBMISS, addr);
        return -1;
    }
    tlbtr = group1[0x280 | setindex];
    dtlb_cache_vpage = vpage;
    dtlb_cache_sm = SR_SM;
    dtlb_cache_phys = tlbtr & 0xFFFFE000;
    dtlb_cache_rok = SR_SM ? ((tlbtr & 0x100) != 0) : ((tlbtr & 0x40) != 0);
    dtlb_cache_wok = SR_SM ? ((tlbtr & 0x200) != 0) : ((tlbtr & 0x80) != 0);
    if ((write && !dtlb_cache_wok) || (!write && !dtlb_cache_rok)) {
        cpu_exception(EXCEPT_DPF, addr);
        return -1;
    }
    return dtlb_cache_phys | (addr & 0x1FFF);
}

// Returns the fetched word, or -1 if an ITLB miss/fault was raised
// (cpu_run_batch must then skip decode entirely for this cycle,
// matching safecpu.js's Step loop: `if (ins==-1) { pc=nextpc++;
// continue; }`).
//
// M9: cold path only, called by cpu_run_batch's inlined fast path
// (see cpu.h's itlb_cache_* comment) only when SR_IME is on and the
// cache doesn't already match -- a new page, a new privilege mode, or
// an invalidation from cpu_set_spr. Does the real group2 tag-match
// lookup; a tag mismatch raises EXCEPT_ITLBMISS without touching the
// cache (there's no valid translation yet to cache -- the guest's own
// miss handler installs one via l.mtspr, which invalidates
// itlb_cache_vpage, forcing a fresh lookup on the retry after l.rfe).
// A tag match populates itlb_cache_phys/_xok -- the permission
// decision is made ONCE here, not re-derived on every cache hit,
// since it can't change while the cache entry stays valid.
int fetch_ins(int addr) {
    int setindex, tlmbr, tlbtr, vpage;
    vpage = addr >> 13;
    setindex = vpage & 63;
    tlmbr = group2[0x200 | setindex];
    if (((tlmbr & 1) == 0) || ((tlmbr >> 19) != (addr >> 19))) {
        cpu_exception(EXCEPT_ITLBMISS, pc << 2);
        return -1;
    }
    tlbtr = group2[0x280 | setindex];
    itlb_cache_vpage = vpage;
    itlb_cache_sm = SR_SM;
    itlb_cache_phys = tlbtr & 0xFFFFE000;
    itlb_cache_xok = SR_SM ? ((tlbtr & 0x40) != 0) : ((tlbtr & 0x80) != 0);
    if (!itlb_cache_xok) { cpu_exception(EXCEPT_IPF, pc << 2); return -1; }
    return ram_lw(itlb_cache_phys | (addr & 0x1FFF));
}

void cpu_reset() {
    int i;
    for (i = 0; i < NREGS; ++i) r[i] = 0;
    for (i = 0; i < 2048; ++i) { group0[i] = 0; group1[i] = 0; group2[i] = 0; }

    TTMR = 0; TTCR = 0;
    PICMR = 0x3; PICSR = 0;
    delayedins = 0;
    EA = -1;

    dtlb_cache_vpage = -1;
    itlb_cache_vpage = -1;

    group0[SPR_IMMUCFGR] = 0x18;
    group0[SPR_DMMUCFGR] = 0x18;
    group0[SPR_ICCFGR] = 0x48;
    group0[SPR_DCCFGR] = 0x48;
    group0[SPR_VR] = 0x12000001;
    group0[SPR_UPR] = 0x619;

    // Flags per safecpu.js's constructor (SR_SM/SR_FO on, all else
    // off) -- NOT routed through cpu_exception(EXCEPT_RESET, ...) the
    // way jor1k's own Reset() does, so pc/nextpc land at 0/1, not the
    // 0x100 reset vector. Test programs load at guest address 0 and
    // have no vector to honor; the real vector-based cold boot is
    // M4's concern (loading vmlinux.bin and starting execution the
    // way real hardware does).
    SR_SM = 1; SR_TEE = 0; SR_IEE = 0; SR_DCE = 0; SR_ICE = 0;
    SR_DME = 0; SR_IME = 0; SR_LEE = 0; SR_CE = 0; SR_F = 0;
    SR_CY = 0; SR_OV = 0; SR_OVE = 0; SR_DSX = 0; SR_EPH = 0;
    SR_FO = 1; SR_SUMRA = 0; SR_CID = 0;

    pc = 0; nextpc = 1;
}

void cpu_dump() {
    int i;
    printf("pc=%d nextpc=%d SR_F=%d SR_CY=%d SR_OV=%d\n", pc, nextpc, SR_F, SR_CY, SR_OV);
    printf("SR_SM=%d SR_TEE=%d SR_IEE=%d SR_DME=%d SR_IME=%d\n", SR_SM, SR_TEE, SR_IEE, SR_DME, SR_IME);
    printf("EPCR=%d EEAR=%d ESR=%d\n", cpu_get_spr(SPR_EPCR_BASE), cpu_get_spr(SPR_EEAR_BASE), cpu_get_spr(SPR_ESR_BASE));
    printf("TTMR=%d TTCR=%d PICMR=%d PICSR=%d\n", TTMR, TTCR, PICMR, PICSR);
    for (i = 0; i < NREGS; ++i) {
        printf("r%d=%d\n", i, r[i]);
    }
}

// M9: the DTLB_FAST-macro-avoidance idea from itlb_cache_* (cpu.h)
// applied to the ten load/store sites in cpu_run_batch below -- a
// function-like macro instead of ten hand-copies of the same six
// lines, since ten near-identical inline blocks are ten chances for
// one of them to drift or typo. Expands to a statement (not an
// expression), so it's always used as `DTLB_FAST(addr, 0-or-1,
// dest);` -- vpage is cpu_run_batch's own local, already declared for
// the ITLB fast path above, reused here.
#define DTLB_FAST(a, wr, out) { \
    vpage = (a) >> 13; \
    if (!SR_DME) out = (a); \
    else if (vpage == dtlb_cache_vpage && SR_SM == dtlb_cache_sm) { \
        if (wr ? !dtlb_cache_wok : !dtlb_cache_rok) { cpu_exception(EXCEPT_DPF, (a)); out = -1; } \
        else out = dtlb_cache_phys | ((a) & 0x1FFF); \
    } else out = dtlb_lookup((a), wr); \
}

// switch/case, matching mmio.c/uart.c/virtio.c and this file's own
// cpu_set_spr/cpu_get_spr above (and safecpu.js's own switch(ins>>>26)
// structure, the porting reference). This was briefly reverted to
// if/else-if during M5 after a real boot's SIGSEGV appeared to
// correlate with whether this function used switch -- that diagnosis
// was wrong. The actual bug was bootfs.c's rd_le32 not sign-extending
// -1 sentinels (fixed the same milestone); the switch/if-else choice
// here just happened to shift malloc's heap layout enough to change
// whether the resulting wild pointer landed in mapped memory. Restored
// after re-testing a full boot against the real fix with this function
// back to switch/case: clean, no crash, same result as if/else. See
// docs/c4or1k-design.md's M5 section for the full bisection story.
//
// M9: this was cpu_step(halt_pc), executing exactly one instruction
// per call. The body below is unchanged per-instruction -- every
// mid-switch `return 0` (branch taken, l.rfe, l.mtspr) became
// `continue` to the next batch iteration instead, and the four
// sub-switch "unimplemented func" paths plus the outer "unimplemented
// opcode" path now set `fault` and break out to a single post-switch
// check instead of returning 2 directly, since a fault must end the
// whole batch (not just this switch), and c4lc has no labeled
// break/goto to jump out of nested switches directly. See cpu.h for
// why batching this way, not fastcpu.js's fence-at-jump scheme.
int cpu_run_batch(int halt_pc, int max_batch, int *ran) {
    int bn, fault, ins, opcode, rd, ra, rb, rA, rB, imm, simm, func, jump, i, result, addr, phys, vpage;

    for (bn = 0; bn < max_batch; ++bn) {
    if (pc == halt_pc) { *ran = bn; return 1; }

    // M9: inlined fast path for the itlb_cache_* hit case (cpu.h) --
    // avoids the fetch_ins() call entirely (not just its internal
    // work) on what is, once SR_IME is on, the overwhelming majority
    // of instruction fetches. fetch_ins is still called, unchanged,
    // for the cold path (SR_IME off, or an actual cache miss).
    addr = pc << 2;
    if (!SR_IME) {
        ins = ram_lw(addr);
    } else {
        vpage = addr >> 13;
        if (vpage == itlb_cache_vpage && SR_SM == itlb_cache_sm) {
            if (!itlb_cache_xok) { cpu_exception(EXCEPT_IPF, addr); ins = -1; }
            else ins = ram_lw(itlb_cache_phys | (addr & 0x1FFF));
        } else {
            ins = fetch_ins(addr);
        }
    }
    if (ins == -1) {
        pc = nextpc; nextpc = pc + 1;
        continue;
    }

    opcode = (ins >> 26) & 0x3F;
    rd = (ins >> 21) & 0x1F;
    ra = (ins >> 16) & 0x1F;
    rb = (ins >> 11) & 0x1F;
    rA = r[ra];
    rB = r[rb];
    fault = 0;
    // M11: `imm` used to be computed unconditionally here (every
    // instruction paid a sext() *function call* -- JSR/ENT/LEV
    // overhead, not just a shift+mask -- regardless of whether that
    // opcode uses an immediate at all). Roughly two thirds of the
    // opcodes below don't touch imm (branches, register-register ALU,
    // l.mtspr/l.rfe, stores, which already compute their own simm
    // lazily the same way). `simm` was already lazy; this makes imm
    // match it -- each of the nine cases that actually needs imm
    // computes it as its own first statement instead. See
    // docs/c4or1k-design.md's M11 section for why this replaced the
    // originally-scoped "decode cache" idea: a real accounting of
    // c4lc's VM-op cost showed shift/mask decode (opcode/rd/ra/rb)
    // is already cheap enough that caching it doesn't clearly help,
    // but an eager function call for a value ~2/3 of instructions
    // never use clearly does.

    switch (opcode) {
    case 0x00:                         // l.j
        jump = pc + sext(ins, 26);
        pc = nextpc; nextpc = jump; delayedins = 1;
        continue;
    case 0x01:                         // l.jal
        r[9] = sext((nextpc << 2) + 4, 32);
        jump = pc + sext(ins, 26);
        pc = nextpc; nextpc = jump; delayedins = 1;
        continue;
    case 0x03:                         // l.bnf
        if (!SR_F) {
            jump = pc + sext(ins, 26);
            pc = nextpc; nextpc = jump; delayedins = 1;
            continue;
        }
        break;
    case 0x04:                         // l.bf
        if (SR_F) {
            jump = pc + sext(ins, 26);
            pc = nextpc; nextpc = jump; delayedins = 1;
            continue;
        }
        break;
    case 0x05:                         // l.nop
        break;
    case 0x06:                         // l.movhi
        r[rd] = sext((ins & 0xFFFF) << 16, 32);
        break;
    case 0x08:                         // l.sys / l.trap
        if ((ins & 0xFFFF0000) == 0x21000000) cpu_exception(EXCEPT_TRAP, group0[SPR_EEAR_BASE]);
        else cpu_exception(EXCEPT_SYSCALL, group0[SPR_EEAR_BASE]);
        break;
    case 0x09:                         // l.rfe
        nextpc = cpu_get_spr(SPR_EPCR_BASE) >> 2;
        pc = nextpc; nextpc = pc + 1; delayedins = 0;
        cpu_set_flags(cpu_get_spr(SPR_ESR_BASE));
        continue;
    case 0x11:                         // l.jr
        jump = rB >> 2;
        pc = nextpc; nextpc = jump; delayedins = 1;
        continue;
    case 0x12:                         // l.jalr
        r[9] = sext((nextpc << 2) + 4, 32);
        jump = rB >> 2;
        pc = nextpc; nextpc = jump; delayedins = 1;
        continue;
    case 0x1B:                         // l.lwa
        imm = sext(ins, 16);
        addr = rA + imm;
        DTLB_FAST(addr, 0, phys);
        if (phys != -1) {
            EA = phys;
            r[rd] = sext(ram_lw(phys), 32);
        }
        break;
    case 0x21:                         // l.lwz
        imm = sext(ins, 16);
        addr = rA + imm;
        DTLB_FAST(addr, 0, phys);
        if (phys != -1) r[rd] = sext(ram_lw(phys), 32);
        break;
    case 0x23:                         // l.lbz
        imm = sext(ins, 16);
        addr = rA + imm;
        DTLB_FAST(addr, 0, phys);
        if (phys != -1) r[rd] = ram_lb(phys);
        break;
    case 0x24:                         // l.lbs
        imm = sext(ins, 16);
        addr = rA + imm;
        DTLB_FAST(addr, 0, phys);
        if (phys != -1) r[rd] = sext(ram_lb(phys), 8);
        break;
    case 0x25:                         // l.lhz
        imm = sext(ins, 16);
        addr = rA + imm;
        DTLB_FAST(addr, 0, phys);
        if (phys != -1) r[rd] = ram_lh(phys);
        break;
    case 0x26:                         // l.lhs
        imm = sext(ins, 16);
        addr = rA + imm;
        DTLB_FAST(addr, 0, phys);
        if (phys != -1) r[rd] = sext(ram_lh(phys), 16);
        break;
    case 0x27:                         // l.addi
        imm = sext(ins, 16);
        result = sext(rA + imm, 32);
        SR_CY = result < rA;
        SR_OV = (((rA ^ imm ^ -1) & (rA ^ result)) & 0x80000000) ? 1 : 0;
        r[rd] = result;
        break;
    case 0x29:                         // l.andi
        r[rd] = rA & (ins & 0xFFFF);
        break;
    case 0x2A:                         // l.ori
        r[rd] = rA | (ins & 0xFFFF);
        break;
    case 0x2B:                         // l.xori
        imm = sext(ins, 16);
        r[rd] = sext(rA ^ imm, 32);
        break;
    case 0x2D:                         // l.mfspr
        r[rd] = cpu_get_spr(rA | (ins & 0xFFFF));
        break;
    case 0x2E:                         // l.slli / l.srli / l.srai
        func = (ins >> 6) & 0x3;
        switch (func) {
        case 0: r[rd] = sext(rA << (ins & 0x1F), 32); break;  // slli
        case 1: r[rd] = uval(rA) >> (ins & 0x1F); break;       // srli (logical; safecpu.js's own comment mislabels this "rori" -- the code is `>>>`, see cpu.h)
        case 2: r[rd] = rA >> (ins & 0x1F); break;             // srai (arithmetic -- c4lc's native >> is exactly this)
        default:
            printf("cpu_step: unimplemented 0x2E func at pc=%d (ins=0x%x)\n", pc, ins); cpu_dump(); fault = 1; break;
        }
        break;
    case 0x2F:                         // l.sfXXi
        func = (ins >> 21) & 0x1F;
        imm = sext(ins, 16);
        switch (func) {
        case 0x0: SR_F = (rA == imm); break;
        case 0x1: SR_F = (rA != imm); break;
        case 0x2: SR_F = (uval(rA) > uval(imm)); break;
        case 0x3: SR_F = (uval(rA) >= uval(imm)); break;
        case 0x4: SR_F = (uval(rA) < uval(imm)); break;
        case 0x5: SR_F = (uval(rA) <= uval(imm)); break;
        case 0xa: SR_F = (rA > imm); break;
        case 0xb: SR_F = (rA >= imm); break;
        case 0xc: SR_F = (rA < imm); break;
        case 0xd: SR_F = (rA <= imm); break;
        default:
            printf("cpu_step: unimplemented 0x2F func at pc=%d (ins=0x%x)\n", pc, ins); cpu_dump(); fault = 1; break;
        }
        break;
    case 0x30:                         // l.mtspr
        simm = ((ins >> 10) & 0xF800) | (ins & 0x7FF); // NOT sign-extended: an SPR-index component, not a byte offset
        pc = nextpc; nextpc = pc + 1; delayedins = 0;
        cpu_set_spr(rA | simm, rB);
        continue;
    case 0x33:                         // l.swa
        simm = sext(((ins >> 10) & 0xF800) | (ins & 0x7FF), 16);
        addr = rA + simm;
        DTLB_FAST(addr, 1, phys);
        if (phys != -1) {
            SR_F = (phys == EA);
            EA = -1;
            if (SR_F) ram_sw(phys, rB);
        }
        break;
    case 0x35:                         // l.sw
        simm = sext(((ins >> 10) & 0xF800) | (ins & 0x7FF), 16);
        addr = rA + simm;
        DTLB_FAST(addr, 1, phys);
        if (phys != -1) ram_sw(phys, rB);
        break;
    case 0x36:                         // l.sb
        simm = sext(((ins >> 10) & 0xF800) | (ins & 0x7FF), 16);
        addr = rA + simm;
        DTLB_FAST(addr, 1, phys);
        if (phys != -1) ram_sb(phys, rB);
        break;
    case 0x37:                         // l.sh
        simm = sext(((ins >> 10) & 0xF800) | (ins & 0x7FF), 16);
        addr = rA + simm;
        DTLB_FAST(addr, 1, phys);
        if (phys != -1) ram_sh(phys, rB);
        break;
    case 0x38:                         // three-operand ALU
        func = ins & 0x3CF;
        switch (func) {
        case 0x0:                                      // add
            result = sext(rA + rB, 32);
            SR_CY = result < rA;
            SR_OV = (((rA ^ rB ^ -1) & (rA ^ result)) & 0x80000000) ? 1 : 0;
            r[rd] = result;
            break;
        case 0x2:                                       // sub
            result = sext(rA - rB, 32);
            SR_CY = rB > rA;
            SR_OV = (((rA ^ rB) & (rA ^ result)) & 0x80000000) ? 1 : 0;
            r[rd] = result;
            break;
        case 0x3: r[rd] = rA & rB; break;                // and
        case 0x4: r[rd] = rA | rB; break;                // or
        case 0x5: r[rd] = sext(rA ^ rB, 32); break;      // xor
        case 0x8: r[rd] = sext(rA << (rB & 0x1F), 32); break; // sll
        case 0x48: r[rd] = uval(rA) >> (rB & 0x1F); break;    // srl
        case 0x88: r[rd] = rA >> (rB & 0x1F); break;          // sra
        case 0xf:                                        // ff1
            r[rd] = 0;
            for (i = 0; i < 32; ++i) {
                if (rA & (1 << i)) { r[rd] = i + 1; break; }
            }
            break;
        case 0x10f:                                       // fl1
            r[rd] = 0;
            for (i = 31; i >= 0; --i) {
                if (rA & (1 << i)) { r[rd] = i + 1; break; }
            }
            break;
        case 0x306:                                        // mul
            result = sext(rA * rB, 32);
            SR_OV = (rA * rB < -2147483648 || rA * rB > 2147483647) ? 1 : 0;
            SR_CY = (uval(rA) * uval(rB) > 4294967295) ? 1 : 0;
            r[rd] = result;
            break;
        case 0x30a:                                         // divu
            SR_CY = (rB == 0);
            SR_OV = 0;
            if (!SR_CY) r[rd] = uval(rA) / uval(rB);
            break;
        case 0x309:                                          // div
            SR_CY = (rB == 0);
            SR_OV = 0;
            if (!SR_CY) r[rd] = sext(rA / rB, 32);
            break;
        default:
            printf("cpu_step: unimplemented 0x38 func at pc=%d (ins=0x%x)\n", pc, ins); cpu_dump(); fault = 1; break;
        }
        break;
    case 0x39:                         // l.sfXX
        func = (ins >> 21) & 0x1F;
        switch (func) {
        case 0x0: SR_F = (rA == rB); break;
        case 0x1: SR_F = (rA != rB); break;
        case 0x2: SR_F = (uval(rA) > uval(rB)); break;
        case 0x3: SR_F = (uval(rA) >= uval(rB)); break;
        case 0x4: SR_F = (uval(rA) < uval(rB)); break;
        case 0x5: SR_F = (uval(rA) <= uval(rB)); break;
        case 0xa: SR_F = (rA > rB); break;
        case 0xb: SR_F = (rA >= rB); break;
        case 0xc: SR_F = (rA < rB); break;
        case 0xd: SR_F = (rA <= rB); break;
        default:
            printf("cpu_step: unimplemented 0x39 func at pc=%d (ins=0x%x)\n", pc, ins); cpu_dump(); fault = 1; break;
        }
        break;
    default:
        printf("cpu_step: unimplemented opcode 0x%x at pc=%d (ins=0x%x)\n", opcode, pc, ins);
        cpu_dump();
        fault = 1;
    }

    if (fault) { *ran = bn; return 2; }

    r[0] = 0; // nothing here writes r0; kept explicit, matches M0
    pc = nextpc; nextpc = pc + 1; delayedins = 0;
    }

    *ran = max_batch;
    return 0;
}
