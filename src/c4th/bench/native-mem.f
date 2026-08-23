\ A memory-touching loop, now that VARIABLE, ! and @ compile.
VARIABLE S
: BENCH2 ( -- n )  0 S !  0 BEGIN 1+ DUP S @ + S ! DUP 100000 < WHILE REPEAT DROP S @ ;
LATEST >BODY HERE
: REPORT ( body end -- )
   CYCLES BENCH2 DROP CYCLES SWAP - ." threaded: " . ." cycles" CR
   NCOMPILE
   IF CYCLES ASMBUF INVOKE DROP CYCLES SWAP - ." native  : " . ." cycles" CR
      ." emitted : " ASM-LEN 1 CELLS / . ." instructions" CR
   ELSE ." native  : declined" CR THEN ;
REPORT
