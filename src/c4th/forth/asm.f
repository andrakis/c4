\ c4th asm.f -- a C4 opcode assembler, in Forth.
\
\ The instruction set is the one in c4.c and c4m.c: one cell per
\ instruction, and the first eight (LEA..ADJ) plus JSRI and JSRS carry a
\ second cell holding their operand. Everything else is a bare cell. That
\ single rule -- `i <= ADJ` -- is the whole encoding.
\
\ Emitting words are named with a trailing comma, the Forth convention for
\ "compile this": IMM, ADD, LEV, and so on. The comma also keeps AND, OR,
\ XOR and LT from colliding with the Forth words of those names, which is
\ a happy accident of the convention rather than a workaround.
\
\ This is the piece B5's native backend emits through, so it is worth
\ having correct and readable before anything depends on it.

CREATE ASMBUF 65536 ALLOT
VARIABLE ASMP
: ASM-RESET  ASMBUF ASMP ! ;
ASM-RESET

: OP,   ( n -- )  ASMP @ !  1 CELLS ASMP +! ;
: OP2,  ( n op -- )  OP, OP, ;          \ opcode first, then its operand
: ASM-HERE ( -- a )  ASMP @ ;
: ASM-LEN  ( -- n )  ASMP @ ASMBUF - ;

\ -- opcode numbers, in the order c4.c declares them -------------------
 0 CONSTANT #LEA    1 CONSTANT #IMM    2 CONSTANT #JMP    3 CONSTANT #JSR
 4 CONSTANT #BZ     5 CONSTANT #BNZ    6 CONSTANT #ENT    7 CONSTANT #ADJ
 8 CONSTANT #LEV    9 CONSTANT #LI    10 CONSTANT #LC    11 CONSTANT #SI
12 CONSTANT #SC    13 CONSTANT #PSH   14 CONSTANT #OR    15 CONSTANT #XOR
16 CONSTANT #AND   17 CONSTANT #EQ    18 CONSTANT #NE    19 CONSTANT #LT
20 CONSTANT #GT    21 CONSTANT #LE    22 CONSTANT #GE    23 CONSTANT #SHL
24 CONSTANT #SHR   25 CONSTANT #ADD   26 CONSTANT #SUB   27 CONSTANT #MUL
28 CONSTANT #DIV   29 CONSTANT #MOD   30 CONSTANT #OPEN  31 CONSTANT #READ
32 CONSTANT #CLOS  33 CONSTANT #PRTF  34 CONSTANT #MALC  35 CONSTANT #FREE
36 CONSTANT #MSET  37 CONSTANT #MCMP  38 CONSTANT #EXIT  39 CONSTANT #PUTC
40 CONSTANT #PUTS  41 CONSTANT #RALC  42 CONSTANT #MCPY  43 CONSTANT #STRC
61 CONSTANT #JSRI  62 CONSTANT #JSRS  63 CONSTANT #JMPA  64 CONSTANT #TLEV
\ 66-78 belong to c4mp, and so do 79 up: the fused opcodes, see
\ docs/fused-opcodes.md. Nothing emits them unless NOPC is set in
\ native.f, so by default the assembler's output runs on plain c4 --
\ and with NOPC set it needs c4mp, not c4m.
79 CONSTANT #LDL   80 CONSTANT #LDG   81 CONSTANT #PSHL  82 CONSTANT #PSHG
83 CONSTANT #LEAP  84 CONSTANT #IMMP  85 CONSTANT #LIP   86 CONSTANT #ADDL
87 CONSTANT #STL   88 CONSTANT #POPA

\ -- the emitters ------------------------------------------------------
\ Operand-carrying first. LEA and ADJ count cells; IMM is a value; JMP,
\ JSR, BZ and BNZ take an absolute address.

: LEA,  ( n -- )  #LEA  OP2, ;
: IMM,  ( n -- )  #IMM  OP2, ;
: JMP,  ( a -- )  #JMP  OP2, ;
: JSR,  ( a -- )  #JSR  OP2, ;
: BZ,   ( a -- )  #BZ   OP2, ;
: BNZ,  ( a -- )  #BNZ  OP2, ;
: ENT,  ( n -- )  #ENT  OP2, ;
: ADJ,  ( n -- )  #ADJ  OP2, ;
: JSRI, ( a -- )  #JSRI OP2, ;
: JSRS, ( n -- )  #JSRS OP2, ;

