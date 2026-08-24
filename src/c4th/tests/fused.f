\ Every fused opcode, checked against the sequence it replaces.
\
\ This is a VM-level test, not a compiler one. Each opcode is
\ hand-assembled into a tiny C4 function, the sequence it is supposed to
\ be equivalent to is assembled into another, and both are called with
\ the same arguments; they must agree. c4th's assembler is the natural
\ place to write it, because it is the only thing in the tree that can
\ lay down an arbitrary instruction and then call it.
\
\ Run under c4m -- INVOKE2 is a call into generated code, and the gcc
\ build has no VM to call into. See docs/fused-opcodes.md.
\
\ Argument positions: INVOKE2 calls f(a, b), so C4 pushes a then b, JSR
\ pushes the return address and ENT the old bp -- which puts b at bp+2
\ and a at bp+3.

CREATE BUF2 4096 ALLOT           \ a second buffer, for the unfused twin
VARIABLE B2P
: B2, ( n -- ) B2P @ ! 1 CELLS B2P +! ;
: PAIR  ASM-RESET  BUF2 B2P ! ;

VARIABLE FAILS   0 FAILS !
: SAME ( x y addr len -- )
   TYPE SPACE
   2DUP = IF 2DROP ." ok" ELSE ." MISMATCH " SWAP . . 1 FAILS +! THEN CR ;
: SAMEOP ( x y op -- )
   .OPNAME SPACE
   2DUP = IF 2DROP ." ok" ELSE ." MISMATCH " SWAP . . 1 FAILS +! THEN CR ;
: RUN2 ( a b -- x y )  2DUP ASMBUF INVOKE2 >R  BUF2 INVOKE2  R> SWAP ;

\ Two scratch cells, so the global forms have something real to read.
CREATE G1 1 CELLS ALLOT   1234567 G1 !
CREATE G2 1 CELLS ALLOT       -89 G2 !

\ -- LDL: a = *(bp+n) --------------------------------------------------
PAIR
   0 ENT,   3 LDL,   LEV,
   #ENT B2, 0 B2,   #LEA B2, 3 B2, #LI B2,   #LEV B2,
11 22 RUN2 S" LDL " SAME

\ -- LDG: a = *(int *)n ------------------------------------------------
PAIR
   0 ENT,   G1 LDG,   LEV,
   #ENT B2, 0 B2,   #IMM B2, G1 B2, #LI B2,   #LEV B2,
11 22 RUN2 S" LDG " SAME

\ -- PSHL: load a local, push it, and keep it in the accumulator -------
PAIR
   0 ENT,   3 PSHL,  2 LDL,  ADD,  LEV,
   #ENT B2, 0 B2,   #LEA B2, 3 B2, #LI B2, #PSH B2,
                    #LEA B2, 2 B2, #LI B2, #ADD B2,   #LEV B2,
11 22 RUN2 S" PSHL" SAME

\ -- PSHG --------------------------------------------------------------
PAIR
   0 ENT,   G1 PSHG,  G2 LDG,  ADD,  LEV,
   #ENT B2, 0 B2,   #IMM B2, G1 B2, #LI B2, #PSH B2,
                    #IMM B2, G2 B2, #LI B2, #ADD B2,   #LEV B2,
11 22 RUN2 S" PSHG" SAME

\ -- LEAP: push bp+n ---------------------------------------------------
\ The address depends on where the stack landed, so both sides
\ dereference it rather than returning it.
PAIR
   0 ENT,   3 LEAP,  0 IMM,  ADDL,  LEV,
   #ENT B2, 0 B2,   #LEA B2, 3 B2, #PSH B2,
                    #IMM B2, 0 B2, #ADD B2, #LI B2,   #LEV B2,
11 22 RUN2 S" LEAP" SAME

\ -- IMMP: push an immediate ------------------------------------------
PAIR
   0 ENT,   77 IMMP,  3 LDL,  ADD,  LEV,
   #ENT B2, 0 B2,   #IMM B2, 77 B2, #PSH B2,
                    #LEA B2, 3 B2, #LI B2, #ADD B2,   #LEV B2,
11 22 RUN2 S" IMMP" SAME

\ -- LIP: a = *a, then push it ----------------------------------------
PAIR
   0 ENT,   G1 IMM,  LIP,  G2 LDG,  ADD,  LEV,
   #ENT B2, 0 B2,   #IMM B2, G1 B2, #LI B2, #PSH B2,
                    #IMM B2, G2 B2, #LI B2, #ADD B2,   #LEV B2,
11 22 RUN2 S" LIP " SAME

\ -- ADDL: a = *(*sp++ + a) -------------------------------------------
PAIR
   0 ENT,   G1 IMM,  PSH,  0 IMM,  ADDL,  LEV,
   #ENT B2, 0 B2,   #IMM B2, G1 B2, #PSH B2, #IMM B2, 0 B2,
                    #ADD B2, #LI B2,   #LEV B2,
11 22 RUN2 S" ADDL" SAME

\ -- STL: *(bp+n) = a --------------------------------------------------
PAIR
   1 ENT,   99 IMM,  -1 STL,  -1 LDL,  LEV,
   #ENT B2, 1 B2,   #LEA B2, -1 B2, #PSH B2, #IMM B2, 99 B2, #SI B2,
                    #LEA B2, -1 B2, #LI B2,   #LEV B2,
11 22 RUN2 S" STL " SAME

\ -- POPA: a = *sp++ ---------------------------------------------------
PAIR
   0 ENT,   5 IMM,  PSH,  POPA,  LEV,
   #ENT B2, 0 B2,   #IMM B2, 5 B2, #PSH B2, #IMM B2, 0 B2, #ADD B2,
   #LEV B2,
11 22 RUN2 S" POPA" SAME

\ The immediate-ALU family is deliberately absent. It was measured at
\ 1.0% of c4cc's executed instructions and 5.2% of c4sp's -- sixteen
\ opcodes, and sixteen microcode routines on c4bb, for that. The ten
\ above are 35.1% and 32.3% on their own. docs/fused-opcodes.md.

." fused: " FAILS @ . ." mismatches" CR
