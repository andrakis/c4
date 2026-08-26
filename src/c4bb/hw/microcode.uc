# microcode.uc - the c4bb CPU microcode.
#
# Semantics reference: the dispatch switch in c4m.c (line 1490 on).
# One line = one microstep = one bus transfer. Step counts follow the
# per-opcode decomposition worked out in docs/oisc4-design.md.
#
# Machine conventions (32-bit words, byte addresses):
#   - the ALU computes  left OP right;  left = B (or A with ALU_LA),
#     right = A (or T with ALU_RT, or MDR with ALU_RM). This mirrors
#     c4's "a = *sp++ OP a": stack value left, accumulator right.
#   - OPR_OUTX4 / T_OUTX4 are wired shifts (<<2): word counts become
#     byte offsets with zero chips.
#   - MAR_IN+n latches bus + 4n: a small offset adder on the MAR input
#     for indexed stack reads (syscall opcodes read args in place).
#   - a routine falling off its end means "instruction complete".

const UART_TX   0x100
const TIME_MS   0x10c
const USLP_US   0x118
const DISK_NAME 0x120
const DISK_FD   0x124
const DISK_ADDR 0x128
const DISK_LEN  0x12c
const DISK_CLOSE 0x130
const OPNAME    0x134
const DISK_FLAGS 0x138
const POWER     0x140
const INFO_REG  0x14c
const INTERVAL_REG 0x154
const TRESTORE_REG 0x15c
const MODE_REG  0x160
const TT_REG    0x164
const TP_REG    0x168
const HND_REG   0x16c
const JMODE_REG 0x170
const JINTERVAL_REG 0x174
const TLEV_ROM  0x10
const VEC_MALC  0x20
const VEC_FREE  0x24
const VEC_RALC  0x28
const VEC_PRTF  0x2c
const VEC_STRC  0x30
const OPNAMES_ROM 0x200

# ---- fetch: word at PC into IR, decode --------------------------------
routine fetch:
    PC_OUT MAR_IN
    MEM_RD MDR_OUT IR_IN PC_INC
    dispatch

# ---- core loads/stores/branches (c4 classic, 0-13) --------------------

op LEA operand:                 # a = bp + n words
    OPR_OUTX4 B_IN
    BP_OUT T_IN
    ALU=ADD ALU_RT ALU_OUT A_IN

op IMM operand:                 # a = n
    OPR_OUT A_IN

op JMP operand:
    OPR_OUT PC_IN

op JSR operand:                 # push return pc; pc = n
    SP_DEC
    SP_OUT MAR_IN
    PC_OUT MDR_IN MEM_WR
    OPR_OUT PC_IN

op BZ operand:
    br az .take
    goto fetch
.take:
    OPR_OUT PC_IN

op BNZ operand:
    br !az .take
    goto fetch
.take:
    OPR_OUT PC_IN

op ENT operand:                 # push bp; bp = sp; sp -= n words
    SP_DEC
    SP_OUT MAR_IN
    BP_OUT MDR_IN MEM_WR
    SP_OUT BP_IN
    SP_OUT B_IN
    OPR_OUTX4 T_IN
    ALU=SUB ALU_RT ALU_OUT SP_IN

op ADJ operand:                 # sp += n words
    SP_OUT B_IN
    OPR_OUTX4 T_IN
    ALU=ADD ALU_RT ALU_OUT SP_IN

op LEV:                         # sp = bp; bp = *sp++; pc = *sp++
    BP_OUT SP_IN
    SP_OUT MAR_IN
    MEM_RD MDR_OUT BP_IN SP_INC
    SP_OUT MAR_IN
    MEM_RD MDR_OUT PC_IN SP_INC

op LI:                          # a = *(int*)a
    A_OUT MAR_IN
    MEM_RD MDR_OUT A_IN

op LC:                          # a = *(char*)a  (signed byte)
    A_OUT MAR_IN
    MEM_RDB MDR_OUT A_IN

op SI:                          # *(int*)*sp++ = a
    SP_OUT MAR_IN
    MEM_RD MDR_OUT T_IN SP_INC
    T_OUT MAR_IN
    A_OUT MDR_IN MEM_WR

op SC:                          # a = *(char*)*sp++ = a (signed reload!)
    SP_OUT MAR_IN
    MEM_RD MDR_OUT T_IN SP_INC
    T_OUT MAR_IN
    A_OUT MDR_IN MEM_WRB
    MEM_RDB MDR_OUT A_IN

op PSH:
    SP_DEC
    SP_OUT MAR_IN
    A_OUT MDR_IN MEM_WR

# ---- ALU ops (14-29): a = *sp++ OP a ----------------------------------

