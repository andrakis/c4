\ The program in src/c4th/tests/fact.c, hand-assembled with asm.f.
\
\ Written against C4's calling convention: arguments are pushed by the
\ caller and addressed upward from bp, so the single parameter n is LEA 2
\ (bp+0 is the saved bp and bp+1 the return pc). The caller cleans up with
\ ADJ after the call, and a function's value comes back in the accumulator.
\
\ The listing this produces is compared against c4cc's for the same source,
\ so the two code generators are checked against each other rather than
\ against anything written here.

ASM-RESET
ASM-HERE                      \ fact's entry address, kept on the stack

\ fact ( n -- n! )
0 ENT,
2 LEA, LI, PSH, 2 IMM, LT,    \ n < 2 ?
0 BZ, >MARK                   \ ( fact mark ) -- forward to the recursion
  1 IMM, LEV,                 \ ... then return 1
>RESOLVE                      \ ( fact )
2 LEA, LI, PSH,               \ n *
2 LEA, LI, PSH, 1 IMM, SUB, PSH,   \ fact(n - 1)
DUP JSR, 1 ADJ,
MUL, LEV,

\ main ( -- fact(10) )
0 ENT,
10 IMM, PSH,
DUP JSR, 1 ADJ,
LEV,

DROP
DIS
