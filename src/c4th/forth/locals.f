\ c4th locals.f -- named arguments and locals, Forth-2012 syntax.
\
\   : ATOI {: a u -- n :}  0  u 0 ?DO 10 * a I + C@ 48 - + LOOP ;
\
\ This is the word that makes a compiler writable. self.f has twenty-one
\ VARIABLEs whose only job is to hold a value across three lines because
\ there was nowhere else to put it -- TQ PQ NQ EQ HQ HK NA NN2 STA STN
\ STO WI TA TN and the rest. Every one of them is a local that could not
\ be spelled, and every one of them is also a re-entrancy bug waiting for
\ the day the word is called recursively. A parser is recursive.
\
\ HOW IT WORKS, in three parts and no magic.
\
\ 1. Names. {: parses the names into a compile-time table. They are NOT
\    dictionary entries -- c4th's dictionary and its code share one
\    space, so a header created here would land in the middle of the
\    body being compiled. Instead they are found through NOTFOUND, the
\    outer interpreter's one extension point: a word that is neither
\    defined nor a number is offered to a handler before the error. A
\    local name arrives there and compiles a frame access.
\
\ 2. The frame. Locals live on a stack of their own, not on the return
\    stack, because c4th's DO/LOOP keeps its parameters there and I
\    inside a loop would read the wrong thing. Each frame stores the
\    previous frame pointer, so recursion works.
\
\ 3. The end. ; and EXIT are redefined to drop the frame first. That is
\    safe rather than clever: c4th hides a definition while compiling
\    it, so the ; that ends the new ; finds the old one.

\ -- the frame, at run time --------------------------------------------

512 CONSTANT LSMAX
CREATE LSTK LSMAX CELLS ALLOT
VARIABLE LFP   VARIABLE LSP
0 LFP !  1 LSP !

: LA ( i -- a )  CELLS LSTK + ;

: (LFRAME) ( x1..xn n m -- )            \ n from the stack, m slots in all
   LSP @ >R
   LFP @ R@ LA !                        \ the frame remembers its parent
   R@ 1+ + LSP !
   R@ LFP !
   BEGIN DUP 0> WHILE
      1-  DUP LFP @ + 1+ LA  SWAP >R ! R>
   REPEAT
   DROP R> DROP ;

: (LDROP)  ( -- )   LFP @ LSP !  LFP @ LA @ LFP ! ;
: (LOCAL@) ( i -- x )    LFP @ + 1+ LA @ ;
: (LOCAL!) ( x i -- )    LFP @ + 1+ LA ! ;

\ -- the names, at compile time ----------------------------------------

32 CONSTANT LMAX
CREATE LNBUF 1024 ALLOT
CREATE LNOFF LMAX CELLS ALLOT
CREATE LNLEN LMAX CELLS ALLOT
VARIABLE LN     VARIABLE LNB
VARIABLE LARGS  VARIABLE LON
VARIABLE LPH    VARIABLE LDONE

: BYTES= ( a1 a2 u -- f )
   0 ?DO
      OVER C@ OVER C@ <> IF 2DROP 0 UNLOOP EXIT THEN
      1+ SWAP 1+ SWAP
   LOOP 2DROP -1 ;
: SAME? ( a1 u1 a2 u2 -- f )
   ROT OVER <> IF DROP 2DROP 0 EXIT THEN
   BYTES= ;

: LNAME ( i -- a u )  DUP CELLS LNOFF + @ LNBUF +  SWAP CELLS LNLEN + @ ;
: LADD  ( a u -- )
   LN @ LMAX < 0= IF ." locals.f: too many locals" CR ABORT THEN
   LNB @ LN @ CELLS LNOFF + !
   DUP LN @ CELLS LNLEN + !
   >R LNBUF LNB @ + R@ MOVE
   R> LNB +!
   1 LN +! ;
: LFIND ( a u -- i | -1 )
   LON @ 0= IF 2DROP -1 EXIT THEN
   LN @ 0 ?DO 2DUP I LNAME SAME? IF 2DROP I UNLOOP EXIT THEN LOOP
   2DROP -1 ;

\ The handler. Chaining is deliberate: a later layer can save this one
\ and add syntax of its own without editing this file.
VARIABLE LPREV
: L-NOTFOUND ( a u -- f )
   2DUP LFIND DUP 0< 0= IF
      >R 2DROP R> POSTPONE LITERAL POSTPONE (LOCAL@) -1 EXIT
   THEN DROP
   LPREV @ ?DUP IF EXECUTE ELSE 2DROP 0 THEN ;

\ -- the syntax ---------------------------------------------------------

: {:
   0 LN ! 0 LNB ! 1 LON ! 0 LARGS ! 0 LPH ! 0 LDONE !
   BEGIN
      BL WORD COUNT DUP 0= IF 2DROP ." locals.f: {: without :}" CR ABORT THEN
      2DUP S" :}" SAME? IF 1 LDONE ! THEN
      LDONE @ 0= IF
         2DUP S" |"  SAME? IF 1 LPH ! ELSE
         2DUP S" --" SAME? IF 2 LPH ! ELSE
            LPH @ 2 < IF 2DUP LADD  LPH @ 0= IF 1 LARGS +! THEN THEN
         THEN THEN
      THEN
      2DROP
      LDONE @
   UNTIL
   LARGS @ POSTPONE LITERAL
   LN @ POSTPONE LITERAL
   POSTPONE (LFRAME) ; IMMEDIATE

' ; CONSTANT (OLD;)
: ;  LON @ IF POSTPONE (LDROP) 0 LON ! 0 LN ! THEN (OLD;) EXECUTE ; IMMEDIATE

' EXIT CONSTANT (OLD-EXIT)
: EXIT  LON @ IF POSTPONE (LDROP) THEN (OLD-EXIT) , ; IMMEDIATE

: TO ( "name" -- )
   BL WORD DUP COUNT LFIND DUP 0< 0= IF
      NIP
      STATE @ IF POSTPONE LITERAL POSTPONE (LOCAL!)
              ELSE DROP ." locals.f: TO a local outside a definition" CR THEN
   ELSE
      DROP FIND 0= IF ." locals.f: TO: not found" CR ABORT THEN
      >BODY STATE @ IF POSTPONE LITERAL POSTPONE ! ELSE ! THEN
   THEN ; IMMEDIATE

NOTFOUND @ LPREV !
' L-NOTFOUND NOTFOUND !
