\ c4th B5d: the metacompiler, checked against the interpreter.
\
\ The same source, run two ways. c4th runs MAIN on its own threaded
\ engine and prints; then it COMPILES MAIN into a standalone .c4r and the
\ Makefile runs that image on c4m, on c4mp, and on plain c4 -- and every
\ one of them must print the same thing. The interpreter is the oracle
\ here for the same reason it was at B5: it is the engine that passes the
\ Forth-2012 CORE suite.
\
\ Needs the native build, because writing the file needs a write syscall
\ and the C4 VM has none.

TVARIABLE ACC
TVARIABLE SEED

\ Never written at run time, so what it prints is what TSYNC baked into
\ the image's data segment -- which is the only thing here that proves
\ initialized data works at all.
TVARIABLE MAGIC   4242 MAGIC !

\ SEED is set by the program rather than at compile time, deliberately.
\ It was compile-time-initialized at first, and then the interpreter run
\ below mutated it before TSYNC copied it, so the image started from a
\ different seed and printed a different number. The differential caught
\ that on the first run; it is exactly the order dependence that makes a
\ compiler's output diverge from its interpreter's.

\ A pseudo-random walk, so the arithmetic actually has to be right rather
\ than merely present: multiply, add, mask, unsigned compare, division.
: NEXTR ( -- u )
   SEED @ 1103515245 * 12345 +  DUP SEED !  1 RSHIFT  65535 AND ;

: SQUARES ( -- )
   10 1 DO I DUP * .T LOOP CRT ;

: ACCUM ( -- )
   0 ACC !
   20 0 DO ACC @ I + ACC ! LOOP
   ACC @ .T  CRT ;

: SIGNS ( -- )
   5 -3 DO I .T LOOP CRT
   -7 ABS .T  7 NEGATE .T  0 17 - .T  CRT ;

: PICKS ( -- )
   12345 SEED !
   0 8 0 DO NEXTR 10 MOD + LOOP .T
   17 5 /MOD .T .T  CRT ;

: BRANCHY ( -- )
   10 0 DO
      I 2 MOD 0= IF 48 I + EMIT ELSE 46 EMIT THEN
   LOOP CRT ;

: NESTED ( -- )
   0  4 0 DO 3 0 DO I J * + LOOP LOOP .T CRT ;

: MAIN ( -- n )
   SQUARES ACCUM SIGNS PICKS BRANCHY NESTED
   MAGIC @ .T CRT
   ACC @ 190 = IF 0 ELSE 1 THEN ;

MAIN DROP                               \ the oracle: run it interpreted
S" .c4th_b5d.c4r" ' MAIN TC4R           \ then compile it and write the image
