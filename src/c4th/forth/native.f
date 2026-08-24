\ c4th native.f -- the native backend: compile a colon definition's
\ threaded body into real C4 instructions.
\
\ Strategy (b), chosen by the probe in src/c4th/bench: the C4 stack IS
\ the Forth data stack and the top of stack is cached in the accumulator.
\ That is not a trick -- C4's instruction set is already shaped this way.
\ Every ALU opcode is `a = *sp++ OP a`, which is exactly a binary Forth
\ word: + is one ADD, @ is one LI, DUP is one PSH.
\
\ Three mechanisms carry the whole backend, and they compose:
\
\   1. THE ACCUMULATOR MODEL.  NDEPTH is how many values are live and
\      NACC says whether the topmost is in the accumulator rather than on
\      the stack.  SPILL pushes it out; NEED-ACC brings it back, which
\      costs an IMM 0 / ADD because C4 has no plain pop.
\
\   2. CODE MOTION (B5b).  Every live item remembers WHERE ITS CODE
\      BEGINS, so the compiler can still move it.  SWAP, ROT, -ROT and
\      2SWAP are then permutations of the output buffer costing nothing
\      at runtime, and ! falls out of SWAP.
\
\   3. THE FRAME (B5c).  ENT reserves cells below bp, so the compiler has
\      somewhere to put things that are not stack items: >R's saved
\      value, and a counted loop's index and limit.  Reading one is
\      LEA/LI -- two instructions where the threaded engine charges
\      twenty for I.
\
\ And because the frame is bp-relative while the data stack grows below
\ it, the compiler always knows the address of any live item: with s
\ items spilled, the one d below the top is at bp-(ENT+s-d).  That is the
\ general fallback -- OVER, 2DUP, ROT after a pin, MIN/MAX -- and it is
\ why B5c compiles words B5b had to decline.
\
\ Calls are INLINED rather than called.  A software return stack would
\ cost about as much per call as the NEXT it replaces (see B5c in
\ docs/c4th-design.md), and inlining also lets the callee's items merge
\ into the caller's model, so a called word optimizes as if written out.
\ Recursion is declined, which is the honest answer for an inliner.

\ -- limits ------------------------------------------------------------

4096 CONSTANT NMAX                 \ threaded cells mappable, across all
                                   \ inlined bodies
64 CONSTANT NITEMS                 \ live compile-time stack items
8 CONSTANT NPERMMAX                \ regions one permutation may touch
32 CONSTANT NINMAX                 \ inline nesting
1 1 CELLS 8 * 1- LSHIFT CONSTANT NMININT

\ -- state -------------------------------------------------------------

VARIABLE NDEPTH                    \ live items
VARIABLE NACC                      \ is the top one in the accumulator?
VARIABLE NOK                       \ cleared when something is unsupported
VARIABLE NBAD                      \ the xt that stopped it, for surveying
VARIABLE NDEAD                     \ set where control cannot fall through
VARIABLE NCUR                      \ the word being emitted, for NBAD

\ Set NOPC and the backend emits the fused opcodes it can use -- LDL,
\ STL and POPA -- instead of the two- and three-instruction sequences
\ that stand in for them. OFF by default, because with it off the
\ emitted code uses nothing above MOD and runs on every host; with it
\ on it needs c4mp, which is where those opcodes live.
\ See docs/fused-opcodes.md.
VARIABLE NOPC

CREATE ISTART NITEMS CELLS ALLOT   \ where item i's code begins, or -1 for
                                   \ "not movable"
CREATE ISEP   NITEMS CELLS ALLOT   \ 1 if a PSH separator precedes it
VARIABLE NENTRY                    \ arguments the definition was given
CREATE ISCRATCH 65536 ALLOT
VARIABLE NPINOFF                   \ nothing starting below this may move

CREATE NMAP  NMAX CELLS ALLOT      \ threaded slot -> native address
CREATE NTGT  NMAX CELLS ALLOT      \ non-zero if a threaded slot is a target
CREATE NTDEP NMAX CELLS ALLOT      \ NDEPTH required there, or -1
CREATE NFIXA NMAX CELLS ALLOT      \ operand cell awaiting a target
CREATE NFIXT NMAX CELLS ALLOT      \ the threaded slot it should point at
VARIABLE NFIXN
CREATE NLEAF NMAX CELLS ALLOT      \ LEA operands awaiting the frame size
VARIABLE NLEAN

VARIABLE NBASE                     \ threaded start of the body in hand
VARIABLE NEND                      \ one past its last cell
VARIABLE NORIG                     \ its first slot in NMAP
VARIABLE NSLOT                     \ next free slot

VARIABLE NRSP                      \ frame cells in use
VARIABLE NRMAX                     \ high water mark: the ENT operand
VARIABLE NENTA                     \ address of ENT's operand cell

CREATE NLSTK 32 CELLS ALLOT        \ open counted loops: their index cell
VARIABLE NLSP

CREATE NINSTK NINMAX CELLS ALLOT   \ words currently being inlined
VARIABLE NINSP
VARIABLE NINL                      \ inline depth: 0 means the outer word

VARIABLE 'NBODY                    \ NEMIT and NBODY are mutually recursive

\ -- the accumulator model ---------------------------------------------

