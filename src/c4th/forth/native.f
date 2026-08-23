\ c4th native.f -- B5: compile a colon definition's threaded body into
\ real C4 instructions.
\
\ Strategy (b), chosen by the probe in src/c4th/bench: the C4 stack IS
\ the Forth data stack and the top of stack is cached in the accumulator.
\ That is not a trick -- C4's instruction set is already shaped this way.
\ Every ALU opcode is `a = *sp++ OP a`, which is exactly a binary Forth
\ word: + is one ADD, @ is one LI, DUP is one PSH.
\
\ The model is two pieces of state. NDEPTH is how many values are live,
\ and NACC says whether the topmost of them is in the accumulator (the
\ rest are always on the C4 stack). SPILL pushes the accumulator out to
\ the stack; NEED-ACC brings the top back in, which costs an IMM 0 / ADD
\ because C4 has no plain pop.
\
\ Words that reorder the stack -- SWAP, OVER -- are the expensive ones,
\ since one accumulator cannot hold two values. They go through scratch
\ cells. Words that do not reorder are one instruction each, and that is
\ where the 14x in the probe lives.

CREATE NSCR 4 CELLS ALLOT
VARIABLE NDEPTH
VARIABLE NACC
VARIABLE NOK                       \ cleared when something is unsupported
VARIABLE NBAD                      \ the xt that stopped it, for surveying

1024 CONSTANT NMAX
CREATE NMAP  NMAX CELLS ALLOT      \ threaded cell offset -> native address
CREATE NFIXA NMAX CELLS ALLOT      \ operand cell needing a target
CREATE NFIXT NMAX CELLS ALLOT      \ the threaded offset it should point at
VARIABLE NFIXN
VARIABLE NBASE                     \ threaded body start
CREATE NTGT NMAX CELLS ALLOT       \ non-zero if a threaded offset is branched to

: SPILL     NACC @ IF PSH, 0 NACC ! THEN ;
: NEED-ACC  NACC @ 0= IF 0 IMM, ADD, 1 NACC ! THEN ;

\ Forth wants all-bits-set for true; C4's comparisons yield 1. Multiplying
\ by -1 is the cheapest exact conversion: three instructions, and it is
\ correct for the 0/1 the VM actually produces.
: TOFLAG    PSH, -1 IMM, MUL, ;

\ -- what this backend does NOT compile, and why ----------------------
\
\ SWAP, OVER, ROT and ! are left threaded. This is not an oversight and
\ not laziness about typing them out: C4's store is
\ `*(int *)*sp++ = a`, so the DESTINATION has to be pushed before the
\ value is computed -- and here the value is already sitting in the
\ accumulator, which loading the address would destroy. There is one
\ register, so there is nowhere to put it first.
\
\ Done properly, SWAP through the frame costs about seventeen
\ instructions, which is worse than the twenty-odd cycles the threaded
\ inner interpreter charges for it. So compiling it badly would make
\ code slower, not faster.
\
\ The real answer is a deferred-operand model: keep the top few stack
\ items as compile-time descriptions -- this one is a literal, that one
\ is a fetch from an address -- and only emit code when something forces
\ them into existence. Then `1 2 SWAP -` emits IMM 2, PSH, IMM 1, SUB
\ and SWAP costs nothing, and `x addr !` can emit the address push
\ first because the compiler, not the machine, decides the order. That
\ is the rest of B5 and it is a real piece of work; it is not something
\ to bolt on.
\
\ Until then NCOMPILE reports failure for any word it cannot do well,
\ and the caller keeps the threaded definition. Refusing is the honest
\ behaviour: a native backend that silently emits worse code than the
\ interpreter is worse than no backend.

\ -- the words this backend knows -------------------------------------
' + CONSTANT n+   ' - CONSTANT n-   ' * CONSTANT n*   ' / CONSTANT n/
' MOD CONSTANT nMOD  ' AND CONSTANT nAND  ' OR CONSTANT nOR
' XOR CONSTANT nXOR
' = CONSTANT n=  ' <> CONSTANT n<>  ' < CONSTANT n<  ' > CONSTANT n>
' <= CONSTANT n<=  ' >= CONSTANT n>=
' DUP CONSTANT nDUP  ' DROP CONSTANT nDROP  ' SWAP CONSTANT nSWAP
' OVER CONSTANT nOVER  ' @ CONSTANT n@  ' C@ CONSTANT nC@
' 1+ CONSTANT n1+  ' 1- CONSTANT n1-
' LIT CONSTANT nLIT  ' BRANCH CONSTANT nBRANCH  ' 0BRANCH CONSTANT n0BRANCH
' EXIT CONSTANT nEXIT

: BIN, ( opcode -- )  NEED-ACC OP, -1 NDEPTH +! ;
: CMP, ( opcode -- )  NEED-ACC OP, TOFLAG -1 NDEPTH +! ;

\ Record that the operand cell just emitted must end up pointing at the
\ native address of threaded offset t.
: FIX! ( t -- )
   NFIXN @ NMAX < IF
      ASM-HERE 1 CELLS -  NFIXN @ CELLS NFIXA + !
      NFIXN @ CELLS NFIXT + !
      1 NFIXN +!
   ELSE DROP 0 NOK ! THEN ;

