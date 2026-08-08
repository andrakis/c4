#!/usr/bin/env python3
# A tiny two-pass OR1000 "assembler" for hand-written c4or1k test
# programs: labels + one mnemonic per line, one word per instruction
# (no directives, no macros -- just enough to keep M1's test program
# and its jor1k-oracle counterpart in sync without hand-computing
# branch offsets).
#
# Encodings and field positions are read directly from
# jor1k/js/worker/or1k/safecpu.js (see cpu.h's header comment for why
# that matters more than matching the OR1000 spec from memory).
#
# pc/branch offsets are in WORDS, relative to the branch/jump
# instruction's own address, matching safecpu.js's convention (its
# `pc` is already a word index, and it adds the raw sign-extended
# immediate to it with no <<2).

import re
import sys

REG = re.compile(r"r(\d+)$")

def reg(tok):
    m = REG.match(tok)
    if not m:
        raise ValueError(f"not a register: {tok!r}")
    n = int(m.group(1))
    assert 0 <= n <= 31
    return n

# op -> (kind, opcode, subop)
THREE_REG = {  # (0x38<<26)|(rD<<21)|(rA<<16)|(rB<<11)|func
    "add": 0x0, "sub": 0x2, "and": 0x3, "or": 0x4, "xor": 0x5,
    "sll": 0x8, "srl": 0x48, "ff1": 0xf, "sra": 0x88, "fl1": 0x10f,
    "mul": 0x306, "divu": 0x30a, "div": 0x309,
}
SF_REG = {  # (0x39<<26)|(subop<<21)|(rA<<16)|(rB<<11)
    "sfeq": 0x0, "sfne": 0x1, "sfgtu": 0x2, "sfgeu": 0x3, "sfltu": 0x4,
    "sfleu": 0x5, "sfgts": 0xa, "sfges": 0xb, "sflts": 0xc, "sfles": 0xd,
}
IMM_OP = {  # (op<<26)|(rD<<21)|(rA<<16)|(imm&0xFFFF)
    "movhi": 0x06, "addi": 0x27, "andi": 0x29, "ori": 0x2A, "xori": 0x2B,
}
SF_IMM = {  # (0x2F<<26)|(subop<<21)|(rA<<16)|(imm&0xFFFF)
    "sfeqi": 0x0, "sfnei": 0x1, "sfgtui": 0x2, "sfgeui": 0x3, "sfltui": 0x4,
    "sfleui": 0x5, "sfgtsi": 0xa, "sfgesi": 0xb, "sfltsi": 0xc, "sflesi": 0xd,
}
SHIFT_IMM = {"slli": 0, "srli": 1, "srai": 2}  # (0x2E<<26)|(rD<<21)|(rA<<16)|(func<<6)|(imm&0x1F)
LOAD_OP = {"lwz": 0x21, "lbz": 0x23, "lbs": 0x24, "lhz": 0x25, "lhs": 0x26, "lwa": 0x1B}
STORE_OP = {"sw": 0x35, "sb": 0x36, "sh": 0x37, "swa": 0x33}
BRANCH_OP = {"j": 0x0, "jal": 0x1, "bnf": 0x3, "bf": 0x4}


def parse_lines(text):
    out = []
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.endswith(":"):
            out.append(("label", line[:-1]))
            continue
        parts = line.replace(",", " ").split()
        out.append(("ins", parts[0], parts[1:]))
    return out


def assemble(text):
    lines = parse_lines(text)

    # pass 1: label -> word address
    labels = {}
    addr = 0
    for item in lines:
        if item[0] == "label":
            labels[item[1]] = addr
        else:
            addr += 1

    def parse_imm(tok):
        # %hi(label)/%lo(label): label's BYTE address (word_addr*4),
        # for loading a jump target into a register with movhi+ori
        # ahead of l.jalr/l.jr (see tests/m1_test.s's jalr case).
        m = re.match(r"%hi\((\w+)\)$", tok)
        if m:
            return (labels[m.group(1)] * 4) >> 16
        m = re.match(r"%lo\((\w+)\)$", tok)
        if m:
            return (labels[m.group(1)] * 4) & 0xFFFF
        return int(tok, 0)

    # pass 2: encode
    words = []
    addr = 0
    for item in lines:
        if item[0] == "label":
            continue
        mnem, args = item[1], item[2]

        if mnem == "nop":
            w = (0x5 << 26)
        elif mnem in THREE_REG:
            rd, ra, rb = reg(args[0]), reg(args[1]), reg(args[2])
            w = (0x38 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | THREE_REG[mnem]
        elif mnem in SF_REG:
            ra, rb = reg(args[0]), reg(args[1])
            w = (0x39 << 26) | (SF_REG[mnem] << 21) | (ra << 16) | (rb << 11)
        elif mnem in IMM_OP:
            if mnem == "movhi":
                rd, imm = reg(args[0]), parse_imm(args[1])
                ra = 0
            else:
                rd, ra, imm = reg(args[0]), reg(args[1]), parse_imm(args[2])
            w = (IMM_OP[mnem] << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF)
        elif mnem in SF_IMM:
            ra, imm = reg(args[0]), int(args[1], 0)
            w = (0x2F << 26) | (SF_IMM[mnem] << 21) | (ra << 16) | (imm & 0xFFFF)
        elif mnem in SHIFT_IMM:
            rd, ra, imm = reg(args[0]), reg(args[1]), int(args[2], 0)
            w = (0x2E << 26) | (rd << 21) | (ra << 16) | (SHIFT_IMM[mnem] << 6) | (imm & 0x1F)
        elif mnem in LOAD_OP:
            # lwz rD, imm(rA)
            rd = reg(args[0])
            m = re.match(r"(-?\w+)\((r\d+)\)$", args[1])
            imm, ra = int(m.group(1), 0), reg(m.group(2))
            w = (LOAD_OP[mnem] << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF)
        elif mnem in STORE_OP:
            # sw imm(rA), rB
            m = re.match(r"(-?\w+)\((r\d+)\)$", args[0])
            imm, ra = int(m.group(1), 0), reg(m.group(2))
            rb = reg(args[1])
            w = (STORE_OP[mnem] << 26) | (((imm >> 11) & 0x1F) << 21) | (ra << 16) | (rb << 11) | (imm & 0x7FF)
        elif mnem in ("jr", "jalr"):
            rb = reg(args[0])
            op = 0x11 if mnem == "jr" else 0x12
            w = (op << 26) | (rb << 11)
        elif mnem in BRANCH_OP:
            target = labels[args[0]]
            offset = target - addr
            w = (BRANCH_OP[mnem] << 26) | (offset & 0x3FFFFFF)
        else:
            raise ValueError(f"unknown mnemonic: {mnem!r}")

        words.append(w & 0xFFFFFFFF)
        addr += 1

    return words, labels


if __name__ == "__main__":
    src = open(sys.argv[1]).read() if len(sys.argv) > 1 else sys.stdin.read()
    words, labels = assemble(src)
    fmt = sys.argv[2] if len(sys.argv) > 2 else "c"
    if fmt == "c":
        for i, w in enumerate(words):
            print(f"    prog[{i}] = 0x{w:08X};")
        print(f"// {len(words)} words")
    elif fmt == "js":
        print("[" + ",".join(f"0x{w:08X}" for w in words) + "]")
    elif fmt == "hex":
        for w in words:
            print(f"{w:08X}")
    elif fmt == "bin":
        # big-endian word stream, matches main.c's load_program()
        out = sys.stdout.buffer
        for w in words:
            out.write(w.to_bytes(4, "big"))
    else:
        raise SystemExit(f"unknown format: {fmt!r}")