op OR:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=OR ALU_OUT A_IN
op XOR:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=XOR ALU_OUT A_IN
op AND:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=AND ALU_OUT A_IN
op EQ:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=EQ ALU_OUT A_IN
op NE:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=NE ALU_OUT A_IN
op LT:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=LT ALU_OUT A_IN
op GT:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=GT ALU_OUT A_IN
op LE:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=LE ALU_OUT A_IN
op GE:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=GE ALU_OUT A_IN
op SHL:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=SHL ALU_OUT A_IN
op SHR:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=SHR ALU_OUT A_IN
op ADD:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=ADD ALU_OUT A_IN
op SUB:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=SUB ALU_OUT A_IN
op MUL:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=MUL ALU_OUT A_IN
op DIV:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=DIV ALU_OUT A_IN
op MOD:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=MOD ALU_OUT A_IN

# ---- extended control flow --------------------------------------------

op JMPA:                        # pc = a  (jumptable switch)
    A_OUT PC_IN

op _JMP:                        # pc = *sp++
    SP_OUT MAR_IN
    MEM_RD MDR_OUT PC_IN SP_INC

op _ADJ:                        # sp += *sp words
    SP_OUT MAR_IN
    MEM_RD MDR_OUT T_IN
    T_OUTX4 B_IN
    SP_OUT T_IN
    ALU=ADD ALU_RT ALU_OUT SP_IN

op JSRI operand:                # push pc; pc = *(*(operand))
    OPR_OUT MAR_IN
    MEM_RD MDR_OUT T_IN
    SP_DEC
    SP_OUT MAR_IN
    PC_OUT MDR_IN MEM_WR
    T_OUT PC_IN

op JSRS operand:                # push pc; pc = *(bp + operand words)
    OPR_OUTX4 B_IN
    BP_OUT T_IN
    ALU=ADD ALU_RT ALU_OUT MAR_IN
    MEM_RD MDR_OUT T_IN
    SP_DEC
    SP_OUT MAR_IN
    PC_OUT MDR_IN MEM_WR
    T_OUT PC_IN

# ---- disk controller --------------------------------------------------
# open/read/close backed by the disk device. fd 0 is the line-buffered
# keyboard; "/dev/stdin" opens a byte-oriented keyboard fd honoring
# O_NONBLOCK. A blocking read that would wait returns -2 and the
# microcode rewinds PC one word to retry the READ next instruction, so
# the cycle interrupt keeps firing while a task waits for input.

op OPEN:                        # a = open((char*)sp[1], *sp)
    SP_OUT MAR_IN
    MEM_RD MDR_OUT T_IN
    C=DISK_FLAGS MAR_IN
    T_OUT MDR_IN MEM_WR
    SP_OUT MAR_IN+1
    MEM_RD MDR_OUT T_IN
    C=DISK_NAME MAR_IN
    T_OUT MDR_IN MEM_WR
    MEM_RD MDR_OUT A_IN

op READ:                        # a = read(sp[2], (char*)sp[1], *sp)
    SP_OUT MAR_IN+2
    MEM_RD MDR_OUT T_IN
    C=DISK_FD MAR_IN
    T_OUT MDR_IN MEM_WR
    SP_OUT MAR_IN+1
    MEM_RD MDR_OUT T_IN
    C=DISK_ADDR MAR_IN
    T_OUT MDR_IN MEM_WR
    SP_OUT MAR_IN
    MEM_RD MDR_OUT T_IN
    C=DISK_LEN MAR_IN
    T_OUT MDR_IN MEM_WR
    MEM_RD MDR_OUT A_IN
    C=-2 B_IN
    ALU=NE ALU_OUT T_IN
    br !tz .done
    PC_OUT B_IN                 # would block: pc -= 4 and re-execute
    C=4 T_IN
    ALU=SUB ALU_RT ALU_OUT PC_IN
.done:

op CLOS:                        # a = close(*sp)
    SP_OUT MAR_IN
    MEM_RD MDR_OUT T_IN
    C=DISK_CLOSE MAR_IN
    T_OUT MDR_IN MEM_WR
    MEM_RD MDR_OUT A_IN

# ---- console and power ------------------------------------------------

op PUTC:                        # a = putchar(*(char*)sp) - unsigned ret
    SP_OUT MAR_IN
    MEM_RDBU MDR_OUT A_IN
    C=UART_TX MAR_IN
    A_OUT MDR_IN MEM_WR

op PUTS:                        # puts(*sp): bytes until NUL, then \n
    SP_OUT MAR_IN
    MEM_RD MDR_OUT U_IN
