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

\ -- items, regions, and code motion -----------------------------------
\
\ The problem SWAP poses is not shuffling a stack, it is that C4 has one
\ register: with x2 in the accumulator and x1 beneath it there is nowhere
\ to put x2 while x1 is fetched. Through memory it costs about seventeen
\ instructions, more than the interpreter charges, so compiling it that
\ way would make code slower.
\
\ The way out is to stop treating emitted code as fixed. Every item on the
\ compile-time stack remembers where its code begins, so the compiler can
\ still move it, and SWAP becomes a rotation of the output buffer that
\ costs nothing at runtime:
\
\     [ codeA ][ PSH ][ codeB ]   ->   [ codeB ][ PSH ][ codeA ]
\
\ That is sound because each region is balanced -- it computes one value
\ into the accumulator and leaves the stack as it found it -- and because
\ no region ever spans a branch: branches flush the model first.
\
\ And ! falls out of it. Forth writes the value before the address while
\ C4's SI wants the address pushed first, so with SWAP free, ! is SWAP
\ then SI.

64 CONSTANT NITEMS
CREATE ISTART NITEMS CELLS ALLOT    \ where item i's code begins, as an offset
CREATE ISCRATCH 1024 CELLS ALLOT

VARIABLE SAOFF  VARIABLE SBOFF  VARIABLE SLENA  VARIABLE SLENB

: IOFF  ( i -- off )   CELLS ISTART + @ ;
: IOFF! ( off i -- )   CELLS ISTART + ! ;
: ASM@  ( off -- a )   ASMBUF + ;

\ Start a new item. Whatever is in the accumulator belongs to the item
\ below, so spill it, then record where this one begins.
: NEWITEM ( -- )
   SPILL
   NDEPTH @ NITEMS < IF ASM-LEN NDEPTH @ IOFF! ELSE 0 NOK ! THEN
   1 NDEPTH +! ;

\ After a branch nothing may move across it, so every live item is marked
\ as having no movable region.
: NFLUSH ( -- )
   NDEPTH @ 0 ?DO ASM-LEN I IOFF! LOOP ;

\ Rotate the top two regions. Region A runs from item n-2's start to item
\ n-1's start and ends with the PSH that spilled it; region B is the rest.
\ Declines on an empty region -- after DUP the top item has no code of its
\ own, and rotating it would move a PSH in front of the code that fills
\ the accumulator it pushes.
: N-SWAP ( -- ok? )
   NDEPTH @ 2 < IF 0 EXIT THEN
   NEED-ACC
   NDEPTH @ 2 - IOFF SAOFF !
   NDEPTH @ 1- IOFF SBOFF !
   SBOFF @ SAOFF @ - 1 CELLS - SLENA !
   ASM-LEN SBOFF @ -           SLENB !
   SLENA @ 0 <= IF 0 EXIT THEN
   SLENB @ 0 <= IF 0 EXIT THEN
   SLENA @ 1024 CELLS > IF 0 EXIT THEN
   SAOFF @ ASM@  ISCRATCH  SLENA @ MOVE                  \ stash codeA
   SBOFF @ ASM@  SAOFF @ ASM@  SLENB @ MOVE              \ codeB to the front
   #PSH  SAOFF @ SLENB @ + ASM@ !                        \ the spill, after it
   ISCRATCH  SAOFF @ SLENB @ + 1 CELLS + ASM@  SLENA @ MOVE
   SAOFF @ SLENB @ + 1 CELLS +  NDEPTH @ 1- IOFF!
   1 ;

\ OVER copies the item below the top. That is only safe when its region
\ can simply be run again, so it is allowed for a bare IMM -- a literal or
\ a variable's address, which is the case that actually occurs -- and
\ declined otherwise rather than guessed at.
: N-OVER ( -- ok? )
   NDEPTH @ 2 < IF 0 EXIT THEN
   NEED-ACC
   NDEPTH @ 2 - IOFF SAOFF !
   NDEPTH @ 1- IOFF SBOFF !
   SBOFF @ SAOFF @ - 1 CELLS - SLENA !
   SLENA @ 2 CELLS <> IF 0 EXIT THEN
   SAOFF @ ASM@ @ #IMM <> IF 0 EXIT THEN
   SAOFF @ 1 CELLS + ASM@ @
   NEWITEM IMM,  1 NACC !
   1 ;

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
' ! CONSTANT n!  ' C! CONSTANT nC!  ' 0= CONSTANT n0=
' 0<> CONSTANT n0<>  ' 0< CONSTANT n0<  ' 0> CONSTANT n0>
' NEGATE CONSTANT nNEG  ' INVERT CONSTANT nINV  ' NIP CONSTANT nNIP
' 2* CONSTANT n2*  ' CELLS CONSTANT nCELLS  ' CELL+ CONSTANT nCELL+
' SWAP CONSTANT nSWAP  ' OVER CONSTANT nOVER