: SRCOFF ( a -- n )  NBASE @ - 1 CELLS / ;

: NEMIT ( a xt -- a' )        \ emit one threaded instruction
   DUP nLIT = IF DROP SPILL DUP 1 CELLS + @ IMM, 1 NACC ! 1 NDEPTH +! 2 CELLS + EXIT THEN
   DUP n+ = IF DROP #ADD BIN, 1 CELLS + EXIT THEN
   DUP n- = IF DROP #SUB BIN, 1 CELLS + EXIT THEN
   DUP n* = IF DROP #MUL BIN, 1 CELLS + EXIT THEN
   DUP n/ = IF DROP #DIV BIN, 1 CELLS + EXIT THEN
   DUP nMOD = IF DROP #MOD BIN, 1 CELLS + EXIT THEN
   DUP nAND = IF DROP #AND BIN, 1 CELLS + EXIT THEN
   DUP nOR  = IF DROP #OR  BIN, 1 CELLS + EXIT THEN
   DUP nXOR = IF DROP #XOR BIN, 1 CELLS + EXIT THEN
   DUP n=  = IF DROP #EQ CMP, 1 CELLS + EXIT THEN
   DUP n<> = IF DROP #NE CMP, 1 CELLS + EXIT THEN
   DUP n<  = IF DROP #LT CMP, 1 CELLS + EXIT THEN
   DUP n>  = IF DROP #GT CMP, 1 CELLS + EXIT THEN
   DUP n<= = IF DROP #LE CMP, 1 CELLS + EXIT THEN
   DUP n>= = IF DROP #GE CMP, 1 CELLS + EXIT THEN
   DUP n1+ = IF DROP NEED-ACC PSH, 1 IMM, ADD, 1 CELLS + EXIT THEN
   DUP n1- = IF DROP NEED-ACC PSH, 1 IMM, SUB, 1 CELLS + EXIT THEN
   DUP n@  = IF DROP NEED-ACC LI, 1 CELLS + EXIT THEN
   DUP nC@ = IF DROP NEED-ACC LC, 1 CELLS + EXIT THEN
   DUP nDUP = IF DROP NEED-ACC PSH, 1 NDEPTH +! 1 CELLS + EXIT THEN
   DUP nDROP = IF DROP NACC @ IF 0 NACC ! ELSE 1 ADJ, THEN
                    -1 NDEPTH +! 1 CELLS + EXIT THEN
   \ Branches canonicalise: everything on the stack, accumulator free, so
   \ that the state at a target does not depend on which way it was
   \ reached. 0BRANCH needs its flag in the accumulator to test.
   DUP n0BRANCH = IF DROP
      NEED-ACC 0 BZ, DUP 1 CELLS + @ SRCOFF FIX!
      -1 NDEPTH +! 0 NACC !
      2 CELLS + EXIT THEN
   DUP nBRANCH = IF DROP
      SPILL 0 JMP, DUP 1 CELLS + @ SRCOFF FIX!
      2 CELLS + EXIT THEN
   DUP nEXIT = IF DROP NEED-ACC LEV, 1 CELLS + EXIT THEN
   \ Anything else: this backend does not know it. Note the DUP above --
   \ without it this arm consumes the xt and the fallback below then
   \ operates on the ADDRESS instead, quietly eating a loop variable
   \ rather than underflowing. That shape reports "declined" for the
   \ wrong reason and looks fine.
   NBAD ! 0 NOK ! 1 CELLS + ;

: NCOMPILE ( body end -- ok? )
   OVER NBASE !  ASM-RESET
   0 NDEPTH !  0 NACC !  0 NFIXN !  1 NOK !  0 NBAD !
   NMAX 0 DO 0 NTGT I CELLS + ! LOOP
   \ Find the branch targets first. At a target the accumulator must be
   \ free, because the state there cannot depend on which way it was
   \ reached -- the branching paths all arrive canonical, and the
   \ fall-through path has to be made to match.
   OVER                                  ( body end a )
   BEGIN 2DUP > WHILE
      DUP @ DUP n0BRANCH = SWAP nBRANCH = OR IF
         DUP 1 CELLS + @ NBASE @ - 1 CELLS /
         DUP 0 >= OVER NMAX < AND IF 1 SWAP NTGT SWAP CELLS + ! ELSE DROP THEN
      THEN
      DUP @ DUP nLIT = SWAP DUP n0BRANCH = SWAP nBRANCH = OR OR
      IF 2 CELLS + ELSE 1 CELLS + THEN
   REPEAT DROP                           ( body end )
   OVER                                    ( body end a )
   0 ENT,                                  \ no locals: the data stack is the C4 stack
   BEGIN 2DUP > NOK @ AND WHILE
      DUP SRCOFF CELLS NTGT + @ IF SPILL THEN
      DUP SRCOFF CELLS NMAP + ASM-HERE SWAP !
      DUP DUP @ NEMIT NIP
   REPEAT DROP 2DROP
   \ resolve the branches
   NOK @ IF
      NFIXN @ 0 ?DO
         I CELLS NFIXT + @ CELLS NMAP + @      ( native-target )
         I CELLS NFIXA + @ !
      LOOP
   THEN
   NOK @ ;