.loop:
    U_OUT MAR_IN
    MEM_RDBU br mz .done
    C=UART_TX MAR_IN
    MEM_WRB U_INC1 goto .loop
.done:
    C=10 MDR_IN
    C=UART_TX MAR_IN
    MEM_WR
    C=10 A_IN

op EXIT:                        # status = *sp; halt (POWER latch)
    SP_OUT MAR_IN
    MEM_RD MDR_OUT T_IN
    C=POWER MAR_IN
    T_OUT MDR_IN MEM_WR

# ---- memory block ops (visible microcoded loops) ----------------------

op MSET:                        # memset(sp[2], sp[1], *sp); a = dest
    SP_OUT MAR_IN
    MEM_RD MDR_OUT T_IN
    SP_OUT MAR_IN+1
    MEM_RD MDR_OUT B_IN
    SP_OUT MAR_IN+2
    MEM_RD MDR_OUT U_IN
    SP_OUT MAR_IN+2
    MEM_RD MDR_OUT A_IN
.loop:
    br tz .done
    U_OUT MAR_IN
    B_OUT MDR_IN MEM_WRB
    U_INC1 T_DEC1 goto .loop
.done:

op MCPY:                        # memcpy(sp[2], sp[1], *sp); a = dest
    SP_OUT MAR_IN
    MEM_RD MDR_OUT T_IN
    SP_OUT MAR_IN+1
    MEM_RD MDR_OUT B_IN
    SP_OUT MAR_IN+2
    MEM_RD MDR_OUT U_IN
    SP_OUT MAR_IN+2
    MEM_RD MDR_OUT A_IN
.loop:
    br tz .done
    B_OUT MAR_IN
    MEM_RDBU
    U_OUT MAR_IN
    MEM_WRB
    B_INC1 U_INC1 T_DEC1 goto .loop
.done:

op MCMP:                        # memcmp(sp[2], sp[1], *sp) - glibc
    SP_OUT MAR_IN               #   semantics: unsigned byte difference
    MEM_RD MDR_OUT T_IN
    SP_OUT MAR_IN+1
    MEM_RD MDR_OUT B_IN
    SP_OUT MAR_IN+2
    MEM_RD MDR_OUT U_IN
    C=0 A_IN
.loop:
    br tz .done
    U_OUT MAR_IN
    MEM_RDBU MDR_OUT A_IN
    B_OUT MAR_IN
    MEM_RDBU
    ALU=SUB ALU_LA ALU_RM ALU_OUT A_IN
    br !az .out
    U_INC1 B_INC1 T_DEC1 goto .loop
.done:
.out:

# ---- firmware vector calls (software syscalls, no traps involved) -----
# The synthesized JSR makes the opcode's in-place stack args look like
# ordinary function arguments to the firmware routine; its LEV returns
# to the instruction after the opcode, and the caller's ADJ cleans up.

op MALC:
    SP_DEC
    SP_OUT MAR_IN
    PC_OUT MDR_IN MEM_WR
    C=VEC_MALC MAR_IN
    MEM_RD MDR_OUT PC_IN

op FREE:
    SP_DEC
    SP_OUT MAR_IN
    PC_OUT MDR_IN MEM_WR
    C=VEC_FREE MAR_IN
    MEM_RD MDR_OUT PC_IN

op RALC:
    SP_DEC
    SP_OUT MAR_IN
    PC_OUT MDR_IN MEM_WR
    C=VEC_RALC MAR_IN
    MEM_RD MDR_OUT PC_IN

op PRTF:
    SP_DEC
    SP_OUT MAR_IN
    PC_OUT MDR_IN MEM_WR
    C=VEC_PRTF MAR_IN
    MEM_RD MDR_OUT PC_IN

op STRC:
    SP_DEC
    SP_OUT MAR_IN
    PC_OUT MDR_IN MEM_WR
    C=VEC_STRC MAR_IN
    MEM_RD MDR_OUT PC_IN

# ---- devices ----------------------------------------------------------

op C4CY:                        # a = cycle
    CYC_OUT A_IN

op TIME:                        # a = simulated milliseconds
    C=TIME_MS MAR_IN
    MEM_RD MDR_OUT A_IN

op USLP:                        # usleep(*sp); a = 0
    SP_OUT MAR_IN
    MEM_RD MDR_OUT T_IN
    C=USLP_US MAR_IN
    T_OUT MDR_IN MEM_WR
    C=0 A_IN

op INFO:                        # a = capability bits (INFO device reg)
    C=INFO_REG MAR_IN
    MEM_RD MDR_OUT A_IN