: LEV,  #LEV OP, ;   : LI,   #LI  OP, ;   : LC,   #LC  OP, ;
: SI,   #SI  OP, ;   : SC,   #SC  OP, ;   : PSH,  #PSH OP, ;
: OR,   #OR  OP, ;   : XOR,  #XOR OP, ;   : AND,  #AND OP, ;
: EQ,   #EQ  OP, ;   : NE,   #NE  OP, ;   : LT,   #LT  OP, ;
: GT,   #GT  OP, ;   : LE,   #LE  OP, ;   : GE,   #GE  OP, ;
: SHL,  #SHL OP, ;   : SHR,  #SHR OP, ;   : ADD,  #ADD OP, ;
: SUB,  #SUB OP, ;   : MUL,  #MUL OP, ;   : DIV,  #DIV OP, ;
: MOD,  #MOD OP, ;   : PRTF, #PRTF OP, ;  : EXIT, #EXIT OP, ;
: PUTC, #PUTC OP, ;  : JMPA, #JMPA OP, ;

: LDL,  ( n -- )  #LDL  OP2, ;   : LDG,  ( n -- )  #LDG  OP2, ;
: PSHL, ( n -- )  #PSHL OP2, ;   : PSHG, ( n -- )  #PSHG OP2, ;
: LEAP, ( n -- )  #LEAP OP2, ;   : IMMP, ( n -- )  #IMMP OP2, ;
: STL,  ( n -- )  #STL  OP2, ;
: LIP,  #LIP  OP, ;   : ADDL, #ADDL OP, ;   : POPA, #POPA OP, ;

\ -- forward references ------------------------------------------------
\ A branch whose target is not known yet is emitted with a zero operand
\ and patched later. >MARK leaves the operand's address; >RESOLVE fills it
\ in with wherever the assembler has since reached.

: >MARK    ( -- a )  ASM-HERE 1 CELLS -  ;   \ after a BZ, / JMP, etc
: >RESOLVE ( a -- )  ASM-HERE SWAP ! ;

\ -- disassembly -------------------------------------------------------
\ Four characters per mnemonic, indexed by opcode -- the same shape as
\ c4.c's own name string, which is what the VM's own debug output uses.

S" LEA IMM JMP JSR BZ  BNZ ENT ADJ LEV LI  LC  SI  SC  PSH OR  XOR AND EQ  NE  LT  GT  LE  GE  SHL SHR ADD SUB MUL DIV MOD OPENREADCLOSPRTFMALCFREEMSETMCMPEXITPUTCPUTSRALCMCPYSTRCITH _OPC_BLT_TRPOPCD_JMP_ADJC4CFC4CYTIMESIGHSIGIUSLPINFOOPSLC4IVFLT JSRIJSRSJMPATLEVDBG CPUICPUNCPUSCPUHCAS XCHGFADDCWAICWAKIPI LXI SXI TRAWLDL LDG PSHLPSHGLEAPIMMPLIP ADDLSTL POPA" DROP CONSTANT OPNAMES

: .OPNAME ( n -- )  4 * OPNAMES +  4 OVER + SWAP
                    BEGIN 2DUP > WHILE DUP C@ EMIT 1+ REPEAT 2DROP ;

\ One place decides which opcodes carry an operand word -- the same rule
\ c4m_has_operand states in c4m.c. LIP, ADDL and POPA take none.
: HAS-OPERAND? ( op -- f )
   DUP #ADJ <=  OVER #JSRI = OR  OVER #JSRS = OR
   OVER DUP #LDL >= SWAP #IMMP <= AND OR
   SWAP #STL = OR ;

\ Addresses inside the emitted code are printed as * rather than as
\ numbers: they depend on where the buffer happened to land, and the
\ point of a disassembly listing is the instruction sequence.
: .OPERAND ( op n -- )  SWAP DUP #JMP >= SWAP #BNZ <= AND
                        IF DROP ." *" ELSE . THEN ;

: DIS ( -- )
   ASMBUF
   BEGIN DUP ASMP @ < WHILE          ( a )
      DUP @                          ( a op )
      DUP .OPNAME                    ( a op )
      DUP HAS-OPERAND? IF            ( a op )
         SPACE  OVER 1 CELLS + @     ( a op operand )
         .OPERAND                    ( a )
         2 CELLS +
      ELSE
         DROP                        ( a )
         1 CELLS +
      THEN
      CR
   REPEAT DROP ;