: IOFF  ( i -- off )   CELLS ISTART + @ ;
: IOFF! ( off i -- )   CELLS ISTART + ! ;
: ASM@  ( off -- a )   ASMBUF + ;

: SPILL     NACC @ IF PSH, 0 NACC ! THEN ;
: NEED-ACC  NACC @ 0= IF
               NOPC @ IF POPA, ELSE 0 IMM, ADD, THEN  1 NACC ! THEN ;

\ a OP n -- three instructions, which is what 1+, CELLS, NEGATE, INVERT,
\ 2/, 0=, MIN, MAX, ABS and the true-flag conversion all cost. There is
\ no fused form: the immediate-ALU family was measured at 1.0% of c4cc's
\ executed instructions and 5.2% of c4sp's, which did not justify
\ sixteen more opcodes and sixteen more microcode routines on c4bb.
\ Writing it as one rule anyway keeps the call sites honest about what
\ they cost.
: NOPI, ( n op -- )  >R  PSH,  IMM,  R> OP, ;
: NADD, ( n -- )  #ADD NOPI, ;
: NMUL, ( n -- )  #MUL NOPI, ;

\ acc = frame cell k, for a compile-time-known k.
: FLD, ( k -- )  NEGATE  NOPC @ IF LDL, ELSE LEA, LI, THEN ;

\ Forth wants all-bits-set for true; C4's comparisons yield 1. Multiplying
\ by -1 is the cheapest exact conversion, and it is correct for the 0/1
\ the VM actually produces.
: TOFLAG    -1 NMUL, ;

\ A word that needs n items and has not got them is a broken definition,
\ so say so rather than emitting code that reads below the stack.
: NEED ( n -- )  NDEPTH @ > IF 0 NOK ! THEN ;

\ Start a new item. Whatever is in the accumulator belongs to the item
\ below, so spill it, then record where this one begins.
\
\ ISEP records whether that spill actually happened. It usually does, and
\ then the cell just before this item's code is the PSH that pushed the
\ one below -- which is the separator a permutation rewrites. When the
\ item below was ALREADY on the stack, no PSH is emitted, the preceding
\ cell is ordinary code, and a permutation that assumed otherwise would
\ overwrite it. See NPERM.
: NEWITEM ( -- )
   NDEPTH @ NITEMS < IF NACC @ NDEPTH @ CELLS ISEP + ! THEN
   SPILL
   NDEPTH @ NITEMS < IF ASM-LEN NDEPTH @ IOFF! ELSE 0 NOK ! THEN
   1 NDEPTH +! ;

\ The accumulator holds a value whose code cannot be moved -- it was
\ produced in place, by a local branch or by an sp-relative load.
: NRESULT ( -- )
   NDEPTH @ NITEMS < IF -1 NDEPTH @ IOFF! ELSE 0 NOK ! THEN
   1 NDEPTH +!  1 NACC ! ;

\ After a branch nothing may move across it. ISTART goes to -1 rather
\ than to ASM-LEN: "the code is right here" is a lie that a later in-place
\ operation would turn into a wrong rotation, since the region would then
\ appear to be that operation's code alone.
: NFLUSH ( -- )
   NDEPTH @ 0 ?DO -1 I IOFF! LOOP ;

\ -- the frame ---------------------------------------------------------
\ Cell k of the frame is at LEA -k. Allocation is a stack: a counted loop
\ takes two cells and gives them back at LOOP, >R takes one and gives it
\ back at R>. NRMAX is the high water mark and becomes the ENT operand,
\ which is patched in at the end because it is not known until then.

: SLOT+ ( n -- k )
   NRSP @ 1+ SWAP NRSP +!
   NRSP @ NRMAX @ > IF NRSP @ NRMAX ! THEN ;
: SLOT- ( n -- )  NEGATE NRSP +! ;

\ -- addressing a live item --------------------------------------------
\ Every live item has an address, which is what lets the backend compile
\ words that reorder or copy the stack at all. Item i is the (i+1)'th
\ thing pushed since ENT -- everything below the top is always spilled,
\ so an item's position never depends on what happened after it -- and
\ therefore sits at bp-(frame+i+1). The frame size is not known until the
\ end, so the operand goes out as -(i+1) and the rest is subtracted then.
\
\ Code holding such an address is pinned: moving it past another item's
\ code would change how many things are on the stack, and the address
\ with it. NPINOFF records how far the buffer had filled when something
\ got pinned, and a permutation is refused unless every region it touches
\ begins at or after that mark.

: NLEAREC ( a -- )
   NLEAN @ NMAX < IF NLEAN @ CELLS NLEAF + ! 1 NLEAN +!
   ELSE DROP 0 NOK ! THEN ;

: IADDR, ( i -- )                            \ acc = address of item i
   1+ NEGATE LEA,
   ASM-HERE 1 CELLS - NLEAREC  ASM-LEN NPINOFF ! ;

: ILD, ( i -- )                              \ acc = value of item i
   NOPC @ 0= IF IADDR, LI, EXIT THEN
   1+ NEGATE LDL,
   ASM-HERE 1 CELLS - NLEAREC  ASM-LEN NPINOFF ! ;

: NA, ( d -- )  NDEPTH @ 1- SWAP - IADDR, ;  \ d items below the top
: NL, ( d -- )  NDEPTH @ 1- SWAP - ILD, ;

