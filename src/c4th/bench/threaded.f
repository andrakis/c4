\ The same loop the two C models in this directory run: sum 0..99999.
\ Run it under c4m, where CYCLES is a real counter:
\   ./c4m load-c4r.c -- c4th.c4r src/c4th/forth/core.f src/c4th/bench/threaded.f
: BENCH ( n -- sum ) 0 SWAP 0 DO I + LOOP ;
CYCLES  100000 BENCH DROP  CYCLES SWAP -
." threaded cycles: " . CR
