# c4or1k M1 cross-check program: exercises every instruction form
# cpu.c implements at least once. Straight-line (falls off the end,
# no backward loop) so main.c can stop at word-count and both
# main.c and tools/or1k-oracle.js can dump identical, diffable state:
# all 32 registers, SR_F/SR_CY/SR_OV, and a fixed RAM window at
# 0x1000 that each test result is written into (rather than trying to
# keep ~40 distinct results alive in only 31 usable registers).
#
# See tools/asm.py for the mnemonics/encodings and cpu.h for why every
# signed field goes through sext() rather than the usual C
# shift-truncate idiom.

    movhi r1, 0x1234
    ori   r1, r1, 0x5678   # r1 = 0x12345678
    ori   r2, r0, 10       # r2 = 10
    addi  r3, r0, -7       # r3 = -7 (sign-extended immediate)
    ori   r4, r0, 3        # r4 = 3  (shift count)
    ori   r30, r0, 0x1000  # r30 = scratch RAM base

# ---- three-operand ALU (opcode 0x38) ----
    add   r20, r1, r2
    sw    0(r30), r20
    sub   r20, r1, r2
    sw    4(r30), r20
    and   r20, r1, r2
    sw    8(r30), r20
    or    r20, r1, r2
    sw    12(r30), r20
    xor   r20, r1, r2
    sw    16(r30), r20
    sll   r20, r2, r4
    sw    20(r30), r20
    srl   r20, r1, r4
    sw    24(r30), r20
    sra   r20, r3, r4
    sw    28(r30), r20
    ff1   r20, r1, r1
    sw    32(r30), r20
    fl1   r20, r1, r1
    sw    36(r30), r20
    mul   r20, r2, r3
    sw    40(r30), r20
    div   r20, r1, r4
    sw    44(r30), r20
    divu  r20, r1, r4
    sw    48(r30), r20

# ---- immediate ALU ----
    addi  r20, r1, 100
    sw    52(r30), r20
    andi  r20, r1, 0xFF
    sw    56(r30), r20
    ori   r20, r1, 0xFF00
    sw    60(r30), r20
    xori  r20, r1, 0xFFFF
    sw    64(r30), r20

# ---- shift-immediate (opcode 0x2E) ----
    slli  r20, r1, 4
    sw    68(r30), r20
    srli  r20, r1, 4
    sw    72(r30), r20
    srai  r20, r3, 2
    sw    76(r30), r20

# ---- register compares (opcode 0x39), result materialized via bf ----
# l.bf/l.bnf/l.j/l.jal/l.jr/l.jalr all have ONE mandatory delay slot:
# the instruction immediately after them always executes, whether or
# not a conditional branch is actually taken. Every branch/jump below
# is followed by an explicit nop for exactly that reason -- the first
# draft of this file omitted them, and every compare read back as
# "true" because the delay slot silently ran the true-case's `ori`
# unconditionally. Real bug, found by the oracle diff; see
# docs/c4or1k-design.md.
    sfeq  r1, r2
    bf    c0t
    nop                    # delay slot of bf (always runs, whether or not taken)
    ori   r20, r0, 0
    j     c0d
    nop                    # delay slot of j (always runs)
c0t:
    ori   r20, r0, 1
c0d:
    sw    80(r30), r20

    sfne  r1, r2
    bf    c1t
    nop
    ori   r20, r0, 0
    j     c1d
    nop
c1t:
    ori   r20, r0, 1
c1d:
    sw    84(r30), r20

    sfgtu r1, r3
    bf    c2t
    nop
    ori   r20, r0, 0
    j     c2d
    nop
c2t:
    ori   r20, r0, 1
c2d:
    sw    88(r30), r20

    sfltu r1, r3
    bf    c3t
    nop
    ori   r20, r0, 0
    j     c3d
    nop
c3t:
    ori   r20, r0, 1
c3d:
    sw    92(r30), r20

    sfgts r1, r3
    bf    c4t
    nop
    ori   r20, r0, 0
    j     c4d
    nop
c4t:
    ori   r20, r0, 1
c4d:
    sw    96(r30), r20

    sflts r1, r3
    bf    c5t
    nop
    ori   r20, r0, 0
    j     c5d
    nop
c5t:
    ori   r20, r0, 1
c5d:
    sw    100(r30), r20

# ---- immediate compares (opcode 0x2F) ----
    sfeqi r2, 10
    bf    c6t
    nop
    ori   r20, r0, 0
    j     c6d
    nop
c6t:
    ori   r20, r0, 1
c6d:
    sw    104(r30), r20

    sfnei r2, 10
    bf    c7t
    nop
    ori   r20, r0, 0
    j     c7d
    nop
c7t:
    ori   r20, r0, 1
c7d:
    sw    108(r30), r20

    sfgtui r3, 5
    bf    c8t
    nop
    ori   r20, r0, 0
    j     c8d
    nop
c8t:
    ori   r20, r0, 1
c8d:
    sw    112(r30), r20

    sfltsi r3, 0
    bf    c9t
    nop
    ori   r20, r0, 0
    j     c9d
    nop
c9t:
    ori   r20, r0, 1
c9d:
    sw    116(r30), r20

# ---- loads/stores: byte/half/word round trips ----
    addi  r6, r0, -1       # r6 = 0xFFFFFFFF
    sb    200(r30), r6
    lbz   r20, 200(r30)    # zero-extend: 0x000000FF
    sw    204(r30), r20
    lbs   r20, 200(r30)    # sign-extend: 0xFFFFFFFF
    sw    208(r30), r20
    sh    212(r30), r6
    lhz   r20, 212(r30)    # zero-extend: 0x0000FFFF
    sw    216(r30), r20
    lhs   r20, 212(r30)    # sign-extend: 0xFFFFFFFF
    sw    220(r30), r20
    sw    224(r30), r6
    lwz   r20, 224(r30)    # 0xFFFFFFFF
    sw    228(r30), r20

# ---- lwa/swa: reservation must match to store ----
    addi  r7, r0, 42
    lwa   r20, 224(r30)    # sets EA; r20 = 0xFFFFFFFF
    swa   224(r30), r7     # EA matches -> SR_F=1, mem[224]=42
    lwz   r20, 224(r30)    # 42
    sw    232(r30), r20

# ---- l.jal / l.jr: call/return, r9 is the link register ----
    jal   sub1
    nop                    # delay slot
    add   r21, r9, r0      # save jal's return-link (executes after sub1 returns here)
    sw    240(r30), r21
    j     after_jal
    nop                    # delay slot -- otherwise sub1's first instruction
                            # below runs an extra, unintended time here
sub1:
    addi  r6, r0, 0x55
    sw    244(r30), r6     # proves sub1 actually ran
    jr    r9
    nop                    # delay slot
after_jal:

# ---- l.jalr: jump through a register holding a computed target ----
    movhi r8, %hi(sub2)
    ori   r8, r8, %lo(sub2)
    jalr  r8
    nop                    # delay slot
    add   r22, r9, r0
    sw    248(r30), r22
    j     end
    nop                    # delay slot
sub2:
    addi  r6, r0, 0x66
    sw    252(r30), r6
    jr    r9
    nop
end:
    nop