op _OPC:                        # a = __opcode(name): OPNAME device
    SP_OUT MAR_IN
    MEM_RD MDR_OUT T_IN
    C=OPNAME MAR_IN
    T_OUT MDR_IN MEM_WR
    MEM_RD MDR_OUT A_IN

op OPSL:                        # a = address of the opcode-name ROM
    C=OPNAMES_ROM A_IN

# ---- re-dispatch ------------------------------------------------------
# OPCD reads the requested opcode from the stack WITHOUT popping and
# re-dispatches; the guard for operand-carrying opcodes (<= ADJ) lives
# in the dispatcher, as in c4m.c:1535.

op OPCD:
    SP_OUT MAR_IN
    MEM_RD MDR_OUT IR_IN
    dispatch

# ---- the trap microroutine --------------------------------------------
# Entered by the trap jam (never by dispatch): the sequencer latches
# TT/TP/HND and forces the microprogram counter here. Implements
# c4m.c's trap() verbatim (c4m.c:1086-1157): synthesize a call frame
# on the interrupted stack, 15 words below the live top, whose slots
# are the handler's parameters; point the frame's return pc at the
# TLEV ROM word; mask the cycle interrupt; enter the handler past its
# ENT. The interrupted context rides entirely in this frame - a
# handler assigning to its parameters performs a context switch.

routine trap:
    SP_OUT U_IN                 # U = interrupted sp (the frame's sp slot)
    SP_OUT B_IN
    C=60 T_IN
    ALU=SUB ALU_RT ALU_OUT SP_IN  # sp -= TRAP_OFFSET (15 words)
    C=JINTERVAL_REG MAR_IN        # push interval as it was at the jam
    MEM_RD                        # (lands at bp+9) ...
    SP_DEC
    SP_OUT MAR_IN
    MEM_WR
    C=0 MDR_IN                    # ... and mask: handlers run with the
    C=INTERVAL_REG MAR_IN         # cycle interrupt off (c4m.c:1126)
    MEM_WR
    C=TT_REG MAR_IN               # push trap type (bp+8)
    MEM_RD
    SP_DEC
    SP_OUT MAR_IN
    MEM_WR
    C=TP_REG MAR_IN               # push parameter (bp+7)
    MEM_RD
    SP_DEC
    SP_OUT MAR_IN
    MEM_WR
    C=JMODE_REG MAR_IN            # push mode as it was at the jam (bp+6)
    MEM_RD
    SP_DEC
    SP_OUT MAR_IN
    MEM_WR
    SP_DEC                        # push a (bp+5)
    SP_OUT MAR_IN
    A_OUT MDR_IN MEM_WR
    SP_DEC                        # push bp (bp+4)
    SP_OUT MAR_IN
    BP_OUT MDR_IN MEM_WR
    SP_DEC                        # push interrupted sp (bp+3)
    SP_OUT MAR_IN
    U_OUT MDR_IN MEM_WR
    SP_DEC                        # push return pc (bp+2)
    SP_OUT MAR_IN
    PC_OUT MDR_IN MEM_WR
    SP_DEC                        # push &tlev_instruction (bp+1):
    SP_OUT MAR_IN                 # the handler's LEV returns THERE
    C=TLEV_ROM MDR_IN MEM_WR
    SP_DEC                        # bp-link slot (bp+0) points at itself
    SP_OUT MAR_IN
    SP_OUT MDR_IN MEM_WR
    SP_OUT BP_IN                  # bp = handler frame base
    C=HND_REG MAR_IN              # read handler address
    MEM_RD MDR_OUT T_IN
    T_OUT MAR_IN+1                # peek the handler's ENT n operand
    MEM_RD MDR_OUT OPR_IN         # OPR = n locals
    T_OUT PC_IN                   # pc = handler + 2 words (skip ENT n)
    PC_INC
    PC_INC
    SP_OUT B_IN                   # sp -= n words for the locals
    OPR_OUTX4 T_IN
    ALU=SUB ALU_RT ALU_OUT SP_IN

# ---- trap return ------------------------------------------------------
# The TLEV ROM word at 0x10 holds this opcode; every trap handler's
# LEV loads pc from bp+1 and lands on it. Restores pc/sp/bp/a/mode
# from the frame - which the handler may have rewritten - and, when
# CONF_TRAP_RESTORES_INTERVAL is on, re-arms the cycle interrupt from
# the bp+9 slot (c4m.c:1732-1759). The fetch logic refuses to
# interrupt when the NEXT instruction is TLEV, so this sequence is
# atomic by construction.

