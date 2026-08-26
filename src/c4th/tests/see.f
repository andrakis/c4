\ SEE: what a Forth definition compiles to as C4 VM code.
\
\ The accumulator model is visible in the listing and worth reading once.
\ DROP is `IMM 0; ADD` -- a = *sp++ + 0 -- because C4's ALU already pops
\ the stack, so dropping needs no opcode of its own. And a flag is
\ `LT; PSH; IMM -1; MUL`, because Forth wants -1 for true where C4's
\ comparisons give 1.

: SQ    DUP * ;
: ABS3  DUP 0< IF NEGATE THEN ;

." : SQ DUP * ;" CR
1 SEE SQ
." : ABS3 DUP 0< IF NEGATE THEN ;" CR
1 SEE ABS3
