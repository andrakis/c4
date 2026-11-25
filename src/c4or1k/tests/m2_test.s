# c4or1k M2 cross-check program: SPR read/write, SR flag round-trip,
# EXCEPT_SYSCALL/EXCEPT_TRAP delivery + l.rfe return, and a real
# DTLB-miss-then-retry cycle (handler installs a TLB entry, l.rfe
# retries the faulting l.lwz, which then succeeds).
#
# Straight-line like tests/m1_test.s, but the three exception
# handlers below live at their REAL vector addresses (.org, see
# tools/asm.py) since that's where the CPU actually jumps on a fault
# -- not inline with the code that triggers them. The main program
# explicitly jumps to final_halt once done, so it never straight-line
# falls into the (mostly unreachable filler) address range between
# here and the handlers.
#
# Vectors used: EXCEPT_DTLBMISS=0x900 (word 0x240), EXCEPT_SYSCALL=
# 0xC00 (word 0x300), EXCEPT_TRAP=0xE00 (word 0x380) -- see cpu.h.

    ori   r30, r0, 0x1000   # scratch RAM base, same convention as m1_test.s

# ---- 1. general SPR read/write round-trip (SPR 200, unused by cpu.c) ----
    ori   r1, r0, 0x1234
    mtspr r0, r1, 200
    mfspr r2, r0, 200
    sw    0(r30), r2        # expect 0x1234

# ---- 2. SR flag round-trip via mtspr(SPR_SR=17) ----
# bit0=SM bit1=TEE bit9=F; SR_FO (bit15) is forced on by cpu_set_flags
# regardless of what's written, so the read-back always has it set.
    ori   r3, r0, 515       # 1 | 2 | 512
    mtspr r0, r3, 17
    mfspr r4, r0, 17
    sw    4(r30), r4        # expect 515 | 0x8000 = 33283

# back to a known, quiet SR before triggering anything: SM=1 only, F cleared.
    ori   r5, r0, 1
    mtspr r0, r5, 17

# ---- 3. EXCEPT_SYSCALL (l.sys): EPCR = trap addr + 4, plain rfe just continues ----
    sys
after_sys:
    mfspr r6, r0, 32        # SPR_EPCR_BASE
    mfspr r7, r0, 48        # SPR_EEAR_BASE
    mfspr r8, r0, 64        # SPR_ESR_BASE
    sw    8(r30), r6
    sw    12(r30), r7
    sw    16(r30), r8

# ---- 4. EXCEPT_TRAP (l.trap): EPCR = trap addr itself, handler must
# bump EPCR by 4 before rfe or it re-triggers forever ----
    trap
after_trap:
    mfspr r9, r0, 32
    sw    20(r30), r9

# ---- 5. DTLB miss -> handler installs a TLB entry -> l.rfe retries
# the SAME l.lwz, which then succeeds ----
    ori   r15, r0, 0xABCD
    sw    0x2000(r0), r15   # pre-store the value the retried load should read back (DME still off here)
    ori   r10, r0, 0x21     # SM=1, DME=1 (bit5)
    mtspr r0, r10, 17
    ori   r11, r0, 0x2000
    lwz   r12, 0(r11)       # faults here first time; DTLBMISS handler retries it
after_dtlbmiss:
    mfspr r13, r0, 48       # EEAR: should read back 0x2000 (the address that faulted)
    ori   r14, r0, 1
    mtspr r0, r14, 17       # SR: SM=1 only, DME off again -- before any further memory access
    sw    24(r30), r13
    sw    28(r30), r12      # the value the retried load actually read: should be 0xABCD

    j     final_halt
    nop                     # delay slot

.org 0x240                  # EXCEPT_DTLBMISS vector (word address)
dtlbmiss_handler:
    ori   r16, r0, 1        # tlmbr: valid bit set, tag=0 (matches 0x2000>>19 == 0)
    mtspr r0, r16, 0xA01    # group1[0x200|1] -- setindex=(0x2000>>13)&63=1; idx=(1<<11)|0x201
    ori   r17, r0, 0x2100   # tlbtr: physical base 0x2000, SRE (supervisor read enable, bit8) set
    mtspr r0, r17, 0xA81    # group1[0x280|1]; idx=(1<<11)|0x281
    rfe

.org 0x300                  # EXCEPT_SYSCALL vector
sys_handler:
    rfe

.org 0x380                  # EXCEPT_TRAP vector
trap_handler:
    mfspr r18, r0, 32       # SPR_EPCR_BASE
    addi  r18, r18, 4       # skip past the l.trap instruction itself
    mtspr r0, r18, 32
    rfe

final_halt:
    nop
