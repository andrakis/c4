\ c4th trt.f -- a runtime for generated programs.
\
\ Everything a .c4r image needs beyond the code generator itself, written
\ in Forth so the backend can compile it. That is the point rather than a
\ convenience: c4th's own `.` and `TYPE` are C functions operating on a C
\ data stack, and a standalone image has neither -- so the way past them
\ is not to call them but to write them again in a language the compiler
\ can see. See docs/c4th-design.md, B5d.
\
\ Load after c4r.f. Names end in T so they sit beside the host's own.

24 TBUFFER: NBUFT
TVARIABLE NBPT

: CRT    ( -- )  10 EMIT ;
: SPACET ( -- )  32 EMIT ;

\ Digits come out backwards, so they go into a buffer and come back
\ forwards. No recursion: an inliner has no bottom for it.
: U.T ( u -- )
   NBUFT 24 + NBPT !
   BEGIN
      -1 NBPT +!
      DUP 10 MOD 48 + NBPT @ C!
      10 /
      DUP 0=
   UNTIL DROP
   NBPT @
   BEGIN DUP NBUFT 24 + < WHILE DUP C@ EMIT 1+ REPEAT
   DROP ;

: .T ( n -- )  DUP 0< IF 45 EMIT NEGATE THEN U.T SPACET ;
