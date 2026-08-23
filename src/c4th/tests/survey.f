\ Which words does the native backend decline, and what stops it?
\
\ This is the measurement that decides whether c4m needs new opcodes: if
\ most refusals are stack reordering, opcodes would pay; if they are calls
\ or loop control, they would not, and the work belongs in the compiler.

: >CODE ( xt -- a )  5 CELLS + @ ;
: >NLEN ( xt -- n )  3 CELLS + @ ;
: >NAME ( xt -- a )  2 CELLS + @ ;
: PROBE ;
' PROBE >CODE CONSTANT COLONS     \ what a colon definition's code field holds

\ A definition ends at its EXIT; nothing in core.f uses EXIT mid-word.
: BODY-END ( body -- end )
   BEGIN DUP @ nEXIT = 0= WHILE
      DUP @ DUP nLIT = SWAP DUP n0BRANCH = SWAP nBRANCH = OR OR
      IF 2 CELLS + ELSE 1 CELLS + THEN
   REPEAT 1 CELLS + ;

VARIABLE NTRIED  VARIABLE NDONE
: SURVEY ( -- )
   0 NTRIED !  0 NDONE !
   LATEST
   BEGIN DUP WHILE
      DUP >CODE COLONS = IF
         DUP >BODY DUP BODY-END NCOMPILE
         1 NTRIED +!
         IF 1 NDONE +!
         ELSE ."   declined: " DUP DUP >NAME SWAP >NLEN TYPE
              ."   stopped by: " NBAD @ ?DUP IF DUP >NAME SWAP >NLEN TYPE ELSE ." (end)" THEN CR
         THEN
      THEN
      @                                 \ W_LINK is the first cell
   REPEAT DROP
   ." compiled " NDONE @ . ." of " NTRIED @ . ." colon words" CR ;
SURVEY
