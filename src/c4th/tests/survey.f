\ Which words does the native backend decline, and what stops it?
\
\ This is the measurement that decides where the backend's remaining work
\ is -- and, at B5b, whether c4m needed new stack opcodes. It walks
\ core.f's colon definitions, tries to compile each one, and names the
\ word that stopped it.
\
\ core.f only: the words asm.f and native.f define are the compiler's own
\ scaffolding and would swamp the count. CMOVE is core.f's last
\ definition, and the dictionary is a chain from newest to oldest, so
\ starting there surveys core.f and then the primitives, which are not
\ colon words and are skipped.

: >CODE ( xt -- a )  5 CELLS + @ ;
: >NLEN ( xt -- n )  3 CELLS + @ ;
: >NAME ( xt -- a )  2 CELLS + @ ;
: .WNAME ( xt -- )   DUP >NAME SWAP >NLEN TYPE ;
: PROBE ;
' PROBE >CODE CONSTANT COLONS     \ what a colon definition's code field holds

\ Nothing in Forth declares how many arguments a definition takes, so
\ NCOMPILE? asks the compiler: it tries each arity in turn and the
\ fewest that compiles without underflowing is the answer.

VARIABLE NTRIED  VARIABLE NDONE
: SURVEY ( xt -- )
   0 NTRIED !  0 NDONE !
   BEGIN DUP WHILE
      DUP >CODE COLONS = IF
         DUP >BODY DUP BODY-END NCOMPILE?
         1 NTRIED +!
         DUP 0 >= IF 1 NDONE +!
              ."   compiled: " OVER .WNAME ."  ( " . ." in )" CR
         ELSE DROP ."   declined: " DUP .WNAME
              ."   stopped by: " NBAD @ ?DUP IF .WNAME ELSE ." (nothing)" THEN CR
         THEN
      THEN
      @                                 \ W_LINK is the first cell
   REPEAT DROP
   ." compiled " NDONE @ . ." of " NTRIED @ . ." colon words" CR ;
' CMOVE SURVEY