\ Push a copy of the item d below the top. This is OVER, 2DUP, 2OVER and
\ TUCK's second half, and it is two instructions.
: N-COPY ( d -- )
   >R NEWITEM
   R> 1+ NA,  LI,  1 NACC !
   -1 NDEPTH @ 1- IOFF! ;

: NDROPS ( n -- )
   DUP NDEPTH @ > IF DROP 0 NOK ! EXIT THEN
   ?DUP IF DUP ADJ, NEGATE NDEPTH +! THEN ;

\ Move the top item into frame cell k and drop it. With STL that is the
\ whole of it -- and the ordering problem that made stores expensive
\ disappears, because STL takes its address as an operand rather than
\ off the stack.
: SLOT! ( k -- )
   1 NEED
   NOPC @ IF
      NEED-ACC  NEGATE STL,  -1 NDEPTH +!  0 NACC !
      ASM-LEN NPINOFF !  EXIT
   THEN
   SPILL
   NEGATE LEA, PSH,                \ &frame[k]
   0 NL,                           \ the value
   SI,                             \ frame[k] = value
   1 NDROPS
   ASM-LEN NPINOFF ! ;

\ Push frame cell k. Bp-relative, so this region IS movable.
: SLOT@ ( k -- )
   NEWITEM  FLD,  1 NACC ! ;

\ -- permuting the top regions -----------------------------------------
\ The problem SWAP poses is not shuffling a stack, it is that C4 has one
\ register: with x2 in the accumulator and x1 beneath it there is nowhere
\ to put x2 while x1 is fetched.
\
\ The way out is to stop treating emitted code as fixed. Item i's code
\ runs from ISTART[i] to ISTART[i+1] minus the PSH that spilled it, and
\ each such region is BALANCED -- it computes one value into the
\ accumulator and leaves the stack as it found it -- so the regions may
\ be written back in any order:
\
\     [ A ][ PSH ][ B ][ PSH ][ C ]  ->  [ B ][ PSH ][ C ][ PSH ][ A ]
\
\ which is ROT, for nothing. SWAP, -ROT and 2SWAP are the same machine.

CREATE RSTART NPERMMAX CELLS ALLOT
CREATE RLEN   NPERMMAX CELLS ALLOT
CREATE RSOFF  NPERMMAX CELLS ALLOT
CREATE NPRM   NPERMMAX CELLS ALLOT      \ NPRM[i] = which region lands at i
VARIABLE NK  VARIABLE NB0  VARIABLE NPP  VARIABLE NPI  VARIABLE NPB

: R@S ( i -- a )  CELLS RSTART + ;
: R@L ( i -- a )  CELLS RLEN + ;
: R@O ( i -- a )  CELLS RSOFF + ;
: R@P ( i -- a )  CELLS NPRM + ;

: PERM2   1 0 R@P !  0 1 R@P ! ;                                \ SWAP
: PERM3   1 0 R@P !  2 1 R@P !  0 2 R@P ! ;                     \ ROT
: PERM3R  2 0 R@P !  0 1 R@P !  1 2 R@P ! ;                     \ -ROT
: PERM4   2 0 R@P !  3 1 R@P !  0 2 R@P !  1 3 R@P ! ;          \ 2SWAP

: NPERM ( k -- ok? )
   DUP NK !  NDEPTH @ SWAP -  DUP NB0 !  0 < IF 0 EXIT THEN
   NK @ 2 < IF 0 EXIT THEN
   NK @ NPERMMAX > IF 0 EXIT THEN
   NEED-ACC
   NK @ 0 DO
      NB0 @ I + IOFF  DUP 0 < IF DROP UNLOOP 0 EXIT THEN  I R@S !
   LOOP
   \ Every region but the first must be preceded by a real PSH, because
   \ the rebuild writes the separators back and would otherwise write one
   \ over a cell that is code -- which is a wrong answer, not a crash.
   NK @ 1 ?DO
      NB0 @ I + CELLS ISEP + @ 0= IF UNLOOP 0 EXIT THEN
   LOOP
   0 R@S @ NPINOFF @ < IF 0 EXIT THEN
   NK @ 1- 0 ?DO
      I 1+ R@S @  1 CELLS -  I R@S @ -  I R@L !
   LOOP
   ASM-LEN  NK @ 1- R@S @ -  NK @ 1- R@L !
   NK @ 0 DO I R@L @ 0 <= IF UNLOOP 0 EXIT THEN LOOP
   0 NPP !
   NK @ 0 DO I R@L @ NPP +! LOOP
   NPP @ 65536 > IF 0 EXIT THEN
   0 NPP !
   NK @ 0 DO
      I R@S @ ASM@  ISCRATCH NPP @ +  I R@L @ MOVE
      NPP @ I R@O !
      I R@L @ NPP +!
   LOOP
   0 R@S @ NPP !
   NK @ 0 DO
      I R@P @ NPI !
      NPI @ R@O @ ISCRATCH +   NPP @ ASM@   NPI @ R@L @   MOVE
      NPP @  NB0 @ I +  IOFF!
      NPI @ R@L @ NPP +!
      I NK @ 1- < IF  #PSH  NPP @ ASM@ !  1 CELLS NPP +!  THEN
   LOOP
   1 ;

