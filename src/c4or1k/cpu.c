#include "cpu.h"
#include "mem.h"

int r[NREGS];
int SR_F, SR_CY, SR_OV;
int pc, nextpc;
int EA;

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

void cpu_reset() {
    int i;
    for (i = 0; i < NREGS; ++i) r[i] = 0;
    SR_F = 0; SR_CY = 0; SR_OV = 0;
    EA = -1;
    pc = 0; nextpc = 1;
}

void cpu_dump() {
    int i;
    printf("pc=%d nextpc=%d SR_F=%d SR_CY=%d SR_OV=%d\n", pc, nextpc, SR_F, SR_CY, SR_OV);
    for (i = 0; i < NREGS; ++i) {
        printf("r%d=%d\n", i, r[i]);
    }
}

int cpu_step(int halt_pc) {
    int ins, opcode, rd, ra, rb, rA, rB, imm, simm, func, jump, i, result;

    if (pc == halt_pc) return 1;

    ins = ram_lw(pc << 2);
    opcode = (ins >> 26) & 0x3F;
    rd = (ins >> 21) & 0x1F;
    ra = (ins >> 16) & 0x1F;
    rb = (ins >> 11) & 0x1F;
    rA = r[ra];
    rB = r[rb];
    imm = sext(ins, 16);

    if (opcode == 0x00) {              // l.j
        jump = pc + sext(ins, 26);
        pc = nextpc; nextpc = jump;
        return 0;
    } else if (opcode == 0x01) {       // l.jal
        r[9] = sext((nextpc << 2) + 4, 32);
        jump = pc + sext(ins, 26);
        pc = nextpc; nextpc = jump;
        return 0;
    } else if (opcode == 0x03) {       // l.bnf
        if (!SR_F) {
            jump = pc + sext(ins, 26);
            pc = nextpc; nextpc = jump;
            return 0;
        }
    } else if (opcode == 0x04) {       // l.bf
        if (SR_F) {
            jump = pc + sext(ins, 26);
            pc = nextpc; nextpc = jump;
            return 0;
        }
    } else if (opcode == 0x05) {       // l.nop
        // nothing
    } else if (opcode == 0x06) {       // l.movhi
        r[rd] = sext((ins & 0xFFFF) << 16, 32);
    } else if (opcode == 0x11) {       // l.jr
        jump = rB >> 2;
        pc = nextpc; nextpc = jump;
        return 0;
    } else if (opcode == 0x12) {       // l.jalr
        r[9] = sext((nextpc << 2) + 4, 32);
        jump = rB >> 2;
        pc = nextpc; nextpc = jump;
        return 0;
    } else if (opcode == 0x1B) {       // l.lwa
        result = rA + imm;
        EA = result;
        r[rd] = sext(ram_lw(result), 32);
    } else if (opcode == 0x21) {       // l.lwz
        r[rd] = sext(ram_lw(rA + imm), 32);
    } else if (opcode == 0x23) {       // l.lbz
        r[rd] = ram_lb(rA + imm);
    } else if (opcode == 0x24) {       // l.lbs
        r[rd] = sext(ram_lb(rA + imm), 8);
    } else if (opcode == 0x25) {       // l.lhz
        r[rd] = ram_lh(rA + imm);
    } else if (opcode == 0x26) {       // l.lhs
        r[rd] = sext(ram_lh(rA + imm), 16);
    } else if (opcode == 0x27) {       // l.addi
        result = sext(rA + imm, 32);
        SR_CY = result < rA;
        SR_OV = (((rA ^ imm ^ -1) & (rA ^ result)) & 0x80000000) ? 1 : 0;
        r[rd] = result;
    } else if (opcode == 0x29) {       // l.andi
        r[rd] = rA & (ins & 0xFFFF);
    } else if (opcode == 0x2A) {       // l.ori
        r[rd] = rA | (ins & 0xFFFF);
    } else if (opcode == 0x2B) {       // l.xori
        r[rd] = sext(rA ^ imm, 32);
    } else if (opcode == 0x2E) {       // l.slli / l.srli / l.srai
        func = (ins >> 6) & 0x3;
        if (func == 0) r[rd] = sext(rA << (ins & 0x1F), 32);       // slli
        else if (func == 1) r[rd] = uval(rA) >> (ins & 0x1F);       // srli (logical; safecpu.js's own comment mislabels this "rori" -- the code is `>>>`, see cpu.h)
        else if (func == 2) r[rd] = rA >> (ins & 0x1F);              // srai (arithmetic -- c4lc's native >> is exactly this)
        else { printf("cpu_step: unimplemented 0x2E func at pc=%d (ins=0x%x)\n", pc, ins); cpu_dump(); return 2; }
    } else if (opcode == 0x2F) {       // l.sfXXi
        func = (ins >> 21) & 0x1F;
        if (func == 0x0) SR_F = (rA == imm);
        else if (func == 0x1) SR_F = (rA != imm);
        else if (func == 0x2) SR_F = (uval(rA) > uval(imm));
        else if (func == 0x3) SR_F = (uval(rA) >= uval(imm));
        else if (func == 0x4) SR_F = (uval(rA) < uval(imm));
        else if (func == 0x5) SR_F = (uval(rA) <= uval(imm));
        else if (func == 0xa) SR_F = (rA > imm);
        else if (func == 0xb) SR_F = (rA >= imm);
        else if (func == 0xc) SR_F = (rA < imm);
        else if (func == 0xd) SR_F = (rA <= imm);
        else { printf("cpu_step: unimplemented 0x2F func at pc=%d (ins=0x%x)\n", pc, ins); cpu_dump(); return 2; }
    } else if (opcode == 0x33) {       // l.swa
        simm = sext(((ins >> 10) & 0xF800) | (ins & 0x7FF), 16);
        result = rA + simm;
        SR_F = (result == EA);
        EA = -1;
        if (SR_F) ram_sw(result, rB);
    } else if (opcode == 0x35) {       // l.sw
        simm = sext(((ins >> 10) & 0xF800) | (ins & 0x7FF), 16);
        ram_sw(rA + simm, rB);
    } else if (opcode == 0x36) {       // l.sb
        simm = sext(((ins >> 10) & 0xF800) | (ins & 0x7FF), 16);
        ram_sb(rA + simm, rB);
    } else if (opcode == 0x37) {       // l.sh
        simm = sext(((ins >> 10) & 0xF800) | (ins & 0x7FF), 16);
        ram_sh(rA + simm, rB);
    } else if (opcode == 0x38) {       // three-operand ALU
        func = ins & 0x3CF;
        if (func == 0x0) {                          // add
            result = sext(rA + rB, 32);
            SR_CY = result < rA;
            SR_OV = (((rA ^ rB ^ -1) & (rA ^ result)) & 0x80000000) ? 1 : 0;
            r[rd] = result;
        } else if (func == 0x2) {                    // sub
            result = sext(rA - rB, 32);
            SR_CY = rB > rA;
            SR_OV = (((rA ^ rB) & (rA ^ result)) & 0x80000000) ? 1 : 0;
            r[rd] = result;
        } else if (func == 0x3) r[rd] = rA & rB;      // and
        else if (func == 0x4) r[rd] = rA | rB;        // or
        else if (func == 0x5) r[rd] = sext(rA ^ rB, 32); // xor
        else if (func == 0x8) r[rd] = sext(rA << (rB & 0x1F), 32); // sll
        else if (func == 0x48) r[rd] = uval(rA) >> (rB & 0x1F);    // srl
        else if (func == 0x88) r[rd] = rA >> (rB & 0x1F);          // sra
        else if (func == 0xf) {                       // ff1
            r[rd] = 0;
            for (i = 0; i < 32; ++i) {
                if (rA & (1 << i)) { r[rd] = i + 1; break; }
            }
        } else if (func == 0x10f) {                    // fl1
            r[rd] = 0;
            for (i = 31; i >= 0; --i) {
                if (rA & (1 << i)) { r[rd] = i + 1; break; }
            }
        } else if (func == 0x306) {                     // mul
            result = sext(rA * rB, 32);
            SR_OV = (rA * rB < -2147483648 || rA * rB > 2147483647) ? 1 : 0;
            SR_CY = (uval(rA) * uval(rB) > 4294967295) ? 1 : 0;
            r[rd] = result;
        } else if (func == 0x30a) {                      // divu
            SR_CY = (rB == 0);
            SR_OV = 0;
            if (!SR_CY) r[rd] = uval(rA) / uval(rB);
        } else if (func == 0x309) {                       // div
            SR_CY = (rB == 0);
            SR_OV = 0;
            if (!SR_CY) r[rd] = sext(rA / rB, 32);
        } else { printf("cpu_step: unimplemented 0x38 func at pc=%d (ins=0x%x)\n", pc, ins); cpu_dump(); return 2; }
    } else if (opcode == 0x39) {       // l.sfXX
        func = (ins >> 21) & 0x1F;
        if (func == 0x0) SR_F = (rA == rB);
        else if (func == 0x1) SR_F = (rA != rB);
        else if (func == 0x2) SR_F = (uval(rA) > uval(rB));
        else if (func == 0x3) SR_F = (uval(rA) >= uval(rB));
        else if (func == 0x4) SR_F = (uval(rA) < uval(rB));
        else if (func == 0x5) SR_F = (uval(rA) <= uval(rB));
        else if (func == 0xa) SR_F = (rA > rB);
        else if (func == 0xb) SR_F = (rA >= rB);
        else if (func == 0xc) SR_F = (rA < rB);
        else if (func == 0xd) SR_F = (rA <= rB);
        else { printf("cpu_step: unimplemented 0x39 func at pc=%d (ins=0x%x)\n", pc, ins); cpu_dump(); return 2; }
    } else {
        printf("cpu_step: unimplemented opcode 0x%x at pc=%d (ins=0x%x)\n", opcode, pc, ins);
        cpu_dump();
        return 2;
    }

    r[0] = 0; // nothing here writes r0; kept explicit, matches M0
    pc = nextpc; nextpc = pc + 1;
    return 0;
}
