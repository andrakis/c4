\ B5c: what counted loops, inlined calls and stack shuffling cost, both
\ ways, on the same work. Run under c4m, where CYCLES is a real counter:
\
\   ./c4m load-c4r.c -- c4th.c4r src/c4th/forth/core.f \
\       src/c4th/forth/asm.f src/c4th/forth/native.f src/c4th/bench/b5c.f

VARIABLE ACC   VARIABLE TH   VARIABLE NA
: TIMED ( xt -- )                       \ threaded cycles for one call
   CYCLES SWAP EXECUTE DROP CYCLES SWAP - TH ! ;
: NATIVE ( xt -- )                      \ compile it and time the result
   >BODY DUP BODY-END NCOMPILE
   IF CYCLES ASMBUF INVOKE DROP CYCLES SWAP - NA ! ELSE -1 NA ! THEN ;
: SHOW ( -- )
   ." threaded " TH @ .
   NA @ 0< IF ." native declined" ELSE
      ." native " NA @ .
      ." x" TH @ NA @ / .
      ." ( " ASM-LEN 1 CELLS / . ." instructions )" THEN CR ;

\ 1. a counted loop -- what DO/LOOP and I cost
: L1 0 100000 0 DO I + LOOP ;
." DO LOOP I            " ' L1 TIMED ' L1 NATIVE SHOW

\ 2. the same shape written with BEGIN/WHILE and the return stack
: L2 0 0 BEGIN DUP 100000 < WHILE DUP >R + R> 1+ REPEAT DROP ;
." BEGIN WHILE >R R>    " ' L2 TIMED ' L2 NATIVE SHOW

\ 3. a loop that calls another word every iteration
: SQ DUP * ;
: L3 0 20000 0 DO I SQ + 1000000 MOD LOOP ;
." loop with a call     " ' L3 TIMED ' L3 NATIVE SHOW

\ 4. a loop that reorders the stack every iteration
: L4 0 1 20000 0 DO OVER OVER + ROT DROP SWAP LOOP DROP ;
." loop with a shuffle  " ' L4 TIMED ' L4 NATIVE SHOW

\ 5. a loop through memory
: L5 0 ACC ! 100000 0 DO ACC @ I + ACC ! LOOP ACC @ ;
." loop through a VARIABLE " ' L5 TIMED ' L5 NATIVE SHOW