op TLEV:
    BP_OUT MAR_IN+2
    MEM_RD MDR_OUT PC_IN          # pc = saved return pc
    BP_OUT MAR_IN+5
    MEM_RD MDR_OUT A_IN           # a
    BP_OUT MAR_IN+6
    MEM_RD                        # mode -> MODE latch
    C=MODE_REG MAR_IN
    MEM_WR
    C=TRESTORE_REG MAR_IN
    MEM_RD br mz .norestore
    BP_OUT MAR_IN+9
    MEM_RD                        # saved interval -> INTERVAL latch
    C=INTERVAL_REG MAR_IN
    MEM_WR
.norestore:
    BP_OUT MAR_IN+3
    MEM_RD MDR_OUT T_IN           # T = saved sp
    BP_OUT MAR_IN+4
    MEM_RD MDR_OUT BP_IN          # bp = saved bp (last: reads were bp-relative)
    T_OUT SP_IN

# ---- system controller (semantics in machine.js execJsop) -------------

op ITH: jsop
op C4CF: jsop
op SIGH: jsop
op SIGI: jsop
op _TRP: jsop
op DBG: jsop
op C4IV: jsop
op FLT: jsop

# ---- fused (79-88): docs/fused-opcodes.md -----------------------------
#
# The board implements the same THREE c4m implements -- LDL, STL, POPA.
# They are what a stack-machine code generator needs to treat the frame
# as registers, and they cost no new circuitry: every transfer below is
# one the board already performs elsewhere. LDL is LEA's address sum
# with the result going to MAR instead of A; STL is that plus the write
# PSH already does; POPA is LEV's pop without the destination.
#
# The other seven (LDG PSHL PSHG LEAP IMMP LIP ADDL) are c4mp's by the
# c4m/c4mp split, and they are here too -- because on a board the split
# is about MICROSTEPS, not about which VM binary you run, and these
# seven need no new circuitry either. They are also where the compiler
# win lives: PSHL alone is 17.2% of what `c4sp -R` executes running
# c4lc, against LDL's 3.2% (docs/fused-opcodes.md). c4mp's OTHER
# extension -- CPUI..TRAW at 66-78 -- is a genuinely bigger job (more
# than one CPU on the board) and buys no speed, so it is not here.

op LDL operand:                 # a = *(bp+n)     was: LEA n; LI
    OPR_OUTX4 B_IN
    BP_OUT T_IN
    ALU=ADD ALU_RT ALU_OUT MAR_IN
    MEM_RD MDR_OUT A_IN

op STL operand:                 # *(bp+n) = a     was: LEA n; PSH; ...; SI
    OPR_OUTX4 B_IN
    BP_OUT T_IN
    ALU=ADD ALU_RT ALU_OUT MAR_IN
    A_OUT MDR_IN MEM_WR

op POPA:                        # a = *sp++       was: IMM 0; ADD
    SP_OUT MAR_IN
    MEM_RD MDR_OUT A_IN SP_INC

op LDG operand:                 # a = *(int*)n    was: IMM n; LI
    OPR_OUT MAR_IN
    MEM_RD MDR_OUT A_IN

op PSHL operand:                # a = *(bp+n); push    was: LEA n; LI; PSH
    OPR_OUTX4 B_IN
    BP_OUT T_IN
    ALU=ADD ALU_RT ALU_OUT MAR_IN
    MEM_RD MDR_OUT A_IN
    SP_DEC
    SP_OUT MAR_IN
    A_OUT MDR_IN MEM_WR

op PSHG operand:                # a = *(int*)n; push   was: IMM n; LI; PSH
    OPR_OUT MAR_IN
    MEM_RD MDR_OUT A_IN
    SP_DEC
    SP_OUT MAR_IN
    A_OUT MDR_IN MEM_WR

op LEAP operand:                # a = bp+n; push       was: LEA n; PSH
    OPR_OUTX4 B_IN
    BP_OUT T_IN
    ALU=ADD ALU_RT ALU_OUT A_IN
    SP_DEC
    SP_OUT MAR_IN
    A_OUT MDR_IN MEM_WR

op IMMP operand:                # a = n; push          was: IMM n; PSH
    OPR_OUT A_IN
    SP_DEC
    SP_OUT MAR_IN
    A_OUT MDR_IN MEM_WR

op LIP:                         # a = *(int*)a; push   was: LI; PSH
    A_OUT MAR_IN
    MEM_RD MDR_OUT A_IN
    SP_DEC
    SP_OUT MAR_IN
    A_OUT MDR_IN MEM_WR

op ADDL:                        # a = *(int*)(*sp++ + a)   was: ADD; LI
    SP_OUT MAR_IN
    MEM_RD MDR_OUT B_IN SP_INC
    ALU=ADD ALU_OUT MAR_IN
    MEM_RD MDR_OUT A_IN