\ When the regions cannot move -- after a branch, or once something is
\ pinned -- the same permutation goes through the frame instead: read the
\ k items into k frame cells, write them back in the new order. Ten
\ instructions per item, about what the threaded engine charges, but it
\ lets the REST of the word compile, which is where the win is.
: NPERM-MEM ( k -- ok? )
   DUP NK !  NDEPTH @ SWAP -  DUP NB0 !  0 < IF 0 EXIT THEN
   NK @ NPERMMAX > IF 0 EXIT THEN
   SPILL
   NK @ SLOT+ NPB !
   NK @ 0 DO
      NPB @ I + NEGATE LEA, PSH,
      NK @ 1- I - NL,
      SI,
   LOOP
   NK @ 0 DO
      NK @ 1- I - NA, PSH,
      NPB @ I R@P @ + NEGATE LEA, LI,
      SI,
   LOOP
   NK @ SLOT-
   0 NACC !  ASM-LEN NPINOFF !
   NK @ 0 DO -1 NB0 @ I + IOFF! LOOP
   1 ;

: NPERM! ( k -- )  DUP NPERM 0= IF NPERM-MEM 0= IF 0 NOK ! THEN ELSE DROP THEN ;

\ -- the words this backend knows --------------------------------------

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
' ROT CONSTANT nROT   ' -ROT CONSTANT nNROT   ' 2SWAP CONSTANT n2SWAP
' TUCK CONSTANT nTUCK ' 2DUP CONSTANT n2DUP   ' 2DROP CONSTANT n2DROP
' 2OVER CONSTANT n2OVER
' >R CONSTANT nTOR    ' R> CONSTANT nRFROM    ' R@ CONSTANT nRAT
' (DO) CONSTANT nPDO  ' (LOOP) CONSTANT nPLOOP ' (+LOOP) CONSTANT nPPLOOP
' I CONSTANT nI       ' J CONSTANT nJ         ' UNLOOP CONSTANT nUNLOOP
' (S") CONSTANT nPSQ
' LSHIFT CONSTANT nLSH ' 2/ CONSTANT n2DIV    ' +! CONSTANT nPLUSST
' MIN CONSTANT nMIN   ' MAX CONSTANT nMAX     ' ABS CONSTANT nABS
' U< CONSTANT nULT    ' U> CONSTANT nUGT      ' /MOD CONSTANT nDIVMOD

\ Header accessors, and the three code fields worth recognising: a colon
\ definition, a CREATEd word (every VARIABLE), and a CREATE/DOES> word
\ (every CONSTANT). Finding out which an xt is means comparing its code
\ field against a known example's -- there is nothing else to compare.
: >WCODE ( xt -- a )  5 CELLS + @ ;
: >WDOES ( xt -- a )  8 CELLS + @ ;
: >WBODY ( xt -- a )  9 CELLS + ;

CREATE NVPROBE
' NVPROBE >WCODE CONSTANT NDOVAR
: NPROBE ;
' NPROBE >WCODE CONSTANT NCOLON
1 CONSTANT NDPROBE
' NDPROBE >WCODE CONSTANT NDOESC

: BIN, ( opcode -- )  2 NEED NEED-ACC OP, -1 NDEPTH +! ;
: CMP, ( opcode -- )  2 NEED NEED-ACC OP, TOFLAG -1 NDEPTH +! ;

