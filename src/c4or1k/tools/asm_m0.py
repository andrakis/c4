#!/usr/bin/env python3
# Hand-assembler for M0's decrement-and-branch smoke test.
#
# c4lc has no assembler and jor1k's own instruction encodings were
# read directly from jor1k/js/worker/or1k/safecpu.js (opcode numbers,
# field positions, and the pc/nextpc delay-slot convention) rather
# than from the OR1000 spec, since M1 will cross-check main.c's
# decoder against that same file as an oracle -- the two need to
# agree on encoding, not just on the spec. Re-run this script and
# paste its output into main.c's prog_init() if N changes.

def enc3(op, rD, rA, rB, func=0):
    return ((op & 0x3F) << 26) | ((rD & 0x1F) << 21) | ((rA & 0x1F) << 16) | ((rB & 0x1F) << 11) | (func & 0x7FF)

def enc_i(op, rD, rA, imm):
    return ((op & 0x3F) << 26) | ((rD & 0x1F) << 21) | ((rA & 0x1F) << 16) | (imm & 0xFFFF)

def enc_branch(op, offset_words):
    return ((op & 0x3F) << 26) | (offset_words & 0x3FFFFFF)

def enc_sfeqi(rA, imm):
    # opcode 0x2F, sub-op field at bits 25:21 (0 == eq despite jor1k's
    # own "sfnei" comment on that case -- the code is r==imm).
    return (0x2F << 26) | (0 << 21) | ((rA & 0x1F) << 16) | (imm & 0xFFFF)

def build(n):
    hi = (n >> 16) & 0xFFFF
    lo = n & 0xFFFF
    prog = [
        ("l.ori r2,r0,1",     enc_i(0x2A, 2, 0, 1)),
        ("l.movhi r1,hi(N)",  enc_i(0x06, 1, 0, hi)),
        ("l.ori r1,r1,lo(N)", enc_i(0x2A, 1, 1, lo)),
        ("l.sfeqi r1,0",      enc_sfeqi(1, 0)),           # 3: loop:
        ("l.bf end",          enc_branch(0x4, 9 - 4)),
        ("l.nop",             (0x5 << 26)),                # delay slot
        ("l.sub r1,r1,r2",    enc3(0x38, 1, 1, 2, 0x2)),
        ("l.j loop",          enc_branch(0x0, 3 - 7)),
        ("l.nop",             (0x5 << 26)),                # delay slot
        ("l.nop (end)",       (0x5 << 26)),                # 9: end:
    ]
    return prog

if __name__ == "__main__":
    import sys
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 300000
    prog = build(n)
    print(f"// N = {n}")
    for i, (name, w) in enumerate(prog):
        print(f"    prog[{i}] = 0x{w & 0xFFFFFFFF:08X}; // {name}")
    print(f"// total dispatched instructions == {6 * (n + 1)}  "
          f"(setup(3) + N normal 6-instr iterations + 1 final 3-instr iteration, "
          f"see main.c's expect_steps)")