\ A CREATEd word -- every VARIABLE, every CONSTANT's underlying store --
\ just pushes the address of its body. That is a literal, so it compiles
\ to one IMM and needs nothing else. Finding out whether an xt is one
\ means comparing its code field against a known example's.
CREATE NVPROBE
' NVPROBE 5 CELLS + @ CONSTANT NDOVAR
: >WCODE ( xt -- a )  5 CELLS + @ ;
: >WBODY ( xt -- a )  9 CELLS + ;

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
   DUP nLIT = IF DROP NEWITEM DUP 1 CELLS + @ IMM, 1 NACC ! 2 CELLS + EXIT THEN
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
   DUP nDUP = IF DROP NEED-ACC PSH, ASM-LEN NDEPTH @ IOFF! 1 NDEPTH +! 1 CELLS + EXIT THEN
   DUP nSWAP = IF DROP N-SWAP 0= IF 0 NOK ! THEN 1 CELLS + EXIT THEN
   DUP nOVER = IF DROP N-OVER 0= IF 0 NOK ! THEN 1 CELLS + EXIT THEN
   \ ! is SWAP then SI: the rotation puts the address on the stack and
   \ leaves the value in the accumulator, which is exactly what SI wants.
   DUP n!  = IF DROP N-SWAP 0= IF 0 NOK ! 1 CELLS + EXIT THEN
                   SI, -2 NDEPTH +! 0 NACC ! 1 CELLS + EXIT THEN
   DUP nC! = IF DROP N-SWAP 0= IF 0 NOK ! 1 CELLS + EXIT THEN
                   SC, -2 NDEPTH +! 0 NACC ! 1 CELLS + EXIT THEN
   DUP nNIP = IF DROP NEED-ACC 1 ADJ, -1 NDEPTH +! 1 CELLS + EXIT THEN
   DUP n0= = IF DROP NEED-ACC PSH, 0 IMM, EQ, TOFLAG 1 CELLS + EXIT THEN
   DUP n0<> = IF DROP NEED-ACC PSH, 0 IMM, NE, TOFLAG 1 CELLS + EXIT THEN
   DUP n0< = IF DROP NEED-ACC PSH, 0 IMM, LT, TOFLAG 1 CELLS + EXIT THEN
   DUP n0> = IF DROP NEED-ACC PSH, 0 IMM, GT, TOFLAG 1 CELLS + EXIT THEN
   DUP nNEG = IF DROP NEED-ACC PSH, -1 IMM, MUL, 1 CELLS + EXIT THEN
   DUP nINV = IF DROP NEED-ACC PSH, -1 IMM, XOR, 1 CELLS + EXIT THEN
   DUP n2* = IF DROP NEED-ACC PSH, 2 IMM, MUL, 1 CELLS + EXIT THEN
   DUP nCELLS = IF DROP NEED-ACC PSH, 1 CELLS IMM, MUL, 1 CELLS + EXIT THEN
   DUP nCELL+ = IF DROP NEED-ACC PSH, 1 CELLS IMM, ADD, 1 CELLS + EXIT THEN
   DUP nDROP = IF DROP NACC @ IF 0 NACC ! ELSE 1 ADJ, THEN
                    -1 NDEPTH +! 1 CELLS + EXIT THEN
   \ Branches canonicalise: everything on the stack, accumulator free, so
   \ that the state at a target does not depend on which way it was
   \ reached. 0BRANCH needs its flag in the accumulator to test.
   DUP n0BRANCH = IF DROP
      NEED-ACC 0 BZ, DUP 1 CELLS + @ SRCOFF FIX!
      -1 NDEPTH +! 0 NACC ! NFLUSH
      2 CELLS + EXIT THEN
   DUP nBRANCH = IF DROP
      SPILL 0 JMP, DUP 1 CELLS + @ SRCOFF FIX! NFLUSH
      2 CELLS + EXIT THEN
   DUP nEXIT = IF DROP NEED-ACC LEV, 1 CELLS + EXIT THEN
   \ CREATEd word: push its body address, which is a compile-time
   \ constant, so this is one IMM and the deferred model can move it.
   DUP >WCODE NDOVAR = IF >WBODY NEWITEM IMM, 1 NACC ! 1 CELLS + EXIT THEN
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
      DUP SRCOFF CELLS NTGT + @ IF SPILL NFLUSH THEN
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