\ -- walking threaded code ---------------------------------------------
\ Five words carry an operand cell in the body, and (S") carries a
\ counted string. Knowing that in one place keeps the target scan, the
\ body-end scan and the emit loop from disagreeing about it, which is a
\ disagreement that shows up as a branch into the middle of an
\ instruction.

: NSTEP ( a -- a' )
   DUP @
   DUP nPSQ = IF DROP
      DUP 1 CELLS + @  1 CELLS + 1-  1 CELLS /  2 + CELLS +  EXIT THEN
   DUP nLIT = OVER nBRANCH = OR OVER n0BRANCH = OR
   OVER nPLOOP = OR OVER nPPLOOP = OR
   IF DROP 2 CELLS + EXIT THEN
   DROP 1 CELLS + ;

: NBRANCHY? ( xt -- f )
   DUP nBRANCH = OVER n0BRANCH = OR OVER nPLOOP = OR SWAP nPPLOOP = OR ;

\ A definition ends at its EXIT.
: BODY-END ( body -- end )
   BEGIN DUP @ nEXIT = 0= WHILE NSTEP REPEAT 1 CELLS + ;

: SRCOFF ( a -- n )  NBASE @ - 1 CELLS / NORIG @ + ;

: FIX! ( t -- )
   NFIXN @ NMAX < IF
      ASM-HERE 1 CELLS -  NFIXN @ CELLS NFIXA + !
      NFIXN @ CELLS NFIXT + !
      1 NFIXN +!
   ELSE DROP 0 NOK ! THEN ;

\ The state at a branch target cannot depend on which way it was reached.
\ The accumulator is emptied on every path, but the DEPTH has to agree
\ too -- and it silently would not, for a definition whose arms leave
\ different numbers of values, which is exactly the shape that turns an
\ item's frame offset into a wrong one. Recording the depth the first
\ time a target is named and checking it every time after turns that into
\ a decline.
: TDEP! ( t -- )
   DUP 0 < IF DROP EXIT THEN
   DUP NMAX >= IF DROP EXIT THEN
   CELLS NTDEP +
   DUP @ 0 < IF NDEPTH @ SWAP ! EXIT THEN
   @ NDEPTH @ <> IF 0 NOK ! THEN ;

\ Reaching a target from code that cannot fall through -- the cell after
\ an unconditional BRANCH, which is where ?DO puts its loop entry --
\ means the model has no state of its own to check. It ADOPTS the depth
\ recorded by whoever branches here instead. Checking it there would
\ compare against whatever the dead path happened to leave, and decline a
\ perfectly good definition.
: TARGET! ( t -- )
   NDEAD @ 0= IF TDEP! EXIT THEN
   0 NDEAD !  0 NACC !
   DUP 0 >= OVER NMAX < AND IF
      CELLS NTDEP + @  DUP 0 < IF DROP 0 NOK ! ELSE NDEPTH ! THEN
   ELSE DROP 0 NOK ! THEN ;

\ -- counted loops -----------------------------------------------------
\ The index and the limit live in two frame cells, so I is LEA/LI and
\ LOOP is a compare against a fixed address. UNLOOP emits nothing: the
\ cells are compile-time, and EXIT's LEV restores sp regardless.

: NLIDX  ( -- k )  NLSP @ 1- CELLS NLSTK + @ ;
: NLIDX2 ( -- k )  NLSP @ 2 - CELLS NLSTK + @ ;

: N-PDO ( -- )
   2 NEED
   2 SLOT+
   DUP SLOT!                       \ index -> frame[k]
   DUP 1+ SLOT!                    \ limit -> frame[k+1]
   NLSP @ 32 < IF  DUP NLSP @ CELLS NLSTK + !  1 NLSP +!
             ELSE  0 NOK ! THEN
   DROP ;

: N-PLOOP ( a -- )
   NLSP @ 0= IF DROP 0 NOK ! EXIT THEN
   SPILL
   NOPC @ IF
      NLIDX FLD,  1 NADD,  NLIDX NEGATE STL,
   ELSE
      NLIDX NEGATE LEA, PSH,       \ &index
      NLIDX FLD,                   \ index
      1 NADD,             \ index+1
      SI,                          \ store it; SI leaves it in the accumulator
   THEN
   PSH, NLIDX 1+ FLD,              \ limit
   NE,                             \ index+1 <> limit
   0 BNZ,
   1 CELLS + @ SRCOFF DUP FIX! TDEP!
   -1 NLSP +!  2 SLOT-
   ASM-LEN NPINOFF !  NFLUSH ;

\ (+LOOP) terminates when the index crosses the boundary between limit-1
\ and limit in EITHER direction, which is why a plain >= is wrong for a
\ negative step. Biasing both the old and the new index by the limit
\ turns "crossed" into "the sign of the difference changed" -- one XOR,
\ exactly as the threaded primitive does it.
: N-PPLOOP ( a -- )
   NLSP @ 0= IF DROP 0 NOK ! EXIT THEN
   1 NEED
   1 SLOT+ >R
   R@ SLOT!                        \ the step
   NLIDX FLD, PSH, NLIDX 1+ FLD, SUB,                         \ i-l
   PSH,
   NOPC @ IF
      NLIDX FLD, PSH, R@ FLD, ADD,                            \ nu = i+n
      NLIDX NEGATE STL,
   ELSE
      NLIDX NEGATE LEA, PSH,
      NLIDX FLD, PSH, R@ FLD, ADD,
      SI,
   THEN
   R> DROP
   PSH, NLIDX 1+ FLD, SUB,                                    \ nu-l
   XOR,
   0 #LT NOPI,
   0 BZ,
   1 CELLS + @ SRCOFF DUP FIX! TDEP!
   1 SLOT-
   -1 NLSP +!  2 SLOT-
   ASM-LEN NPINOFF !  NFLUSH ;

\ -- the ones that need a branch of their own --------------------------

: N-MINMAX ( swap? -- )       \ 0 = MAX, 1 = MIN
   2 NEED
   SPILL
   1 NL, PSH, 0 NL, LT,            \ a < b
   IF 0 BNZ, ELSE 0 BZ, THEN >MARK
      0 NL,                        \ b
      0 JMP, >MARK
   SWAP >RESOLVE
      1 NL,                        \ a
   >RESOLVE
   2 NDROPS  NRESULT
   ASM-LEN NPINOFF ! ;

: N-ABS ( -- )
   1 NEED
   SPILL
   0 NL, 0 #LT NOPI,
   0 BZ, >MARK
      0 NL, -1 NMUL,
      0 JMP, >MARK
   SWAP >RESOLVE
      0 NL,
   >RESOLVE
   1 NDROPS  NRESULT
   ASM-LEN NPINOFF ! ;

\ C4 has no unsigned compare. Flipping the sign bit of both operands maps
\ unsigned order onto signed order exactly, which is two XORs.
: N-UCMP ( gt? -- )
   2 NEED
   SPILL
   IF 0 ELSE 1 THEN  DUP >R  NL, NMININT #XOR NOPI, PSH,
   R> 1 XOR NL, NMININT #XOR NOPI,
   LT,
   2 NDROPS  NRESULT  TOFLAG
   ASM-LEN NPINOFF ! ;

: N-DIVMOD ( -- )               \ ( a b -- rem quot )
   2 NEED
   SPILL
   1 SLOT+ >R
   R@ NEGATE LEA, PSH,  1 NL, PSH, 0 NL, DIV,  SI,     \ tmp = a/b
   1 NA, PSH,           1 NL, PSH, 0 NL, MOD,  SI,     \ a    = a mod b
   0 NA, PSH,           R> NEGATE LEA, LI,     SI,     \ b    = tmp
   1 SLOT-
   0 NACC !  ASM-LEN NPINOFF !
   -1 NDEPTH @ 1- IOFF!  -1 NDEPTH @ 2 - IOFF! ;

: N-PLUSSTORE ( -- )            \ ( n a -- )
   2 NEED
   SPILL
   0 NL, PSH,  0 NL, LI, PSH,  1 NL, ADD,  SI,
   2 NDROPS
   ASM-LEN NPINOFF ! ;

\ ! wants the address pushed before the value is computed, while Forth
\ writes the value first. With code motion that is just SWAP then SI; if
\ the regions are pinned, both operands are read back through the frame
\ instead.
: N-STORE ( opcode -- )
   2 NEED
   \ The rotation leaves the address on the stack and the value in the
   \ accumulator, which is exactly what SI wants -- and SI pops the
   \ address itself, so neither item costs an ADJ.
   PERM2 2 NPERM IF OP, 0 NACC ! -2 NDEPTH +! EXIT THEN
   >R SPILL
   0 NL, PSH,  1 NL,  R> OP,
   2 NDROPS
   ASM-LEN NPINOFF ! ;

\ OVER may re-run the region below the top when that region can simply be
\ executed again -- a bare IMM, which is a literal or a variable's
\ address. That case emits one instruction and stays movable, so it is
\ worth keeping even though N-COPY handles everything.
: N-OVER ( -- )
   2 NEED
   NDEPTH @ 2 < IF EXIT THEN
   NEED-ACC
   NDEPTH @ 1- CELLS ISEP + @ 0= IF 1 N-COPY EXIT THEN
   NDEPTH @ 2 - IOFF   NDEPTH @ 1- IOFF   ( sa sb )
   2DUP 0 >= SWAP 0 >= AND IF
      2DUP SWAP - 1 CELLS -  2 CELLS = IF
         DROP DUP ASM@ @ #IMM = IF
            1 CELLS + ASM@ @  NEWITEM IMM,  1 NACC !  EXIT
         THEN
         DROP 1 N-COPY EXIT
      THEN
   THEN
   2DROP 1 N-COPY ;

\ -- inlining ----------------------------------------------------------

: NINSEEN? ( xt -- f )
   NINSP @ 0 ?DO
      DUP I CELLS NINSTK + @ = IF DROP 1 UNLOOP EXIT THEN
   LOOP DROP 0 ;

: NPUSHIN ( xt -- ok? )
   NINSP @ NINMAX < IF NINSP @ CELLS NINSTK + ! 1 NINSP +! 1
   ELSE DROP 0 THEN ;

: NCALL ( xt -- )
   DUP NINSEEN? IF NBAD ! 0 NOK ! EXIT THEN
   DUP >WBODY DUP BODY-END              ( xt body end )
   ROT NPUSHIN 0= IF 2DROP 0 NOK ! EXIT THEN
   1 NINL +!
   'NBODY @ EXECUTE
   -1 NINL +!  -1 NINSP +! ;

\ A CREATE/DOES> word pushes its body address and then runs the code its
\ definer left behind, so inlining it is one IMM followed by that code.
\ CONSTANT is CREATE , DOES> @ -- which compiles, after inlining, to
\ exactly IMM addr; LI.
: NCALL-DOES ( xt -- )
   DUP NINSEEN? IF NBAD ! 0 NOK ! EXIT THEN
   DUP >WBODY NEWITEM IMM, 1 NACC !
   DUP >WDOES DUP BODY-END              ( xt does end )
   ROT NPUSHIN 0= IF 2DROP 0 NOK ! EXIT THEN
   1 NINL +!
   'NBODY @ EXECUTE
   -1 NINL +!  -1 NINSP +! ;

\ -- one threaded instruction ------------------------------------------

: NEMIT ( a xt -- )
   DUP nLIT = IF DROP 1 CELLS + @ NEWITEM IMM, 1 NACC ! EXIT THEN
   DUP n+ = IF 2DROP #ADD BIN, EXIT THEN
   DUP n- = IF 2DROP #SUB BIN, EXIT THEN
   DUP n* = IF 2DROP #MUL BIN, EXIT THEN
   DUP n/ = IF 2DROP #DIV BIN, EXIT THEN
   DUP nMOD = IF 2DROP #MOD BIN, EXIT THEN
   DUP nAND = IF 2DROP #AND BIN, EXIT THEN
   DUP nOR  = IF 2DROP #OR  BIN, EXIT THEN
   DUP nXOR = IF 2DROP #XOR BIN, EXIT THEN
   DUP nLSH = IF 2DROP #SHL BIN, EXIT THEN
   DUP n=  = IF 2DROP #EQ CMP, EXIT THEN
   DUP n<> = IF 2DROP #NE CMP, EXIT THEN
   DUP n<  = IF 2DROP #LT CMP, EXIT THEN
   DUP n>  = IF 2DROP #GT CMP, EXIT THEN
   DUP n<= = IF 2DROP #LE CMP, EXIT THEN
   DUP n>= = IF 2DROP #GE CMP, EXIT THEN
   DUP n1+ = IF 2DROP 1 NEED NEED-ACC 1 NADD, EXIT THEN
   DUP n1- = IF 2DROP 1 NEED NEED-ACC -1 NADD, EXIT THEN
   DUP n@  = IF 2DROP 1 NEED NEED-ACC LI, EXIT THEN
   DUP nC@ = IF 2DROP 1 NEED NEED-ACC LC, EXIT THEN
   DUP n2* = IF 2DROP 1 NEED NEED-ACC 2 NMUL, EXIT THEN
   DUP n2DIV = IF 2DROP 1 NEED NEED-ACC 1 #SHR NOPI, EXIT THEN
   DUP nCELLS = IF 2DROP 1 NEED NEED-ACC 1 CELLS NMUL, EXIT THEN
   DUP nCELL+ = IF 2DROP 1 NEED NEED-ACC 1 CELLS NADD, EXIT THEN
   DUP nNEG = IF 2DROP 1 NEED NEED-ACC -1 NMUL, EXIT THEN
   DUP nINV = IF 2DROP 1 NEED NEED-ACC -1 #XOR NOPI, EXIT THEN
   DUP n0= = IF 2DROP 1 NEED NEED-ACC 0 #EQ NOPI, TOFLAG EXIT THEN
   DUP n0<> = IF 2DROP 1 NEED NEED-ACC 0 #NE NOPI, TOFLAG EXIT THEN
   DUP n0< = IF 2DROP 1 NEED NEED-ACC 0 #LT NOPI, TOFLAG EXIT THEN
   DUP n0> = IF 2DROP 1 NEED NEED-ACC 0 #GT NOPI, TOFLAG EXIT THEN
   \ DUP is the spill NEWITEM already emits: the value stays in the
   \ accumulator and a copy of it is now on the stack.
   DUP nDUP = IF 2DROP 1 NEED NEED-ACC NEWITEM 1 NACC !
                    -1 NDEPTH @ 1- IOFF! EXIT THEN
   DUP nNIP = IF 2DROP 2 NEED NEED-ACC 1 ADJ, -1 NDEPTH +!
      -1 NDEPTH @ 1- IOFF! EXIT THEN
   DUP nDROP = IF 2DROP 1 NEED
                    NACC @ IF 0 NACC ! -1 NDEPTH +! ELSE 1 NDROPS THEN
                    EXIT THEN
   DUP n2DROP = IF 2DROP 2 NEED
                    NACC @ IF 0 NACC ! -1 NDEPTH +! 1 NDROPS
                           ELSE 2 NDROPS THEN
                    EXIT THEN
   DUP nSWAP = IF 2DROP 2 NEED PERM2 2 NPERM! EXIT THEN
   DUP nROT  = IF 2DROP 3 NEED PERM3 3 NPERM! EXIT THEN
   DUP nNROT = IF 2DROP 3 NEED PERM3R 3 NPERM! EXIT THEN
   DUP n2SWAP = IF 2DROP 4 NEED PERM4 4 NPERM! EXIT THEN
   DUP nOVER = IF 2DROP N-OVER EXIT THEN
   DUP n2DUP = IF 2DROP 2 NEED 1 N-COPY 1 N-COPY EXIT THEN
   DUP n2OVER = IF 2DROP 4 NEED 3 N-COPY 3 N-COPY EXIT THEN
   DUP nTUCK = IF 2DROP 2 NEED PERM2 2 NPERM! 1 N-COPY EXIT THEN
   DUP n! = IF 2DROP #SI N-STORE EXIT THEN
   DUP nC! = IF 2DROP #SC N-STORE EXIT THEN
   DUP nPLUSST = IF 2DROP N-PLUSSTORE EXIT THEN
   DUP nMIN = IF 2DROP 1 N-MINMAX EXIT THEN
   DUP nMAX = IF 2DROP 0 N-MINMAX EXIT THEN
   DUP nABS = IF 2DROP N-ABS EXIT THEN
   DUP nULT = IF 2DROP 0 N-UCMP EXIT THEN
   DUP nUGT = IF 2DROP 1 N-UCMP EXIT THEN
   DUP nDIVMOD = IF 2DROP N-DIVMOD EXIT THEN
   DUP nTOR = IF 2DROP 1 NEED 1 SLOT+ SLOT! EXIT THEN
   DUP nRFROM = IF 2DROP NRSP @ 0= IF 0 NOK !
                    ELSE NRSP @ SLOT@ 1 SLOT- THEN EXIT THEN
   DUP nRAT = IF 2DROP NRSP @ 0= IF 0 NOK ! ELSE NRSP @ SLOT@ THEN EXIT THEN
   DUP nPDO = IF 2DROP N-PDO EXIT THEN
   DUP nPLOOP = IF DROP N-PLOOP EXIT THEN
   DUP nPPLOOP = IF DROP N-PPLOOP EXIT THEN
   DUP nI = IF 2DROP NLSP @ 0= IF 0 NOK ! ELSE NLIDX SLOT@ THEN EXIT THEN
   DUP nJ = IF 2DROP NLSP @ 2 < IF 0 NOK ! ELSE NLIDX2 SLOT@ THEN EXIT THEN
   DUP nUNLOOP = IF 2DROP EXIT THEN
   \ Branches canonicalise: everything on the stack, accumulator free, so
   \ that the state at a target does not depend on which way it was
   \ reached. 0BRANCH needs its flag in the accumulator to test.
   DUP n0BRANCH = IF DROP
      1 NEED NEED-ACC 0 BZ,
      -1 NDEPTH +! 0 NACC !
      1 CELLS + @ SRCOFF DUP FIX! TDEP!
      NFLUSH EXIT THEN
   DUP nBRANCH = IF DROP
      SPILL 0 JMP,
      1 CELLS + @ SRCOFF DUP FIX! TDEP!
      NFLUSH 1 NDEAD ! EXIT THEN
   DUP nEXIT = IF DROP
      NINL @ IF  NEND @ 1 CELLS - <> IF 0 NOK ! THEN
             ELSE DROP NDEPTH @ IF NEED-ACC THEN LEV, 1 NDEAD ! THEN
      EXIT THEN
   DUP >WCODE NCOLON = IF NIP NCALL EXIT THEN
   DUP >WCODE NDOESC = IF NIP NCALL-DOES EXIT THEN
   DUP >WCODE NDOVAR = IF NIP >WBODY NEWITEM IMM, 1 NACC ! EXIT THEN
   \ Anything else: this backend does not know it. NBAD keeps the xt so a
   \ survey can say what stopped it.
   NBAD ! DROP 0 NOK ! ;

\ -- one body ----------------------------------------------------------

: NSCAN ( body end -- )
   NSLOT @ NORIG @ ?DO  0 I CELLS NTGT + !  -1 I CELLS NTDEP + !  LOOP
   OVER                                 ( body end a )
   BEGIN 2DUP > WHILE
      DUP @ NBRANCHY? IF
         DUP 1 CELLS + @ SRCOFF
         DUP 0 >= OVER NMAX < AND IF 1 SWAP CELLS NTGT + ! ELSE DROP THEN
      THEN
      NSTEP
   REPEAT DROP 2DROP ;

: NBODY ( body end -- )
   NBASE @ >R  NEND @ >R  NORIG @ >R
   OVER NBASE !  DUP NEND !
   NSLOT @ NORIG !
   2DUP SWAP - 1 CELLS / NSLOT +!
   NSLOT @ NMAX > IF 0 NOK ! THEN
   NOK @ IF
      2DUP NSCAN
      OVER                              ( body end a )
      BEGIN 2DUP > NOK @ AND WHILE
         DUP SRCOFF DUP CELLS NTGT + @ IF SPILL DUP TARGET! NFLUSH THEN DROP
         DUP SRCOFF CELLS NMAP + ASM-HERE SWAP !
         DUP DUP @ DUP NCUR ! NEMIT
         NOK @ 0= NBAD @ 0= AND IF NCUR @ NBAD ! THEN
         NSTEP
      REPEAT DROP
   THEN
   2DROP
   R> NORIG !  R> NEND !  R> NBASE ! ;

' NBODY 'NBODY !

\ -- compiling a definition --------------------------------------------
\ n is how many arguments the definition is entered with. They are C4
\ arguments -- pushed by the caller and reached off bp -- so a compiled
\ word with n of them is called exactly like any other C4 function of n
\ arguments, which is what INVOKE1/2/3 do.
\
\ The prologue copies them onto the stack rather than leaving them where
\ C4 puts them. Three instructions each, once, and in exchange the body
\ has ONE kind of item: everything is on the stack, + is one ADD again,
\ and the model at a loop's back edge is the same model it had on the way
\ in -- which it is not if some items live in the frame and some do not,
\ because the first iteration spills them and the second finds them
\ somewhere else.

: NCOMPILE-N ( body end n -- ok? )
   ASM-RESET
   DUP NENTRY !  DUP NDEPTH !
   0 NACC !   1 NOK !   0 NBAD !
   0 NFIXN !  0 NLEAN !  0 NPINOFF !  0 NDEAD !
   0 NRSP !   0 NRMAX !  0 NLSP !
   0 NINSP !  0 NINL !
   0 NSLOT !  0 NBASE !  0 NORIG !  0 NEND !
   DUP NITEMS > IF 0 NOK ! THEN
   0 ENT,  ASM-HERE 1 CELLS - NENTA !
   NOK @ IF
      DUP 0 ?DO  1 OVER + I -  LEA, LI, PSH,  -1 I IOFF!  LOOP
   THEN
   DROP
   NOK @ IF 'NBODY @ EXECUTE ELSE 2DROP THEN
   NOK @ IF
      NFIXN @ 0 ?DO
         I CELLS NFIXT + @ CELLS NMAP + @
         I CELLS NFIXA + @ !
      LOOP
      NLEAN @ 0 ?DO
         I CELLS NLEAF + @  DUP @ NRMAX @ -  SWAP !
      LOOP
      NRMAX @ NENTA @ !
   THEN
   NOK @ ;

: NCOMPILE ( body end -- ok? )  0 NCOMPILE-N ;

\ How many arguments does this definition take? Nothing declares it, so
\ ask the compiler: the fewest that compile without underflowing is the
\ answer, because NEED refuses to emit code that would read below the
\ stack it was given.
6 CONSTANT NARITYMAX
: NCOMPILE? ( body end -- n | -1 )
   NARITYMAX 1+ 0 DO
      2DUP I NCOMPILE-N IF 2DROP I UNLOOP EXIT THEN
   LOOP 2DROP -1 ;
