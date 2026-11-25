\ c4th peep.f -- P1, a peephole optimizer over threaded code.
\
\ The wins here are small: each rule saves roughly one trip through the
\ inner interpreter. That is not the point. The point is that the pattern
\ matcher, the instruction decoder and the address fixup all get written
\ and debugged HERE, on a representation small enough to print, before
\ B5's native backend depends on all three.
\
\ Compaction means addresses move, so this is a real rebuild: decode the
\ body into instructions, copy the survivors into a scratch buffer while
\ recording where each one landed, then rewrite every branch operand
\ through that map. A rule is refused if it would delete an instruction
\ that something branches to -- otherwise a loop would silently acquire a
\ new body.

1024 CONSTANT PEEPMAX
CREATE PEEPDST PEEPMAX CELLS ALLOT
CREATE PEEPMAP PEEPMAX CELLS ALLOT   \ src cell offset -> dst cell offset
CREATE PEEPTGT PEEPMAX CELLS ALLOT   \ non-zero if src offset is branched to
VARIABLE PEEPCUT                     \ instructions removed, cumulative
0 PEEPCUT !

\ -- the words this has to know about ----------------------------------
' LIT     CONSTANT 'LIT       ' BRANCH  CONSTANT 'BRANCH
' 0BRANCH CONSTANT '0BRANCH   ' (LOOP)  CONSTANT '(LOOP)
' (+LOOP) CONSTANT '(+LOOP)   ' (S")    CONSTANT '(S")
' EXIT    CONSTANT 'EXIT      ' DUP     CONSTANT 'DUP
' DROP    CONSTANT 'DROP      ' SWAP    CONSTANT 'SWAP
' >R      CONSTANT '>R        ' R>      CONSTANT 'R>
' +       CONSTANT '+         ' -       CONSTANT '-
' *       CONSTANT '*

\ Words carrying one inline operand cell.
: OPERAND? ( xt -- f )
   DUP 'LIT = OVER 'BRANCH = OR OVER '0BRANCH = OR
   OVER '(LOOP) = OR OVER '(+LOOP) = OR NIP ;

\ Words whose operand is a branch target rather than a value.
: BRANCHER? ( xt -- f )
   DUP 'BRANCH = OVER '0BRANCH = OR OVER '(LOOP) = OR SWAP '(+LOOP) = OR ;

\ Cells occupied by the instruction at a. (S") is the awkward one: an xt,
\ a length cell, then the text rounded up to whole cells.
: ISIZE ( a -- n )
   DUP @ '(S") = IF
      1 CELLS + @  1 CELLS + 1-  1 CELLS /  2 +
   ELSE
      @ OPERAND? IF 2 ELSE 1 THEN
   THEN ;

\ -- pass one: which offsets are branched to ---------------------------
: CLEAR-TGT  PEEPMAX 0 DO 0 PEEPTGT I CELLS + ! LOOP ;

: MARK-TARGETS ( start end -- )
   OVER -                              ( start len )
   OVER SWAP OVER +                    ( start start end )
   SWAP                                ( start end a )
   BEGIN 2DUP > WHILE                  ( start end a )
      DUP @ BRANCHER? IF
         DUP 1 CELLS + @               ( ... target-address )
         3 PICK - 1 CELLS /            ( ... target-offset )
         DUP 0 >= OVER PEEPMAX < AND IF
            1 SWAP PEEPTGT SWAP CELLS + !
         ELSE DROP THEN
      THEN
      DUP ISIZE CELLS +
   REPEAT DROP 2DROP ;

\ -- pass two: rebuild -------------------------------------------------
\ Rules, each ( a -- a' matched? ) where a' is the address to continue
\ from. They only fire when the instructions they swallow past the first
\ are not branch targets, which TGT? answers.

VARIABLE PEEPBASE
: TGT? ( a -- f )
   PEEPBASE @ - 1 CELLS /
   DUP 0 >= OVER PEEPMAX < AND IF PEEPTGT SWAP CELLS + @ ELSE DROP 0 THEN ;

: 2ND ( a -- a2 )  DUP ISIZE CELLS + ;
: 3RD ( a -- a3 )  2ND 2ND ;

VARIABLE DP2                        \ emit pointer into PEEPDST
: D,  ( n -- )  DP2 @ !  1 CELLS DP2 +! ;

: CANCELS? ( a w1 w2 -- f )   \ a's first two instructions are w1 then w2
   >R >R DUP @ R> = SWAP 2ND DUP @ R> = SWAP TGT? 0= AND AND ;

: FOLD? ( a -- f )            \ LIT x LIT y op
   DUP @ 'LIT = IF
      DUP 2ND DUP @ 'LIT = SWAP TGT? 0= AND IF
         DUP 3RD DUP @ DUP '+ = SWAP DUP '- = SWAP '* = OR OR
         SWAP TGT? 0= AND
         NIP                      \ the address has done its job
      ELSE DROP 0 THEN
   ELSE DROP 0 THEN ;

: FOLD ( a -- a' )            \ emit the folded literal, skip three
   DUP 1 CELLS + @            ( a x )
   OVER 2ND 1 CELLS + @       ( a x y )
   2 PICK 3RD @               ( a x y op )
   DUP '+ = IF DROP + ELSE DUP '- = IF DROP - ELSE DROP * THEN THEN
   'LIT D, D,
   3RD 2ND ;

: STEP ( a -- a' )            \ copy one instruction verbatim
   DUP ISIZE 0 DO DUP I CELLS + @ D, LOOP
   DUP ISIZE CELLS + ;

: PEEP ( start end -- newend )
   OVER PEEPBASE !
   2DUP SWAP - 1 CELLS / PEEPMAX > IF NIP EXIT THEN    \ too big: leave alone
   CLEAR-TGT
   2DUP MARK-TARGETS
   PEEPDST DP2 !
   OVER                               ( start end a )
   BEGIN 2DUP > WHILE
      \ Record where this source instruction will END UP -- its address
      \ once the rebuilt body has been copied back over the original,
      \ NOT its address inside the scratch buffer. Storing the scratch
      \ address instead leaves every branch pointing into PEEPDST, which
      \ still holds a copy of the code and so appears to work until the
      \ next definition is optimized over the top of it.
      DUP PEEPBASE @ - 1 CELLS / CELLS PEEPMAP +      ( a mapslot )
      DP2 @ PEEPDST - PEEPBASE @ +                    ( a mapslot final )
      SWAP !
      DUP FOLD? IF FOLD 2 PEEPCUT +!
      ELSE DUP 'DUP 'DROP CANCELS? IF 2ND 2ND 2 PEEPCUT +!
      ELSE DUP 'SWAP 'SWAP CANCELS? IF 2ND 2ND 2 PEEPCUT +!
      ELSE DUP '>R 'R> CANCELS? IF 2ND 2ND 2 PEEPCUT +!
      ELSE STEP
      THEN THEN THEN THEN
   REPEAT DROP                        ( start end )
   \ rewrite branch operands through the map
   DROP PEEPDST DP2 @                 ( start dst dstend )
   ROT >R                             ( dst dstend ) ( R: start )
   OVER SWAP                          ( dst dst dstend )
   SWAP                               ( dst dstend dst )
   BEGIN 2DUP > WHILE
      DUP @ BRANCHER? IF
         DUP 1 CELLS + DUP @          ( .. opaddr target )
         R@ - 1 CELLS /               ( .. opaddr srcoff )
         CELLS PEEPMAP + @            ( .. opaddr newaddr )
         SWAP !
      THEN
      DUP ISIZE CELLS +
   REPEAT DROP DROP                   ( dst )
   R> SWAP                            ( start dst )
   \ copy back; it can only have shrunk
   DP2 @ PEEPDST -                    ( start dst len )
   ROT SWAP                           ( dst start len )
   2DUP + >R                          ( dst start len ) ( R: newend )
   \ MOVE is ( src dst u ), and the stack is already in that order:
   \ copy the rebuilt body back over the original. Getting this
   \ backwards leaves the ORIGINAL code in place while HERE still moves
   \ down, so every definition is silently truncated -- which looks
   \ like success on any test whose word did not need its tail.
   MOVE
   R> ;

\ Optimize the definition just compiled.
: OPT ( -- )
   LATEST >BODY HERE PEEP
   HERE - ALLOT ;

: .PEEP  ." peep: removed " PEEPCUT @ . ." instructions" CR ;

\ -- optimizing every definition ---------------------------------------
\ With PEEP-AUTO set, ; optimizes what it just finished compiling. This
\ exists so the Forth-2012 CORE suite can be run against optimized code:
\ a peephole that is only ever exercised by its own tests is a peephole
\ nobody trusts, and the standard suite does not know it is being used
\ that way.
\
\ Redefining ; works because the new definition is hidden while it is
\ being compiled, so the ; that ends it finds the old one -- the same
\ property that makes RECURSE necessary.

VARIABLE PEEP-AUTO   0 PEEP-AUTO !
' ; CONSTANT 'SEMI
: ;  'SEMI EXECUTE  PEEP-AUTO @ IF OPT THEN ; IMMEDIATE
