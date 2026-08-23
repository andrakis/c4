\ Native versus threaded, on the same loop, under c4m where CYCLES is a
\ real counter:
\   ./c4m load-c4r.c -- c4th.c4r src/c4th/forth/core.f \
\       src/c4th/forth/asm.f src/c4th/forth/native.f src/c4th/bench/native.f

: BENCH ( -- n )  0 BEGIN 1+ DUP 100000 < WHILE REPEAT ;
LATEST >BODY HERE                       ( body end )

: REPORT ( body end -- )
   CYCLES BENCH DROP CYCLES SWAP -
   ." threaded: " . ." cycles" CR
   NCOMPILE
   IF   CYCLES ASMBUF INVOKE DROP CYCLES SWAP -
        ." native  : " . ." cycles" CR
        ." emitted : " ASM-LEN 1 CELLS / . ." instructions" CR
   ELSE ." native  : declined" CR THEN ;
REPORT
