\ c4th core.f -- the part of Forth-2012 CORE that is written in Forth.
\
\ Everything here is built from the primitives and from IMMEDIATE, which
\ is the point: control structures are ordinary definitions that run at
\ compile time and lay down branches, not syntax wired into the C.

\ -- conditionals ------------------------------------------------------
\ IF leaves the address of the branch operand for THEN to fill in. The
\ operand cell is written as 0 and patched later; nothing ever reads it
\ before then.

: IF     POSTPONE 0BRANCH HERE 0 , ; IMMEDIATE
: THEN   HERE SWAP ! ; IMMEDIATE
: ELSE   POSTPONE BRANCH HERE 0 ,  SWAP  HERE SWAP ! ; IMMEDIATE

\ -- indefinite loops --------------------------------------------------

: BEGIN  HERE ; IMMEDIATE
: AGAIN  POSTPONE BRANCH , ; IMMEDIATE
: UNTIL  POSTPONE 0BRANCH , ; IMMEDIATE
\ WHILE leaves ( orig dest ) -- the SWAP is load-bearing and belongs
\ HERE, not in REPEAT. It keeps BEGIN's destination on top, so a second
\ WHILE can stack another orig underneath it and REPEAT still finds the
\ pair it needs. Put the SWAP in REPEAT instead and the single-WHILE case
\ still works while BEGIN .. WHILE .. WHILE .. REPEAT .. ELSE .. THEN
\ compiles a branch to the wrong address -- which is a segfault, not a
\ failed test. The Forth-2012 suite's GI5 is exactly that shape.
: WHILE  POSTPONE 0BRANCH HERE 0 , SWAP ; IMMEDIATE
: REPEAT POSTPONE BRANCH , HERE SWAP ! ; IMMEDIATE

\ -- defining words ----------------------------------------------------

: VARIABLE  CREATE 1 CELLS ALLOT ;
: CONSTANT  CREATE , DOES> @ ;

\ -- counted loops -----------------------------------------------------
\ LEAVE needs somewhere to record the branches it lays down, and there
\ may be several per loop, so they go on their own stack rather than on
\ the data stack where DO's loop-start address is already sitting. DO
\ records the depth it started at; LOOP resolves everything above it.

CREATE LVSTK 64 CELLS ALLOT
VARIABLE LVSP   0 LVSP !

: LV>    ( a -- )  LVSP @ CELLS LVSTK + !  1 LVSP +! ;
: >LV    ( -- a )  -1 LVSP +!  LVSP @ CELLS LVSTK + @ ;

: DO     POSTPONE (DO)  LVSP @  HERE ; IMMEDIATE
\ ?DO's skip is just another leave: if the limits are equal it branches to
\ wherever LOOP ends up, which is exactly what the leave stack resolves.
\ The depth is captured before that branch is recorded, so LOOP resolves
\ it along with any real LEAVEs. No UNLOOP on this path -- no loop was
\ ever entered.
: ?DO    LVSP @ >R
         POSTPONE 2DUP POSTPONE =
         POSTPONE 0BRANCH HERE 0 ,
         POSTPONE 2DROP
         POSTPONE BRANCH HERE 0 , LV>
         HERE SWAP !
         POSTPONE (DO) R> HERE ; IMMEDIATE
: LEAVE  POSTPONE UNLOOP POSTPONE BRANCH HERE 0 , LV> ; IMMEDIATE

: (RESOLVE-LEAVES)  ( depth -- )
         BEGIN  DUP LVSP @ <  WHILE  >LV HERE SWAP !  REPEAT  DROP ;

: LOOP   POSTPONE (LOOP) ,  (RESOLVE-LEAVES) ; IMMEDIATE
: +LOOP  POSTPONE (+LOOP) , (RESOLVE-LEAVES) ; IMMEDIATE

\ -- scaling -----------------------------------------------------------
\ */ and */MOD keep the intermediate product double wide, which is the
\ whole reason they exist: 3 * MAX-INT / MAX-INT is 3, not an overflow.

: */MOD  ( a b c -- rem quot )  >R M* R> SM/REM ;
: */     ( a b c -- q )  */MOD SWAP DROP ;

\ -- shorthands --------------------------------------------------------

: SPACES ( n -- )  0 MAX  0 ?DO SPACE LOOP ;
: ?      ( a -- )  @ . ;
: WITHIN ( n lo hi -- f )  OVER - >R - R> U< ;
: TRUE   -1 ;
: FALSE  0 ;
: NOT    0= ;
: 2!     ( x y a -- )  SWAP OVER ! CELL+ ! ;
: 2@     ( a -- x y )  DUP CELL+ @ SWAP @ ;
: ERASE  ( a n -- )  0 FILL ;
: CMOVE  ( s d n -- )  MOVE ;
